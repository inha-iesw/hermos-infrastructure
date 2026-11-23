#include "hermos/hermos_executor.hpp"

#include "hermos/hermos.hpp"
#include "hermos/hermos_epoll.hpp"
#include "hermos/hermos_epoll_event.hpp"
#include "hermos/hermos_epoll_update_dispatcher.hpp"
#include "hermos/hermos_tracepoint_wrapper.h"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp/version.h"
#include "rcpputils/scope_exit.hpp"
#include "sys/epoll.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <unordered_map>

namespace hermos
{

HermosExecutor::HermosExecutor(const rclcpp::ExecutorOptions & options)
: rclcpp::Executor(options),
  my_pid_(getpid()),
  epoll_update_tracker_(EpollUpdateDispatcher::get_instance().register_tracker())
{
  EventHandlerArray sources;
  sources[static_cast<uint32_t>(EpollEventType::Subscription)] =
    std::make_unique<SubscriptionEventHandler>(
      my_pid_, &ready_hermos_executables_mutex_, &ready_hermos_executables_);
  sources[static_cast<uint32_t>(EpollEventType::Timer)] = std::make_unique<TimerEventHandler>(
    my_pid_, &ready_hermos_executables_mutex_, &ready_hermos_executables_);
  sources[static_cast<uint32_t>(EpollEventType::Clock)] = std::make_unique<ClockEventHandler>(
    my_pid_, &ready_hermos_executables_mutex_, &ready_hermos_executables_);
  sources[static_cast<uint32_t>(EpollEventType::Shutdown)] = std::make_unique<DummyEventHandler>();

  epoll_manager_ = std::make_unique<EpollManager>(std::move(sources));
}

HermosExecutor::~HermosExecutor() = default;

void HermosExecutor::add_callback_group(
  rclcpp::CallbackGroup::SharedPtr group,
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node, bool notify)
{
  rclcpp::Executor::add_callback_group(std::move(group), std::move(node), notify);
  ownership_generation_.fetch_add(1);
  EpollUpdateDispatcher::get_instance().request_update(epoll_update_tracker_.id());
}

void HermosExecutor::remove_callback_group(rclcpp::CallbackGroup::SharedPtr group, bool notify)
{
  rclcpp::Executor::remove_callback_group(std::move(group), notify);
  ownership_generation_.fetch_add(1);
  EpollUpdateDispatcher::get_instance().request_update(epoll_update_tracker_.id());
}

void HermosExecutor::add_node(
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node, bool notify)
{
  rclcpp::Executor::add_node(std::move(node), notify);
  ownership_generation_.fetch_add(1);
  EpollUpdateDispatcher::get_instance().request_update(epoll_update_tracker_.id());
}

void HermosExecutor::remove_node(
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node, bool notify)
{
  rclcpp::Executor::remove_node(std::move(node), notify);
  ownership_generation_.fetch_add(1);
  EpollUpdateDispatcher::get_instance().request_update(epoll_update_tracker_.id());
}

std::vector<rclcpp::CallbackGroup::WeakPtr> HermosExecutor::get_owned_callback_groups()
{
#if RCLCPP_VERSION_MAJOR < 28
  // Humble does not discover groups created after add_node() in the getter.
  {
    std::lock_guard<std::mutex> guard(mutex_);
    add_callback_groups_from_nodes_associated_to_executor();
  }
#endif
  return get_all_callback_groups();
}

void HermosExecutor::prepare_epoll()
{
  // A stale pass must not detach descriptors after a newer pass has attached them.
  std::lock_guard<std::mutex> prepare_lock(prepare_epoll_mutex_);
  // Snapshot before taking handler locks. A removal after this snapshot must
  // invalidate it so an old prepare cannot reclaim descriptors after transfer.
  const auto generation = ownership_generation_.load();
  std::unordered_map<rclcpp::CallbackGroup::SharedPtr, std::optional<bool>> groups;
  for (const auto & weak_group : get_owned_callback_groups()) {
    if (auto group = weak_group.lock()) {
      groups.emplace(std::move(group), std::nullopt);
    }
  }
  epoll_manager_->prepare_epoll(
    [this, &groups, generation](const rclcpp::CallbackGroup::SharedPtr & group) {
      const auto it = groups.find(group);
      if (it == groups.end() || ownership_generation_.load() != generation) {
        return false;
      }
      // A group can own many subscriptions and timers; validate it once per pass.
      if (!it->second.has_value()) {
        it->second = validate_callback_group(group);
      }
      return *it->second && ownership_generation_.load() == generation;
    });
}

bool HermosExecutor::get_next_hermos_executable(
  HermosExecutable & hermos_executable, const int timeout_ms)
{
  if (get_next_ready_hermos_executable(hermos_executable)) {
    return true;
  }

  epoll_manager_->wait_and_handle_epoll_event(timeout_ms);

  // Try again
  return get_next_ready_hermos_executable(hermos_executable);
}

bool HermosExecutor::get_next_ready_hermos_executable(HermosExecutable & hermos_executable)
{
  std::lock_guard<std::mutex> ready_wait_lock{ready_hermos_executables_mutex_};
  if (ready_hermos_executables_.empty()) {
    return false;
  }
  // Keep the ownership snapshot and queue selection atomic against enqueue.
  // Otherwise newly attached work can be erased using a snapshot predating it.
  // prepare_epoll releases rclcpp's ownership lock before taking handler/queue locks.
  const auto groups = get_owned_callback_groups();

  for (auto it = ready_hermos_executables_.begin(); it != ready_hermos_executables_.end();) {
    // A removed node/group must not leave callbacks retained in this executor's queue.
    const bool group_found = std::any_of(
      groups.begin(), groups.end(),
      [&it](const rclcpp::CallbackGroup::WeakPtr & weak_group) {
        return weak_group.lock() == it->callback_group;
      });
    if (!group_found) {
      it = ready_hermos_executables_.erase(it);
      continue;
    }

    if (
      it->callback_group->type() == rclcpp::CallbackGroupType::MutuallyExclusive &&
      !it->callback_group->can_be_taken_from().exchange(false)) {
      ++it;
      continue;
    }

    hermos_executable = *it;
    ready_hermos_executables_.erase(it);

    return true;
  }

  return false;
}

void HermosExecutor::execute_hermos_executable(HermosExecutable & hermos_executable)
{
  RCPPUTILS_SCOPE_EXIT({
    if (hermos_executable.callback_group->type() == rclcpp::CallbackGroupType::MutuallyExclusive) {
      hermos_executable.callback_group->can_be_taken_from().store(true);
    }
  });
  (*hermos_executable.callable)();
}

}  // namespace hermos
