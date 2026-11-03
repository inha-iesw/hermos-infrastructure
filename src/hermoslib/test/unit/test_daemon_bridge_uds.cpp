#include "hermos/hermos_utils.hpp"
#include "hermos/bridge/hermos_bridge_msg.hpp"
#include "hermos/bridge/hermos_bridge_utils.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace
{
// Restores ROS_DOMAIN_ID to its pre-test value on scope exit, so tests that
// mutate it don't leak into later tests (the runner may start with it set).
class ScopedRosDomainId
{
public:
  ScopedRosDomainId()
  {
    const char * value = getenv("ROS_DOMAIN_ID");
    if (value != nullptr) {
      had_value_ = true;
      old_value_ = value;
    }
  }
  ~ScopedRosDomainId()
  {
    if (had_value_) {
      setenv("ROS_DOMAIN_ID", old_value_.c_str(), 1);
    } else {
      unsetenv("ROS_DOMAIN_ID");
    }
  }

private:
  bool had_value_ = false;
  std::string old_value_;
};
}  // namespace

// The discovery daemon (Python) packs BridgeMsgDaemonPubSubPayload by hand inside a
// BridgeMsg, mirroring this layout. These checks fail loudly if the C++ struct
// drifts from the daemon's `_MSG_PACK_FORMAT`.
TEST(DaemonBridgeUdsTest, DaemonPayloadWireLayout)
{
  using hermos::BridgeMsgDaemonPubSubPayload;
  EXPECT_EQ(sizeof(BridgeMsgDaemonPubSubPayload), 524u);
  EXPECT_EQ(offsetof(BridgeMsgDaemonPubSubPayload, topic_name), 0u);
  EXPECT_EQ(offsetof(BridgeMsgDaemonPubSubPayload, type_name), 256u);
  EXPECT_EQ(offsetof(BridgeMsgDaemonPubSubPayload, direction), 512u);
  EXPECT_EQ(offsetof(BridgeMsgDaemonPubSubPayload, qos_depth), 516u);
  EXPECT_EQ(offsetof(BridgeMsgDaemonPubSubPayload, qos_is_transient_local), 520u);
  EXPECT_EQ(offsetof(BridgeMsgDaemonPubSubPayload, qos_is_reliable), 521u);
}

TEST(DaemonBridgeUdsTest, BridgeMsgTypeNumeric)
{
  EXPECT_EQ(static_cast<uint32_t>(hermos::BridgeMsgType::PubSub), 0u);
  EXPECT_EQ(static_cast<uint32_t>(hermos::BridgeMsgType::Service), 1u);
  EXPECT_EQ(static_cast<uint32_t>(hermos::BridgeMsgType::DaemonPubSub), 2u);
}

TEST(DaemonBridgeUdsTest, BridgeMsgWireLayout)
{
  EXPECT_EQ(offsetof(hermos::BridgeMsg, type), 0u);
  EXPECT_EQ(offsetof(hermos::BridgeMsg, payload), 4u);
  EXPECT_EQ(
    hermos::bridge_msg_wire_size<hermos::BridgeMsgDaemonPubSubPayload>(),
    4u + sizeof(hermos::BridgeMsgDaemonPubSubPayload));
  EXPECT_EQ(
    hermos::bridge_msg_wire_size<hermos::BridgeMsgPubSubPayload>(),
    4u + sizeof(hermos::BridgeMsgPubSubPayload));
  EXPECT_EQ(
    hermos::bridge_msg_wire_size<hermos::BridgeMsgServicePayload>(),
    4u + sizeof(hermos::BridgeMsgServicePayload));
}

// An empty ROS_DOMAIN_ID (set but "") means "no domain": no `_d` suffix.
TEST(DaemonBridgeUdsTest, BridgeUdsAddrEmptyDomainIdHasNoSuffix)
{
  const ScopedRosDomainId guard;
  setenv("ROS_DOMAIN_ID", "", 1);
  const auto addr = hermos::create_uds_addr_for_bridge();
  ASSERT_FALSE(addr.empty());
  EXPECT_EQ(addr[0], '\0');
  const auto expected =
    "hermos_bridge_manager_" + std::to_string(hermos::get_self_ipc_ns_inode());
  EXPECT_EQ(addr.substr(1), expected);
}

TEST(DaemonBridgeUdsTest, BridgeUdsAddrAppendsDomainId)
{
  const ScopedRosDomainId guard;
  setenv("ROS_DOMAIN_ID", "7", 1);
  const auto addr = hermos::create_uds_addr_for_bridge();
  ASSERT_FALSE(addr.empty());
  EXPECT_EQ(addr[0], '\0');
  const auto expected =
    "hermos_bridge_manager_" + std::to_string(hermos::get_self_ipc_ns_inode()) + "_d7";
  EXPECT_EQ(addr.substr(1), expected);
}

TEST(DaemonBridgeUdsTest, BridgeUdsAddrUnsetDomainIdHasNoSuffix)
{
  const ScopedRosDomainId guard;
  unsetenv("ROS_DOMAIN_ID");
  const auto addr = hermos::create_uds_addr_for_bridge();
  ASSERT_FALSE(addr.empty());
  EXPECT_EQ(addr[0], '\0');
  const auto expected =
    "hermos_bridge_manager_" + std::to_string(hermos::get_self_ipc_ns_inode());
  EXPECT_EQ(addr.substr(1), expected);
}

// Performance-mode daemon bridges have no local endpoint to query, so the QoS
// must be rebuilt faithfully from the request's explicit fields.
TEST(DaemonBridgeUdsTest, DaemonRequestQosReliableTransientLocal)
{
  hermos::BridgeMsgDaemonPubSubPayload req{};
  req.qos_depth = 10;
  req.qos_is_reliable = true;
  req.qos_is_transient_local = true;

  const rclcpp::QoS qos = hermos::daemon_request_qos(req);
  EXPECT_EQ(qos.depth(), 10u);
  EXPECT_EQ(qos.reliability(), rclcpp::ReliabilityPolicy::Reliable);
  EXPECT_EQ(qos.durability(), rclcpp::DurabilityPolicy::TransientLocal);
}

TEST(DaemonBridgeUdsTest, DaemonRequestQosBestEffortVolatile)
{
  hermos::BridgeMsgDaemonPubSubPayload req{};
  req.qos_depth = 1;
  req.qos_is_reliable = false;
  req.qos_is_transient_local = false;

  const rclcpp::QoS qos = hermos::daemon_request_qos(req);
  EXPECT_EQ(qos.depth(), 1u);
  EXPECT_EQ(qos.reliability(), rclcpp::ReliabilityPolicy::BestEffort);
  EXPECT_EQ(qos.durability(), rclcpp::DurabilityPolicy::Volatile);
}

// The daemon-forced lease (used by the performance bridge_manager to keep a
// cross-NS bridge alive without a same-graph DDS counterpart) is active for the
// half-open window [registered, registered + DAEMON_FORCE_TTL).
TEST(DaemonBridgeUdsTest, DaemonForceLeaseWindowIsHalfOpen)
{
  const std::chrono::steady_clock::time_point t0{};
  const auto deadline = hermos::daemon_force_deadline(t0);

  // The lease lasts exactly DAEMON_FORCE_TTL.
  EXPECT_EQ(deadline - t0, hermos::DAEMON_FORCE_TTL);

  EXPECT_TRUE(hermos::is_daemon_force_active(deadline, t0));  // just registered
  EXPECT_TRUE(hermos::is_daemon_force_active(
    deadline, t0 + hermos::DAEMON_FORCE_TTL - std::chrono::milliseconds(1)));  // within
  // Boundary: the window is half-open, so the exact deadline is already expired.
  EXPECT_FALSE(hermos::is_daemon_force_active(deadline, t0 + hermos::DAEMON_FORCE_TTL));
  EXPECT_FALSE(hermos::is_daemon_force_active(
    deadline, t0 + hermos::DAEMON_FORCE_TTL + std::chrono::seconds(1)));  // after
}

// Re-asserting the request (the daemon does so every tick) pushes the deadline
// out, so a continuously-requested bridge never lapses.
TEST(DaemonBridgeUdsTest, DaemonForceLeaseRenewalExtendsDeadline)
{
  const std::chrono::steady_clock::time_point t0{};
  const auto first = hermos::daemon_force_deadline(t0);
  const auto renewed = hermos::daemon_force_deadline(t0 + std::chrono::seconds(1));

  EXPECT_GT(renewed, first);
  // Still active at the original deadline because it was renewed before lapsing.
  EXPECT_TRUE(hermos::is_daemon_force_active(renewed, first));
}
