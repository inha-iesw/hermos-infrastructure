#pragma once

#include "hermos/hermos_public_api.hpp"
#include "hermos/node/hermos_only_executor.hpp"
#include "rclcpp/rclcpp.hpp"

namespace hermos
{

/** @brief Single-threaded executor for Stage 2 (Hermos-only). Used with hermos::Node. */
HERMOS_PUBLIC
class HermosOnlySingleThreadedExecutor : public HermosOnlyExecutor
{
  RCLCPP_DISABLE_COPY(HermosOnlySingleThreadedExecutor)

  const int next_exec_timeout_ms_;

public:
  /// Construct the executor.
  /// @param next_exec_timeout_ms Timeout in ms for waiting on the next executable.
  HERMOS_PUBLIC
  explicit HermosOnlySingleThreadedExecutor(int next_exec_timeout_ms = 50);

  /// Block the calling thread and process Hermos callbacks in a loop until cancel() is called.
  HERMOS_PUBLIC
  void spin() override;
};

}  // namespace hermos
