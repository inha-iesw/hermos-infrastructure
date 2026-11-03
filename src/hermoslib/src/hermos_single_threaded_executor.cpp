#include "hermos/hermos_single_threaded_executor.hpp"

#include "hermos/hermos.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sys/epoll.h"

#include <unordered_map>

namespace hermos
{

SingleThreadedHermosExecutor::SingleThreadedHermosExecutor(
  const rclcpp::ExecutorOptions & options, int next_exec_timeout_ms)
: hermos::HermosExecutor(options), next_exec_timeout_ms_(next_exec_timeout_ms)
{
  TRACEPOINT(
    hermos_construct_executor, static_cast<const void *>(this),
    "hermos_single_threaded_executor");

  const int next_exec_timeout_ms_threshold = 500;  // Rough value
  if (next_exec_timeout_ms_ > next_exec_timeout_ms_threshold) {
    RCLCPP_WARN(
      logger,
      "Due to the large next_exec_timeout_ms value, the callbacks registered after spin and ROS 2 "
      "callbacks may be extremely slow to execute.");
  }
}

bool SingleThreadedHermosExecutor::validate_callback_group(
  const rclcpp::CallbackGroup::SharedPtr & group) const
{
  if (!group) {
    RCLCPP_ERROR(logger, "Callback group is nullptr. The node may have been destructed.");
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  if (is_dedicated_to_one_callback_group_) {
    return group == dedicated_callback_group_;
  }

  return true;
}

void SingleThreadedHermosExecutor::spin()
{
  if (spinning.exchange(true)) {
    RCLCPP_ERROR(logger, "spin() called while already spinning");
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  RCPPUTILS_SCOPE_EXIT(this->spinning.store(false););

  while (rclcpp::ok(this->context_) && spinning.load()) {
    if (epoll_update_tracker_.take_update_request()) {
      prepare_epoll();
      warn_if_mixed_callback_groups();
    }

    hermos::HermosExecutable hermos_executable;
    if (get_next_hermos_executable(
          hermos_executable, next_exec_timeout_ms_ /*timed-blocking*/)) {
      execute_hermos_executable(hermos_executable);
    }

    rclcpp::AnyExecutable any_executable;
    if (get_next_executable(any_executable, std::chrono::nanoseconds(0) /*non-blocking*/)) {
      execute_any_executable(any_executable);
    }
  }
}

void SingleThreadedHermosExecutor::warn_if_mixed_callback_groups()
{
  std::unordered_map<rclcpp::CallbackGroup::SharedPtr, std::vector<std::string>> group_topics;

  {
    std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
    for (const auto & [id, info] : id2_callback_info) {
      if (info.callback_group && warned_mixed_groups_.count(info.callback_group.get()) == 0) {
        group_topics[info.callback_group].push_back(info.topic_name);
      }
    }
  }

  for (auto & [group, topics] : group_topics) {
    bool has_ros_callback = false;
    group->collect_all_ptrs(
      [&has_ros_callback](const rclcpp::SubscriptionBase::SharedPtr &) { has_ros_callback = true; },
      [&has_ros_callback](const rclcpp::ServiceBase::SharedPtr &) { has_ros_callback = true; },
      [&has_ros_callback](const rclcpp::ClientBase::SharedPtr &) { has_ros_callback = true; },
      [&has_ros_callback](const rclcpp::TimerBase::SharedPtr &) { has_ros_callback = true; },
      [&has_ros_callback](const rclcpp::Waitable::SharedPtr &) { has_ros_callback = true; });

    if (has_ros_callback) {
      warned_mixed_groups_.insert(group.get());
      std::string hermos_entities_str = "Hermos callbacks";
      if (!topics.empty()) {
        std::string topics_str;
        for (const auto & t : topics) {
          if (!topics_str.empty()) {
            topics_str += ", ";
          }
          topics_str += t;
        }
        hermos_entities_str += " (topics: [" + topics_str + "])";
      }
      RCLCPP_WARN(
        logger,
        "Callback group %p contains both ROS 2 callbacks and %s. "
        "In SingleThreadedHermosExecutor, the Hermos timed wait may block "
        "ROS 2 callbacks in the same callback group every spin iteration. "
        "Consider separating them into different callback groups.",
        static_cast<const void *>(group.get()), hermos_entities_str.c_str());
    }
  }
}

void SingleThreadedHermosExecutor::dedicate_to_callback_group(
  const rclcpp::CallbackGroup::SharedPtr & group,
  const rclcpp::node_interfaces::NodeBaseInterface::SharedPtr & node)
{
  if (!group) {
    RCLCPP_ERROR(logger, "The passed callback group is nullptr.");
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  is_dedicated_to_one_callback_group_ = true;
  dedicated_callback_group_ = group;

  add_callback_group(group, node);
}

}  // namespace hermos
