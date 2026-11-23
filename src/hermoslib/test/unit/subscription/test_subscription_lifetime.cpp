#include "hermos/hermos_callback_info.hpp"
#include "hermos/hermos_executor.hpp"
#include "hermos/hermos_publisher.hpp"
#include "hermos/hermos_subscription.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <system_error>
#include <vector>

namespace
{
std::vector<std::string> events;
std::vector<int64_t> released_ids;
int receive_count = 0;
int driver_wake_fd = -1;
int bind_error = EINVAL;
constexpr char generation_token[] = "/0123456789abcdef0123456789abcdef-0000000000000001";
const char * returned_token = generation_token;
unsigned batch_size = 1;
bool fail_mapping = false;
bool call_again = false;
int messages[MAX_RECEIVE_NUM]{};
}  // namespace

extern "C" int ioctl(int fd, unsigned long request, ...)
{
  using namespace hermos;
  va_list args;
  va_start(args, request);
  void * raw = va_arg(args, void *);
  va_end(args);
  if (_IOC_TYPE(request) != 0xA6) {
    return static_cast<int>(syscall(SYS_ioctl, fd, request, raw));
  }
  if (request == HERMOS_ADD_SUBSCRIBER_CMD) {
    auto * out = static_cast<hermos::ioctl_add_subscriber_args *>(raw);
    out->ret_id = 7;
    std::strcpy(out->ret_mq_topic_name, returned_token);
  } else if (request == HERMOS_ADD_PUBLISHER_CMD) {
    auto * out = static_cast<ioctl_add_publisher_args *>(raw);
    out->ret_id = 8;
    std::strcpy(out->ret_mq_topic_name, returned_token);
  } else if (request == HERMOS_REMOVE_PUBLISHER_CMD) {
    const auto * args = static_cast<const ioctl_remove_publisher_args *>(raw);
    EXPECT_EQ(std::string(args->topic_name.ptr, args->topic_name.len), "/lifetime");
    EXPECT_EQ(args->publisher_id, 8);
    events.emplace_back("remove_publisher");
  } else if (request == HERMOS_OPEN_SUBSCRIBER_NOTIFY_FD_CMD) {
    auto * args = static_cast<ioctl_remove_subscriber_args *>(raw);
    EXPECT_EQ(std::string(args->topic_name.ptr, args->topic_name.len), "/lifetime");
    EXPECT_EQ(args->subscriber_id, 7);
    if (driver_wake_fd >= 0) {
      return driver_wake_fd;
    }
    errno = bind_error;
    return -1;
  } else if (request == HERMOS_RECEIVE_MSG_CMD) {
    ++receive_count;
    auto * out = static_cast<hermos::ioctl_receive_msg_args *>(raw);
    auto * mappings = reinterpret_cast<hermos::publisher_shm_info *>(out->pub_shm_info_addr);
    *out = {};
    if (fail_mapping) {
      mappings[0] = {};
      out->ret_pub_shm_num = 1;
    }
    out->ret_entry_num = batch_size;
    out->ret_call_again = call_again;
    for (unsigned i = 0; i < batch_size; ++i) {
      out->ret_entry_ids[i] = i;
      out->ret_entry_addrs[i] = reinterpret_cast<uint64_t>(&messages[i]);
    }
  } else if (request == HERMOS_TAKE_MSG_CMD) {
    auto * out = static_cast<hermos::ioctl_take_msg_args *>(raw);
    *out = {};
    out->ret_entry_id = 0;
    out->ret_addr = reinterpret_cast<uint64_t>(&messages[0]);
  } else if (request == HERMOS_RELEASE_SUB_REF_CMD) {
    events.emplace_back("release");
    released_ids.push_back(static_cast<hermos::ioctl_update_entry_args *>(raw)->entry_id);
  } else if (request == HERMOS_REMOVE_SUBSCRIBER_CMD) {
    events.emplace_back("remove");
  } else {
    ADD_FAILURE() << "Unexpected ioctl request " << request;
    return -1;
  }
  return 0;
}

namespace hermos
{
void validate_ld_preload() {}

void map_read_only_area(pid_t, uint64_t, uint64_t)
{
  if (fail_mapping) {
    throw std::bad_alloc();
  }
}

mqd_t open_mq_for_subscription(
  const std::string & token, topic_local_id_t, std::pair<mqd_t, std::string> & mq)
{
  EXPECT_EQ(token, generation_token);
  mq = {42, "lifetime_mock"};
  return mq.first;
}

void remove_mq(const std::pair<mqd_t, std::string> &) { events.emplace_back("close_mq"); }
}  // namespace hermos

namespace
{
std::vector<std::string> endpoint_events()
{
  std::vector<std::string> result;
  for (const auto & event : events) {
    if (event != "close_mq") {
      result.push_back(event);
    }
  }
  return result;
}

std::vector<int64_t> released_entries()
{
  auto result = released_ids;
  std::sort(result.begin(), result.end());
  return result;
}

class SubscriptionLifetime : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
    node = std::make_shared<rclcpp::Node>("subscription_lifetime");
    events.clear();
    released_ids.clear();
    receive_count = 0;
    driver_wake_fd = -1;
    bind_error = EINVAL;
    returned_token = generation_token;
    batch_size = 1;
    fail_mapping = false;
    call_again = false;
  }

  void TearDown() override
  {
    EXPECT_TRUE(hermos::id2_callback_info.empty());
    ready.clear();
    node.reset();
    rclcpp::shutdown();
  }

  template <typename Func>
  auto subscribe(Func && callback)
  {
    return std::make_shared<hermos::Subscription<int>>(
      node.get(), "/lifetime", rclcpp::QoS(10), std::forward<Func>(callback),
      hermos::SubscriptionOptions{}, hermos::SubscriptionRole::HermosOnly);
  }

  void receive()
  {
    ASSERT_EQ(hermos::id2_callback_info.size(), 1u);
    const auto id = hermos::id2_callback_info.begin()->first;
    const auto info = hermos::id2_callback_info.begin()->second;
    hermos::receive_and_execute_message(id, getpid(), info, ready_mutex, ready);
  }

  std::shared_ptr<rclcpp::Node> node;
  std::mutex ready_mutex;
  std::vector<hermos::HermosExecutable> ready;
};

TEST_F(SubscriptionLifetime, RetainedMessageKeepsKernelEndpointUntilLastRelease)
{
  hermos::ipc_shared_ptr<const int> held;
  auto sub = subscribe([&](hermos::ipc_shared_ptr<const int> msg) { held = std::move(msg); });
  receive();
  sub.reset();
  // Callback/MQ participation stops now, but the kernel endpoint (and its
  // graph/count/bridge-demand contribution) remains until the final RELEASE.
  EXPECT_EQ(events, (std::vector<std::string>{"close_mq"}));
  EXPECT_TRUE(hermos::id2_callback_info.empty());
  hermos::ipc_shared_ptr<const int> copy(held, held.get());
  held.reset();
  EXPECT_TRUE(endpoint_events().empty());
  copy.reset();
  EXPECT_EQ(endpoint_events(), (std::vector<std::string>{"release", "remove"}));
}

TEST_F(SubscriptionLifetime, UnregisteredPendingCallbackDoesNotReceive)
{
  auto sub = subscribe([](const auto &) {});
  const auto id = hermos::id2_callback_info.begin()->first;
  hermos::enqueue_receive_and_execute(
    id, getpid(), hermos::id2_callback_info.begin()->second, ready_mutex, ready);
  sub.reset();
  ASSERT_EQ(ready.size(), 1u);
  (*ready.front().callable)();
  EXPECT_EQ(receive_count, 0);
}

TEST_F(SubscriptionLifetime, SelfUnregisterSkipsRemainingCallbacksButReleasesBatch)
{
  batch_size = 3;
  call_again = true;
  unsigned callbacks = 0;
  std::shared_ptr<hermos::Subscription<int>> sub;
  sub = subscribe([&](const auto &) { ++callbacks; sub.reset(); });
  receive();
  EXPECT_EQ(callbacks, 1u);
  EXPECT_TRUE(ready.empty());
  EXPECT_EQ(released_entries(), (std::vector<int64_t>{0, 1, 2}));
  EXPECT_EQ(endpoint_events(), (std::vector<std::string>{"release", "release", "release", "remove"}));
}

TEST_F(SubscriptionLifetime, ThrowingCallbackReleasesEntireBatch)
{
  batch_size = 3;
  auto sub = subscribe([](const auto &) { throw std::runtime_error("callback"); });
  EXPECT_THROW(receive(), std::runtime_error);
  EXPECT_EQ(events, (std::vector<std::string>{"release", "release", "release"}));
  EXPECT_EQ(released_entries(), (std::vector<int64_t>{0, 1, 2}));
  sub.reset();
}

TEST_F(SubscriptionLifetime, MessageCreationFailureReleasesUnadoptedEntries)
{
  batch_size = 3;
  auto sub = subscribe([](const auto &) { FAIL() << "Incomplete batch reached callback"; });
  auto & info = hermos::id2_callback_info.begin()->second;
  auto original = info.message_creator;
  unsigned created = 0;
  info.message_creator = [original, &created](
    void * ptr, const std::string & topic, hermos::topic_local_id_t id, int64_t entry) {
    if (++created == 2) {
      throw std::bad_alloc();
    }
    return original(ptr, topic, id, entry);
  };
  EXPECT_THROW(receive(), std::bad_alloc);
  EXPECT_EQ(endpoint_events(), (std::vector<std::string>{"release", "release", "release"}));
  EXPECT_EQ(released_entries(), (std::vector<int64_t>{0, 1, 2}));
  sub.reset();
}

TEST_F(SubscriptionLifetime, MappingFailureReleasesEntireReceivedBatch)
{
  batch_size = 3;
  fail_mapping = true;
  auto sub = subscribe([](const auto &) { FAIL() << "Unmapped batch reached callback"; });
  EXPECT_THROW(receive(), std::bad_alloc);
  EXPECT_EQ(endpoint_events(), (std::vector<std::string>{"release", "release", "release"}));
  EXPECT_EQ(released_entries(), (std::vector<int64_t>{0, 1, 2}));
  sub.reset();
}

TEST_F(SubscriptionLifetime, ContinuationDoesNotReceiveAfterUnregister)
{
  call_again = true;
  auto sub = subscribe([](const auto &) {});
  receive();
  ASSERT_EQ(ready.size(), 1u);
  sub.reset();
  (*ready.front().callable)();
  EXPECT_EQ(receive_count, 1);
  EXPECT_EQ(released_entries(), (std::vector<int64_t>{0}));
}

TEST_F(SubscriptionLifetime, BatchDoesNotWaitForRegistryBetweenCallbacks)
{
  EXPECT_EXIT({
    std::signal(SIGALRM, [](int) { _exit(90); });
    alarm(3);
    batch_size = 3;
    unsigned callbacks = 0;
    std::unique_lock<std::mutex> registry(hermos::id2_callback_info_mtx, std::defer_lock);
    auto sub = subscribe([&](const auto &) {
      if (++callbacks == 1) {
        registry.lock();
      }
    });
    receive();
    registry.unlock();
    sub.reset();
    _exit(callbacks == 3 && released_ids.size() == 3 ? 0 : 91);
  }, ::testing::ExitedWithCode(0), "");
}

TEST_F(SubscriptionLifetime, InFlightHandlerKeepsNotificationOpen)
{
  auto sub = subscribe([](const auto &) {});
  auto info = hermos::id2_callback_info.begin()->second;
  sub.reset();
  EXPECT_TRUE(events.empty());
  info = {};
  EXPECT_EQ(std::count(events.begin(), events.end(), "close_mq"), 1);
  EXPECT_EQ(std::count(events.begin(), events.end(), "remove"), 1);
}

TEST_F(SubscriptionLifetime, DriverWakeSchedulesOneReceiveAndClosesWithLastHandler)
{
  driver_wake_fd = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(driver_wake_fd, 0);
  auto sub = subscribe([](const auto &) {});
  auto info = hermos::id2_callback_info.begin()->second;
  const auto id = hermos::id2_callback_info.begin()->first;
  const int epoll = epoll_create1(EPOLL_CLOEXEC);
  ASSERT_GE(epoll, 0);
  hermos::SubscriptionEventHandler handler(getpid(), &ready_mutex, &ready);
  handler.prepare_epoll(epoll, [](const auto &) { return true; });
  handler.handle(id);
  handler.handle(id);  // Stale event, notification already consumed.
  ASSERT_EQ(ready.size(), 1u);
  (*ready.front().callable)();
  EXPECT_EQ(receive_count, 1);
  sub.reset();
  EXPECT_GE(fcntl(driver_wake_fd, F_GETFD), 0);
  info = {};
  EXPECT_EQ(fcntl(driver_wake_fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
  close(epoll);
}

TEST_F(SubscriptionLifetime, DriverBindFailureReleasesRegistration)
{
  bind_error = EMFILE;
  EXPECT_THROW(subscribe([](const auto &) {}), std::system_error);
  EXPECT_EQ(events, (std::vector<std::string>{"remove"}));
}

TEST_F(SubscriptionLifetime, LegacyTopicNameRejectedBeforeOpeningMq)
{
  for (const char * legacy : {"/lifetime", "/bridged_alias"}) {
    returned_token = legacy;
    try {
      auto sub = subscribe([](const auto &) {});
      FAIL() << "expected incompatible kernel to be rejected";
    } catch (const std::system_error & e) {
      EXPECT_EQ(e.code().value(), EPROTONOSUPPORT);
    }
    EXPECT_EQ(events, (std::vector<std::string>{"remove"}));
    events.clear();
  }
}

TEST_F(SubscriptionLifetime, PublisherRegistrationRollsBackWithLegacyTopicName)
{
  std::string token;
  returned_token = "/lifetime";
  EXPECT_THROW(
    hermos::initialize_publisher("/lifetime", "/node", rclcpp::QoS(1), false, "", token),
    std::system_error);
  EXPECT_EQ(events, (std::vector<std::string>{"remove_publisher"}));
  returned_token = generation_token;
  EXPECT_EQ(hermos::initialize_publisher("/lifetime", "/node", rclcpp::QoS(1), false, "", token), 8);
  EXPECT_EQ(token, generation_token);
}

TEST_F(SubscriptionLifetime, DriverHangupStopsExecutorWakeups)
{
  int ends[2];
  ASSERT_EQ(pipe2(ends, O_NONBLOCK | O_CLOEXEC), 0);
  driver_wake_fd = ends[0];
  auto sub = subscribe([](const auto &) {});
  const auto id = hermos::id2_callback_info.begin()->first;
  const int epoll = epoll_create1(EPOLL_CLOEXEC);
  ASSERT_GE(epoll, 0);
  hermos::SubscriptionEventHandler handler(getpid(), &ready_mutex, &ready);
  const auto owns_group = [](const auto &) { return true; };
  handler.prepare_epoll(epoll, owns_group);
  close(ends[1]);
  epoll_event event{};
  ASSERT_EQ(epoll_wait(epoll, &event, 1, 0), 1);
  handler.handle(id);
  handler.prepare_epoll(epoll, owns_group);
  EXPECT_TRUE(ready.empty());
  EXPECT_EQ(epoll_wait(epoll, &event, 1, 0), 0);
  sub.reset();
  close(epoll);
}

TEST_F(SubscriptionLifetime, TakeResultOutlivesSubscriptionAndSharesRepeatedEntry)
{
  auto sub = std::make_shared<hermos::TakeSubscription<int>>(
    node.get(), "/lifetime", rclcpp::QoS(10), hermos::SubscriptionOptions{},
    hermos::SubscriptionRole::HermosOnly);
  auto first = sub->take(true);
  auto second = sub->take(true);
  sub.reset();
  EXPECT_TRUE(events.empty());
  first.reset();
  EXPECT_TRUE(events.empty());
  second.reset();
  EXPECT_EQ(events, (std::vector<std::string>{"release", "remove"}));
}
}  // namespace
