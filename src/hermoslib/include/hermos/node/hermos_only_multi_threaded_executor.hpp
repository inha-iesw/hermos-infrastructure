#pragma once

#include "hermos/hermos_public_api.hpp"
#include "hermos/node/hermos_only_executor.hpp"
#include "rclcpp/rclcpp.hpp"

namespace hermos
{

/** @brief Multi-threaded executor for Stage 2 (Hermos-only) with configurable thread count. Used
 * with hermos::Node. */
HERMOS_PUBLIC
class HermosOnlyMultiThreadedExecutor : public HermosOnlyExecutor
{
  RCLCPP_DISABLE_COPY(HermosOnlyMultiThreadedExecutor)

  size_t number_of_threads_;
  bool yield_before_execute_;
  const int next_exec_timeout_ms_;

  void hermos_spin();

public:
  /// Construct the executor.
  /// @param number_of_threads Number of threads (0 = auto).
  /// @param yield_before_execute Yield before executing each callback.
  /// @param next_exec_timeout_ms Timeout in ms for Hermos executables.
  HERMOS_PUBLIC
  explicit HermosOnlyMultiThreadedExecutor(
    size_t number_of_threads = 0, bool yield_before_execute = false, int next_exec_timeout_ms = 50);

  /// Block the calling thread and process Hermos callbacks in a loop until cancel() is called.
  HERMOS_PUBLIC
  void spin() override;
};

}  // namespace hermos
