#include "hermos/hermos.hpp"
#include "hermos/hermos_multi_threaded_executor.hpp"
#include "hermos/hermos_mq.hpp"
#include "hermos/hermos_single_threaded_executor.hpp"
#include "hermos/node/hermos_only_executor.hpp"
#include "node/hermos_signal_handler.hpp"

#include <gtest/gtest.h>
#include <rclcpp/version.h>

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <sys/epoll.h>
#include <pthread.h>

namespace hermos
{
struct ExecutorOwnershipTestAccess
{
  static int try_ready_queue_lock(HermosExecutor & executor)
  {
    // Linux pthread trylock reports EBUSY without std::mutex's permitted spurious failure.
    auto * mutex = executor.ready_hermos_executables_mutex_.native_handle();
    const int result = pthread_mutex_trylock(mutex);
    if (result == 0) {
      EXPECT_EQ(pthread_mutex_unlock(mutex), 0);
    }
    return result;
  }
};
}  // namespace hermos

namespace
{
using namespace std::chrono_literals;

rclcpp::CallbackGroup::SharedPtr make_standalone_callback_group()
{
  // Jazzy requires a context argument; these handler tests need no ROS node.
#if RCLCPP_VERSION_MAJOR >= 28
  return std::make_shared<rclcpp::CallbackGroup>(
    rclcpp::CallbackGroupType::MutuallyExclusive, rclcpp::Context::WeakPtr{});
#else
  return std::make_shared<rclcpp::CallbackGroup>(
    rclcpp::CallbackGroupType::MutuallyExclusive);
#endif
}

template <typename Base>
class TestExecutor : public Base
{
  bool validate_callback_group(const rclcpp::CallbackGroup::SharedPtr & group) const override
  {
    if (during_prepare) {
      auto callback = std::move(during_prepare);
      callback();
    }
    return static_cast<bool>(group);
  }

public:
  using Base::prepare_epoll;
  using Base::execute_hermos_executable;
  mutable std::function<void()> during_prepare;
  std::atomic<bool> pause_next_snapshot{false};
  std::function<void()> after_snapshot;

  std::vector<rclcpp::CallbackGroup::WeakPtr> get_all_callback_groups() override
  {
    auto groups = Base::get_all_callback_groups();
    if (pause_next_snapshot.exchange(false)) {
      after_snapshot();
    }
    return groups;
  }

  void refresh()
  {
    if (this->epoll_update_tracker_.take_update_request()) {
      this->prepare_epoll();
    }
  }

  bool dispatch(int timeout_ms = 10)
  {
    hermos::HermosExecutable executable;
    if (!this->get_next_hermos_executable(executable, timeout_ms)) {
      return false;
    }
    this->execute_hermos_executable(executable);
    return true;
  }

  void collect_event() { this->epoll_manager_->wait_and_handle_epoll_event(1000); }

  void replace_handlers(hermos::EventHandlerArray sources)
  {
    this->epoll_manager_ = std::make_unique<hermos::EpollManager>(std::move(sources));
  }

  bool has_ready_work()
  {
    hermos::HermosExecutable executable;
    if (!this->get_next_hermos_executable(executable, 0)) {
      return false;
    }
    executable.callback_group->can_be_taken_from().store(true);
    return true;
  }
};

class PausedPreparation : public hermos::EpollEventHandler
{
public:
  explicit PausedPreparation(std::unique_ptr<hermos::EpollEventHandler> handler)
  : handler_(std::move(handler)), resume_(release.get_future()) {}

  std::promise<void> entered;
  std::promise<void> release;
  hermos::EpollEventType get_type() const override { return handler_->get_type(); }
  void handle(hermos::EpollEventLocalID id) override { handler_->handle(id); }
  void prepare_epoll(int fd, const hermos::CallbackGroupValidator & validator) override
  {
    if (first_.exchange(false)) {
      entered.set_value();
      resume_.wait();
    }
    handler_->prepare_epoll(fd, validator);
  }

private:
  std::unique_ptr<hermos::EpollEventHandler> handler_;
  std::future<void> resume_;
  std::atomic<bool> first_{true};
};

template <typename Handler>
void release_last_timer_owner_during_prepare()
{
  alarm(3);  // A regression must fail this child, never hang the test process.
  auto group = make_standalone_callback_group();
  auto clock = std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME);
  const auto id = hermos::next_timer_id.fetch_add(1);
  auto timer = std::make_shared<hermos::GenericTimer<std::function<void()>>>(
    id, 1ms, clock, [] {});
  std::weak_ptr<hermos::TimerBase> weak = timer;
  auto info = std::make_shared<hermos::TimerInfo>();
  info->timer_id = id;
  info->timer = timer;
  info->callback_group = group;
  {
    std::lock_guard<std::mutex> guard(hermos::id2_timer_info_mtx);
    hermos::id2_timer_info.emplace(id, info);
  }
  std::mutex ready_mutex;
  std::vector<hermos::HermosExecutable> ready;
  Handler handler(getpid(), &ready_mutex, &ready);
  std::thread release_owner;
  handler.prepare_epoll(-1, [&](const auto & candidate) {
    if (candidate != group) {return false;}
    const auto owners_before_release = weak.use_count();
    release_owner = std::thread([&] {timer.reset();});
    // If prepare acquired a temporary strong reference, make it the last one.
    // expired() introduces no owner, so destruction waits safely for unlock.
    while (weak.use_count() >= owners_before_release) {std::this_thread::yield();}
    return true;
  });
  release_owner.join();
  _exit(weak.expired() ? 0 : 2);
}

class SubscriptionNotification
{
public:
  SubscriptionNotification(const rclcpp::CallbackGroup::SharedPtr & group, const char * suffix,
    bool transient_local = false)
  : name_("/hermos_executor_test_" + std::to_string(getpid()) + suffix)
  {
    mq_attr attr{};
    attr.mq_maxmsg = 1;
    attr.mq_msgsize = sizeof(hermos::MqMsgHermos);
    fd_ = mq_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK, 0600, &attr);
    if (fd_ != -1) {
      id_ = hermos::register_callback<int>(
        [](const hermos::ipc_shared_ptr<int> &) {}, name_, 0, transient_local, fd_, group);
    }
  }

  ~SubscriptionNotification()
  {
    if (fd_ != -1) {
      unregister();
      mq_close(fd_);
      mq_unlink(name_.c_str());
    }
  }

  void unregister() { hermos::unregister_callback(id_); }

  uint32_t id() const { return id_; }

  hermos::CallbackInfo info() const
  {
    std::lock_guard<std::mutex> lock(hermos::id2_callback_info_mtx);
    return hermos::id2_callback_info.at(id_);
  }

  bool notify() const
  {
    hermos::MqMsgHermos message{};
    return fd_ != -1 &&
           mq_send(fd_, reinterpret_cast<const char *>(&message), sizeof(message), 0) == 0;
  }

  long pending() const
  {
    mq_attr attr{};
    return mq_getattr(fd_, &attr) == 0 ? attr.mq_curmsgs : -1;
  }

private:
  std::string name_;
  mqd_t fd_ = -1;
  uint32_t id_ = 0;
};

template <typename Executor>
class ExecutorOwnership : public ::testing::Test
{
protected:
  void SetUp() override { rclcpp::init(0, nullptr); }
  void TearDown() override { rclcpp::shutdown(); }
};

using ExecutorTypes = ::testing::Types<
  TestExecutor<hermos::SingleThreadedHermosExecutor>,
  TestExecutor<hermos::MultiThreadedHermosExecutor>>;
TYPED_TEST_SUITE(ExecutorOwnership, ExecutorTypes);

TYPED_TEST(ExecutorOwnership, BothExecutorsProcessTheirOwnTimer)
{
  auto node_a = std::make_shared<rclcpp::Node>("executor_owner_a");
  auto node_b = std::make_shared<rclcpp::Node>("executor_owner_b");
  auto clock = std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME);
  auto group_a = node_a->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  auto group_b = node_b->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  unsigned calls_a = 0;
  unsigned calls_b = 0;
  auto timer_a = hermos::create_timer(node_a, clock, 1ms, [&] { ++calls_a; }, group_a);
  auto timer_b = hermos::create_timer(node_b, clock, 1ms, [&] { ++calls_b; }, group_b);
  TypeParam executor_a;
  TypeParam executor_b;
  executor_a.add_node(node_a);
  executor_b.add_node(node_b);
  executor_a.prepare_epoll();
  executor_b.prepare_epoll();

  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while ((calls_a == 0 || calls_b == 0) && std::chrono::steady_clock::now() < deadline) {
    executor_a.dispatch();
    executor_b.dispatch();
  }
  EXPECT_GT(calls_a, 0u);
  EXPECT_GT(calls_b, 0u);
}

TYPED_TEST(ExecutorOwnership, DiscoversGroupCreatedAfterNodeWasAdded)
{
  auto node = std::make_shared<rclcpp::Node>("executor_late_group");
  TypeParam executor;
  executor.add_node(node);
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  auto clock = std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME);
  unsigned calls = 0;
  auto timer = hermos::create_timer(node, clock, 1ms, [&] { ++calls; }, group);
  executor.prepare_epoll();

  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (calls == 0 && std::chrono::steady_clock::now() < deadline) {
    executor.dispatch();
  }
  EXPECT_GT(calls, 0u);
}

TYPED_TEST(ExecutorOwnership, EachExecutorReceivesOnlyItsSubscriptionNotification)
{
  auto node_a = std::make_shared<rclcpp::Node>("executor_subscription_a");
  auto node_b = std::make_shared<rclcpp::Node>("executor_subscription_b");
  auto group_a = node_a->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  auto group_b = node_b->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  SubscriptionNotification notification_a(group_a, "_a");
  SubscriptionNotification notification_b(group_b, "_b");
  TypeParam executor_a;
  TypeParam executor_b;
  executor_a.add_node(node_a);
  executor_b.add_node(node_b);
  executor_a.prepare_epoll();
  executor_b.prepare_epoll();

  ASSERT_TRUE(notification_b.notify());
  EXPECT_FALSE(executor_a.has_ready_work());
  EXPECT_TRUE(executor_b.has_ready_work());
  ASSERT_TRUE(notification_a.notify());
  EXPECT_FALSE(executor_b.has_ready_work());
  EXPECT_TRUE(executor_a.has_ready_work());
}

TYPED_TEST(ExecutorOwnership, RemovedNodeDoesNotRetainReadyWork)
{
  auto node = std::make_shared<rclcpp::Node>("executor_removed_node");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  SubscriptionNotification notification(group, "_removed");
  TypeParam executor;
  executor.add_node(node);
  executor.prepare_epoll();
  ASSERT_TRUE(notification.notify());
  executor.collect_event();

  executor.remove_node(node);
  EXPECT_FALSE(executor.has_ready_work());
  executor.add_node(node);
  EXPECT_FALSE(executor.has_ready_work());
}

TYPED_TEST(ExecutorOwnership, NewOwnershipWorkSurvivesAnInFlightReadySnapshot)
{
  for (bool add_node : {false, true}) {
    EXPECT_EXIT({
      alarm(5);
      auto node = std::make_shared<rclcpp::Node>("executor_ready_snapshot");
      auto added = std::make_shared<rclcpp::Node>("executor_added_owner");
      auto busy = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
      SubscriptionNotification initial(busy, "_busy", true);
      TypeParam executor;
      executor.add_node(node);
      executor.prepare_epoll();
      busy->can_be_taken_from().store(false);

      std::promise<void> snapshot_taken;
      std::promise<void> release_snapshot;
      auto resume = release_snapshot.get_future();
      executor.after_snapshot = [&] {
        snapshot_taken.set_value();
        resume.wait();
      };
      executor.pause_next_snapshot.store(true);
      auto selecting = std::async(std::launch::async, [&] {return executor.has_ready_work();});
      snapshot_taken.get_future().wait();

      // Both explicit add_node and automatic discovery can add new ownership.
      auto group = (add_node ? added : node)->create_callback_group(
        rclcpp::CallbackGroupType::MutuallyExclusive);
      SubscriptionNotification incoming(group, "_incoming", true);
      if (add_node) {executor.add_node(added);}
      const int queue_lock = hermos::ExecutorOwnershipTestAccess::try_ready_queue_lock(executor);
      std::future<void> prepare;
      if (queue_lock == 0) {
        // Force the old implementation to enqueue before its stale selection.
        executor.prepare_epoll();
      } else if (queue_lock == EBUSY) {
        // The fixed selector holds the queue lock until its snapshot is released.
        prepare = std::async(std::launch::async, [&] {executor.prepare_epoll();});
      } else {
        _exit(3);
      }
      release_snapshot.set_value();
      const bool selected = selecting.get();
      if (prepare.valid()) {prepare.get();}
      _exit(selected || executor.has_ready_work() ? 0 : 2);
    }, ::testing::ExitedWithCode(0), "");
  }
}

TYPED_TEST(ExecutorOwnership, UnregisterDetachesRetainedNotificationFromEpoll)
{
  auto node = std::make_shared<rclcpp::Node>("executor_detached_notification");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  SubscriptionNotification notification(group, "_retained");
  TypeParam executor;
  executor.add_node(node);
  executor.prepare_epoll();
  const auto in_flight = notification.info();
  ASSERT_GE(in_flight.registered_epoll_fd, 0);
  notification.unregister();
  ASSERT_TRUE(notification.notify());
  EXPECT_FALSE(in_flight.active->load());
  epoll_event event{};
  EXPECT_EQ(epoll_wait(in_flight.registered_epoll_fd, &event, 1, 0), 0);
  EXPECT_EQ(notification.pending(), 1);
}

TEST(SubscriptionOwnership, StaleHandlerCannotConsumeNewOwnersNotification)
{
  auto group = make_standalone_callback_group();
  SubscriptionNotification notification(group, "_stale_handle");
  const int old_epoll = epoll_create1(EPOLL_CLOEXEC);
  const int new_epoll = epoll_create1(EPOLL_CLOEXEC);
  ASSERT_GE(old_epoll, 0);
  ASSERT_GE(new_epoll, 0);
  {
    std::mutex mutex;
    std::vector<hermos::HermosExecutable> old_ready, new_ready;
    hermos::SubscriptionEventHandler old_handler(getpid(), &mutex, &old_ready);
    hermos::SubscriptionEventHandler new_handler(getpid(), &mutex, &new_ready);
    old_handler.prepare_epoll(old_epoll, [](const auto &) {return true;});
    new_handler.prepare_epoll(new_epoll, [](const auto &) {return true;});
    new_ready.clear();  // Remove the transfer recovery receive.
    ASSERT_TRUE(notification.notify());
    old_handler.handle(notification.id());
    EXPECT_TRUE(old_ready.empty());
    EXPECT_EQ(notification.pending(), 1);
    new_handler.handle(notification.id());
    EXPECT_EQ(new_ready.size(), 1u);
    EXPECT_EQ(notification.pending(), 0);
  }
  close(old_epoll);
  close(new_epoll);
}

TYPED_TEST(ExecutorOwnership, TransferredSubscriptionCannotBeDrainedByPreviousExecutor)
{
  for (bool refresh_old_first : {false, true}) {
    auto node = std::make_shared<rclcpp::Node>("executor_transfer_subscription");
    auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    SubscriptionNotification notification(group, "_transfer");
    TypeParam old_executor;
    TypeParam new_executor;
    old_executor.add_node(node);
    old_executor.refresh();
    new_executor.refresh();
    old_executor.remove_node(node);
    new_executor.add_node(node);
    if (refresh_old_first) {
      old_executor.refresh();
    }
    new_executor.refresh();
    ASSERT_TRUE(notification.notify());
    EXPECT_FALSE(old_executor.has_ready_work());
    EXPECT_EQ(notification.pending(), 1);
    EXPECT_TRUE(new_executor.has_ready_work());
    // A transfer also schedules one receive for a notification the old executor
    // may already have consumed. Drain that work before checking the live MQ.
    new_executor.has_ready_work();
    EXPECT_EQ(notification.pending(), 0);
    old_executor.refresh();
    ASSERT_TRUE(notification.notify());
    EXPECT_FALSE(old_executor.has_ready_work());
    EXPECT_TRUE(new_executor.has_ready_work());
  }
}

TYPED_TEST(ExecutorOwnership, TransferredGroupRegistersTimerWithNewExecutor)
{
  for (bool refresh_old_first : {false, true}) {
    auto node = std::make_shared<rclcpp::Node>("executor_transfer_timer");
    auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive, false);
    auto clock = std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME);
    unsigned calls = 0;
    auto timer = hermos::create_timer(node, clock, 1ms, [&] { ++calls; }, group);
    TypeParam old_executor;
    TypeParam new_executor;
    old_executor.add_callback_group(group, node->get_node_base_interface());
    old_executor.refresh();
    new_executor.refresh();
    old_executor.remove_callback_group(group);
    new_executor.add_callback_group(group, node->get_node_base_interface());
    if (refresh_old_first) {
      old_executor.refresh();
    }
    new_executor.refresh();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (calls == 0 && std::chrono::steady_clock::now() < deadline) {
      EXPECT_FALSE(old_executor.has_ready_work());
      new_executor.dispatch();
    }
    EXPECT_GT(calls, 0u);
    old_executor.refresh();
    const auto before = calls;
    while (calls == before && std::chrono::steady_clock::now() < deadline) {
      EXPECT_FALSE(old_executor.has_ready_work());
      new_executor.dispatch();
    }
    EXPECT_GT(calls, before);
  }
}

TYPED_TEST(ExecutorOwnership, TransferredRosClockIsConsumedOnlyByNewExecutor)
{
  for (bool refresh_old_first : {false, true}) {
    auto node = std::make_shared<rclcpp::Node>("executor_transfer_clock");
    auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive, false);
    auto clock = std::make_shared<rclcpp::Clock>(RCL_ROS_TIME);
    ASSERT_EQ(rcl_enable_ros_time_override(clock->get_clock_handle()), RCL_RET_OK);
    ASSERT_EQ(rcl_set_ros_time_override(clock->get_clock_handle(), 1'000'000), RCL_RET_OK);
    unsigned calls = 0;
    auto timer = hermos::create_timer(node, clock, 1ms, [&] { ++calls; }, group);
    TypeParam old_executor;
    TypeParam new_executor;
    old_executor.add_callback_group(group, node->get_node_base_interface());
    old_executor.refresh();
    new_executor.refresh();
    old_executor.remove_callback_group(group);
    new_executor.add_callback_group(group, node->get_node_base_interface());
    if (refresh_old_first) {
      old_executor.refresh();
    }
    new_executor.refresh();
    ASSERT_EQ(rcl_set_ros_time_override(clock->get_clock_handle(), 2'000'000), RCL_RET_OK);
    EXPECT_FALSE(old_executor.has_ready_work());
    EXPECT_TRUE(new_executor.dispatch());
    EXPECT_EQ(calls, 1u);
    old_executor.refresh();
    ASSERT_EQ(rcl_set_ros_time_override(clock->get_clock_handle(), 3'000'000), RCL_RET_OK);
    EXPECT_FALSE(old_executor.has_ready_work());
    EXPECT_TRUE(new_executor.dispatch());
    EXPECT_EQ(calls, 2u);
  }
}

TYPED_TEST(ExecutorOwnership, DestroyedExecutorDoesNotLeaveSubscriptionBoundToClosedEpoll)
{
  auto node = std::make_shared<rclcpp::Node>("executor_destroyed_owner");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  SubscriptionNotification notification(group, "_destroyed");
  {
    TypeParam old_executor;
    old_executor.add_node(node);
    old_executor.refresh();
    ASSERT_TRUE(notification.notify());
    old_executor.collect_event();
    EXPECT_EQ(notification.pending(), 0);
  }
  TypeParam new_executor;
  new_executor.add_node(node);
  new_executor.refresh();
  EXPECT_TRUE(new_executor.has_ready_work());
  ASSERT_TRUE(notification.notify());
  EXPECT_TRUE(new_executor.has_ready_work());
  EXPECT_EQ(notification.pending(), 0);
}

TYPED_TEST(ExecutorOwnership, TransferredRosClockRecoversAnAlreadyConsumedEdge)
{
  auto node = std::make_shared<rclcpp::Node>("executor_queued_clock");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive, false);
  auto clock = std::make_shared<rclcpp::Clock>(RCL_ROS_TIME);
  ASSERT_EQ(rcl_enable_ros_time_override(clock->get_clock_handle()), RCL_RET_OK);
  ASSERT_EQ(rcl_set_ros_time_override(clock->get_clock_handle(), 1'000'000), RCL_RET_OK);
  unsigned calls = 0;
  auto timer = hermos::create_timer(node, clock, 1ms, [&] { ++calls; }, group);
  TypeParam old_executor;
  TypeParam new_executor;
  old_executor.add_callback_group(group, node->get_node_base_interface());
  old_executor.refresh();
  ASSERT_EQ(rcl_set_ros_time_override(clock->get_clock_handle(), 2'000'000), RCL_RET_OK);
  old_executor.collect_event();
  old_executor.remove_callback_group(group);
  new_executor.add_callback_group(group, node->get_node_base_interface());
  new_executor.refresh();
  EXPECT_FALSE(old_executor.has_ready_work());
  EXPECT_TRUE(new_executor.dispatch());
  EXPECT_EQ(calls, 1u);
}

TYPED_TEST(ExecutorOwnership, OwnershipChangeInvalidatesAnInFlightPreparationSnapshot)
{
  auto node = std::make_shared<rclcpp::Node>("executor_stale_snapshot");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  SubscriptionNotification notification(group, "_snapshot");
  TypeParam old_executor;
  TypeParam new_executor;
  old_executor.add_node(node);
  old_executor.refresh();
  old_executor.during_prepare = [&] {
    old_executor.remove_node(node);
    new_executor.add_node(node);
  };
  old_executor.prepare_epoll();
  ASSERT_TRUE(notification.notify());
  EXPECT_FALSE(old_executor.has_ready_work());
  EXPECT_EQ(notification.pending(), 1);
  new_executor.refresh();
  EXPECT_TRUE(new_executor.has_ready_work());
  new_executor.has_ready_work();
  EXPECT_EQ(notification.pending(), 0);
}

TYPED_TEST(ExecutorOwnership, ConcurrentPreparationLeavesOwnedTimerAttached)
{
  EXPECT_EXIT({
    alarm(5);
    auto node = std::make_shared<rclcpp::Node>("executor_parallel_prepare");
    auto added = std::make_shared<rclcpp::Node>("executor_parallel_added_node");
    auto clock = std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME);
    unsigned calls = 0;
    auto timer = hermos::create_timer(node, clock, 1ms, [&] {++calls;});
    TypeParam executor;
    executor.add_node(node);
    std::mutex ready_mutex;
    std::vector<hermos::HermosExecutable> ready;
    auto paused = std::make_unique<PausedPreparation>(
      std::make_unique<hermos::TimerEventHandler>(getpid(), &ready_mutex, &ready));
    auto * gate = paused.get();
    auto entered = gate->entered.get_future();
    hermos::EventHandlerArray sources;
    for (auto & source : sources) {source = std::make_unique<hermos::DummyEventHandler>();}
    sources[static_cast<size_t>(hermos::EpollEventType::Timer)] = std::move(paused);
    executor.replace_handlers(std::move(sources));

    auto old_prepare = std::async(std::launch::async, [&] {executor.prepare_epoll();});
    entered.wait();
    // The old pass has its ownership snapshot but has not entered the registry.
    executor.add_node(added);
    std::promise<void> new_worker_started;
    auto new_worker = new_worker_started.get_future();
    auto new_prepare = std::async(std::launch::async, [&] {
      new_worker_started.set_value();
      executor.refresh();
    });
    new_worker.wait();
    // Without executor serialization, the new pass finishes first and the stale
    // old pass subsequently detaches its timer. A serialized pass waits here.
    const auto pending = new_prepare.wait_for(100ms);
    gate->release.set_value();
    old_prepare.get();
    new_prepare.get();

    executor.collect_event();
    if (ready.size() != 1u) {_exit(2);}
    TypeParam::execute_hermos_executable(ready.front());
    _exit(pending == std::future_status::timeout && calls == 1u ? 0 : 3);
  }, ::testing::ExitedWithCode(0), "");
}

TYPED_TEST(ExecutorOwnership, TimerPrepareCannotDestroyLastOwnerUnderRegistryLock)
{
  EXPECT_EXIT(
    release_last_timer_owner_during_prepare<hermos::TimerEventHandler>(),
    ::testing::ExitedWithCode(0), "");
}

TYPED_TEST(ExecutorOwnership, ClockPrepareCannotDestroyLastOwnerUnderRegistryLock)
{
  EXPECT_EXIT(
    release_last_timer_owner_during_prepare<hermos::ClockEventHandler>(),
    ::testing::ExitedWithCode(0), "");
}

TYPED_TEST(ExecutorOwnership, ThrowingCallbackReleasesExclusiveGroup)
{
  auto node = std::make_shared<rclcpp::Node>("executor_throwing_callback");
  auto group = node->get_node_base_interface()->get_default_callback_group();
  group->can_be_taken_from().store(false);
  hermos::HermosExecutable executable{
    std::make_shared<std::function<void()>>([] { throw std::runtime_error("callback failed"); }),
    group};

  EXPECT_THROW(TypeParam::execute_hermos_executable(executable), std::runtime_error);
  EXPECT_TRUE(group->can_be_taken_from().load());
}

class TestHermosOnlyExecutor : public hermos::HermosOnlyExecutor
{
public:
  void spin() override {}
  using HermosOnlyExecutor::execute_hermos_executable;
  using HermosOnlyExecutor::get_next_ready_hermos_executable;
  void queue(const rclcpp::CallbackGroup::SharedPtr & group, std::function<void()> callback)
  {
    ready_hermos_executables_.push_back(
      {std::make_shared<std::function<void()>>(std::move(callback)), group});
  }
  size_t queued() const {return ready_hermos_executables_.size();}
};

class HermosOnlyOwnership : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
    hermos::SignalHandler::install();
  }
  void TearDown() override
  {
    hermos::SignalHandler::uninstall();
    rclcpp::shutdown();
  }
};

TEST_F(HermosOnlyOwnership, RemovedGroupDropsReadyWorkBeforeReattachment)
{
  auto node = std::make_shared<rclcpp::Node>("only_removed_group");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive, false);
  TestHermosOnlyExecutor executor;
  executor.add_callback_group(group, node->get_node_base_interface());
  executor.queue(group, [] {});
  executor.remove_callback_group(group);
  hermos::HermosExecutable work;
  EXPECT_FALSE(executor.get_next_ready_hermos_executable(work));
  EXPECT_EQ(executor.queued(), 0u);
  executor.add_callback_group(group, node->get_node_base_interface());
  EXPECT_FALSE(executor.get_next_ready_hermos_executable(work));
}

TEST_F(HermosOnlyOwnership, CallbackFailurePermitsNextCallbackFromExclusiveGroup)
{
  auto node = std::make_shared<rclcpp::Node>("only_throwing_callback");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive, false);
  TestHermosOnlyExecutor executor;
  executor.add_callback_group(group, node->get_node_base_interface());
  executor.queue(group, [] {throw std::runtime_error("callback failed");});
  hermos::HermosExecutable work;
  ASSERT_TRUE(executor.get_next_ready_hermos_executable(work));
  EXPECT_THROW(executor.execute_hermos_executable(work), std::runtime_error);
  EXPECT_TRUE(group->can_be_taken_from().load());
  unsigned calls = 0;
  executor.queue(group, [&] {++calls;});
  ASSERT_TRUE(executor.get_next_ready_hermos_executable(work));
  executor.execute_hermos_executable(work);
  EXPECT_EQ(calls, 1u);
}
}  // namespace
