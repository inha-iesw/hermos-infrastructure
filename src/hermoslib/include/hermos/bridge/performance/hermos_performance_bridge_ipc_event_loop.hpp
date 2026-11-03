#pragma once

#include "hermos/bridge/hermos_bridge_ipc_event_loop_base.hpp"

#include <rclcpp/logger.hpp>

namespace hermos
{

class PerformanceBridgeIpcEventLoop : public IpcEventLoopBase
{
public:
  explicit PerformanceBridgeIpcEventLoop(const rclcpp::Logger & logger);
  ~PerformanceBridgeIpcEventLoop() override = default;
};

}  // namespace hermos
