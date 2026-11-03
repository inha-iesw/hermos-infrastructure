#include "hermos/bridge/performance/hermos_performance_bridge_ipc_event_loop.hpp"

#include "hermos/hermos_utils.hpp"
#include "hermos/bridge/hermos_bridge_msg.hpp"

#include <utility>
#include <vector>

namespace hermos
{

PerformanceBridgeIpcEventLoop::PerformanceBridgeIpcEventLoop(const rclcpp::Logger & logger)
: IpcEventLoopBase(
    logger,
    // 1. Abstract-namespace UDS address
    create_uds_addr_for_bridge(),
    // 2. Upper bound on any single received message
    BRIDGE_MSG_MAX_SIZE,
    // 3. Block Signals
    {SIGTERM, SIGINT},
    // 4. Ignore Signals
    {SIGPIPE, SIGHUP})
{
}

}  // namespace hermos
