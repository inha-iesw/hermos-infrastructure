#pragma once

#include "hermos/hermos_callback_info.hpp"
#include "hermos/hermos_executor.hpp"
#include "hermos/hermos_public_api.hpp"
#include "rclcpp/rclcpp.hpp"

#include <unordered_set>

namespace hermos
{

/** @brief Single-threaded executor for Stage 1 that processes both ROS 2 and Hermos callbacks on
 * one thread. */
HERMOS_PUBLIC
class SingleThreadedHermosExecutor : public hermos::HermosExecutor
{
  RCLCPP_DISABLE_COPY(SingleThreadedHermosExecutor)

  const int next_exec_timeout_ms_;
  bool validate_callback_group(const rclcpp::CallbackGroup::SharedPtr & group) const override;

  // Activate when this Executor is used for the implementation of a
  // CallbackIsolatedHermosExecutor.
  bool is_dedicated_to_one_callback_group_ = false;
  rclcpp::CallbackGroup::SharedPtr dedicated_callback_group_ = nullptr;

  std::unordered_set<const rclcpp::CallbackGroup *> warned_mixed_groups_;
  void warn_if_mixed_callback_groups();

public:
  /// Construct the executor.
  /// @param options Executor options.
  /// @param next_exec_timeout_ms Timeout in ms for waiting on the next executable.
  HERMOS_PUBLIC
  explicit SingleThreadedHermosExecutor(
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions(),
    int next_exec_timeout_ms = 50);

  /// Block the calling thread and process callbacks in a loop until rclcpp::shutdown() is called or
  /// the executor is cancelled.
  HERMOS_PUBLIC
  void spin() override;

  // Not used for public API, but required to be exposed for the internal implementation
  void dedicate_to_callback_group(
    const rclcpp::CallbackGroup::SharedPtr & group,
    const rclcpp::node_interfaces::NodeBaseInterface::SharedPtr & node);
};

}  // namespace hermos
