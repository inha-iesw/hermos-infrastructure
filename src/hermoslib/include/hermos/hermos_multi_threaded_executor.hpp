#pragma once

#include "hermos/hermos_executor.hpp"
#include "hermos/hermos_public_api.hpp"
#include "rclcpp/rclcpp.hpp"

#include <chrono>

namespace hermos
{

/** @brief Multi-threaded executor for Stage 1 with configurable thread counts for ROS 2 and
 * Hermos callbacks. */
HERMOS_PUBLIC
class MultiThreadedHermosExecutor : public hermos::HermosExecutor
{
  RCLCPP_DISABLE_COPY(MultiThreadedHermosExecutor)

  size_t number_of_ros2_threads_;
  size_t number_of_hermos_threads_;

  bool yield_before_execute_;
  std::chrono::nanoseconds ros2_next_exec_timeout_;
  const int hermos_next_exec_timeout_ms_;

  void ros2_spin();
  void hermos_spin();
  bool validate_callback_group(const rclcpp::CallbackGroup::SharedPtr & group) const override;

public:
  /// Construct the executor.
  /// @param options Executor options.
  /// @param number_of_ros2_threads Number of threads for ROS 2 callbacks (0 = auto).
  /// @param number_of_hermos_threads Number of threads for Hermos callbacks (0 = auto).
  /// @param yield_before_execute If true, call std::this_thread::yield() before each callback
  /// execution to reduce CPU usage at the cost of latency.
  /// @param ros2_next_exec_timeout Timeout for ROS 2 executables.
  /// @param hermos_next_exec_timeout_ms Timeout in ms for Hermos executables.
  HERMOS_PUBLIC
  explicit MultiThreadedHermosExecutor(
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions(),
    size_t number_of_ros2_threads = 0, size_t number_of_hermos_threads = 0,
    bool yield_before_execute = false,
    std::chrono::nanoseconds ros2_next_exec_timeout = std::chrono::nanoseconds(-1),
    int hermos_next_exec_timeout_ms = 50);

  /// Block the calling thread and process callbacks in a loop until rclcpp::shutdown() is called or
  /// the executor is cancelled.
  HERMOS_PUBLIC
  void spin() override;
};

}  // namespace hermos
