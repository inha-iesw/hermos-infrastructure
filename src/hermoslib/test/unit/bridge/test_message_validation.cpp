#include "bridge/performance/bridge_message_validation.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace
{
using hermos::BridgeMsg;
using hermos::BridgeMsgType;

// Independent oracle for Python's '=I256s256sIIBB2x' daemon wire format.
using DaemonPayload = hermos::BridgeMsgDaemonPubSubPayload;
static_assert(sizeof(BridgeMsgType) == 4 && sizeof(hermos::BridgeDirection) == 4);
static_assert(sizeof(bool) == 1 && offsetof(BridgeMsg, payload) == 4);
static_assert(sizeof(DaemonPayload) == 524);
static_assert(hermos::bridge_msg_wire_size<DaemonPayload>() == 528);
static_assert(offsetof(DaemonPayload, topic_name) == 0);
static_assert(offsetof(DaemonPayload, type_name) == 256);
static_assert(offsetof(DaemonPayload, direction) == 512);
static_assert(offsetof(DaemonPayload, qos_depth) == 516);
static_assert(offsetof(DaemonPayload, qos_is_transient_local) == 520);
static_assert(offsetof(DaemonPayload, qos_is_reliable) == 521);

bool accepts(const BridgeMsg & input, std::size_t size = sizeof(BridgeMsg))
{
  BridgeMsg decoded{};
  return hermos::detail::decode_bridge_message(&input, size, decoded);
}

TEST(BridgeMessageValidation, AcceptsEachVariantAtItsWireSize)
{
  BridgeMsg msg{};
  msg.type = BridgeMsgType::PubSub;
  msg.payload.pubsub.direction = hermos::BridgeDirection::HERMOS_TO_ROS2;
  EXPECT_TRUE(accepts(msg, hermos::bridge_msg_wire_size<hermos::BridgeMsgPubSubPayload>()));
  msg = {};
  msg.type = BridgeMsgType::Service;
  msg.payload.service.create_shadow_node = true;
  EXPECT_TRUE(accepts(msg, hermos::bridge_msg_wire_size<hermos::BridgeMsgServicePayload>()));
  msg = {};
  msg.type = BridgeMsgType::DaemonPubSub;
  msg.payload.daemon_pubsub.qos_depth = 1;
  msg.payload.daemon_pubsub.qos_is_reliable = true;
  msg.payload.daemon_pubsub.qos_is_transient_local = true;
  EXPECT_TRUE(accepts(msg, hermos::bridge_msg_wire_size<hermos::BridgeMsgDaemonPubSubPayload>()));
}

TEST(BridgeMessageValidation, RejectsTruncatedAndUnknownVariants)
{
  BridgeMsg msg{};
  EXPECT_FALSE(accepts(msg, 0));
  msg.type = BridgeMsgType::PubSub;
  EXPECT_FALSE(accepts(msg, hermos::bridge_msg_wire_size<hermos::BridgeMsgPubSubPayload>() - 1));
  msg.type = BridgeMsgType::Service;
  EXPECT_FALSE(accepts(msg, hermos::bridge_msg_wire_size<hermos::BridgeMsgServicePayload>() - 1));
  msg.type = BridgeMsgType::DaemonPubSub;
  EXPECT_FALSE(accepts(msg, hermos::bridge_msg_wire_size<hermos::BridgeMsgDaemonPubSubPayload>() - 1));
  msg.type = static_cast<BridgeMsgType>(99);
  EXPECT_FALSE(accepts(msg));
}

template <std::size_t N>
void check_string_boundary(BridgeMsg & msg, char (&field)[N])
{
  std::memset(field, 'x', N);
  EXPECT_FALSE(accepts(msg));
  field[N - 1] = '\0';
  EXPECT_TRUE(accepts(msg));
  std::memset(field, 0, N);
}

TEST(BridgeMessageValidation, RequiresTerminatorInsideEveryStringField)
{
  BridgeMsg msg{};
  msg.type = BridgeMsgType::PubSub;
  check_string_boundary(msg, msg.payload.pubsub.message_type);
  check_string_boundary(msg, msg.payload.pubsub.topic_name);
  msg = {};
  msg.type = BridgeMsgType::Service;
  check_string_boundary(msg, msg.payload.service.service_type);
  check_string_boundary(msg, msg.payload.service.service_name);
  check_string_boundary(msg, msg.payload.service.shadow_node_namespace);
  check_string_boundary(msg, msg.payload.service.shadow_node_name);
  msg = {};
  msg.type = BridgeMsgType::DaemonPubSub;
  msg.payload.daemon_pubsub.qos_depth = 1;
  check_string_boundary(msg, msg.payload.daemon_pubsub.topic_name);
  check_string_boundary(msg, msg.payload.daemon_pubsub.type_name);
}

TEST(BridgeMessageValidation, RejectsInvalidDirectionAndBooleanRepresentations)
{
  BridgeMsg msg{};
  msg.type = BridgeMsgType::PubSub;
  msg.payload.pubsub.direction = static_cast<hermos::BridgeDirection>(99);
  EXPECT_FALSE(accepts(msg));
  msg = {};
  msg.type = BridgeMsgType::Service;
  auto * flag = reinterpret_cast<unsigned char *>(&msg.payload.service.create_shadow_node);
  *flag = 2;
  EXPECT_FALSE(accepts(msg));
  msg = {};
  msg.type = BridgeMsgType::DaemonPubSub;
  msg.payload.daemon_pubsub.qos_depth = 1;
  flag = reinterpret_cast<unsigned char *>(&msg.payload.daemon_pubsub.qos_is_reliable);
  *flag = 2;
  EXPECT_FALSE(accepts(msg));
  *flag = 1;
  flag = reinterpret_cast<unsigned char *>(&msg.payload.daemon_pubsub.qos_is_transient_local);
  *flag = 2;
  EXPECT_FALSE(accepts(msg));
  *flag = 1;
  msg.payload.daemon_pubsub.direction = static_cast<hermos::BridgeDirection>(99);
  EXPECT_FALSE(accepts(msg));
  msg = {};
  msg.type = BridgeMsgType::Service;
  msg.payload.service.direction = static_cast<hermos::BridgeDirection>(99);
  EXPECT_FALSE(accepts(msg));
}

TEST(BridgeMessageValidation, RejectsZeroDaemonDepthBeforeEndpointCreation)
{
  BridgeMsg msg{};
  msg.type = BridgeMsgType::DaemonPubSub;
  EXPECT_FALSE(accepts(msg));
  msg.payload.daemon_pubsub.qos_depth = 1;
  EXPECT_TRUE(accepts(msg));
  msg.payload.daemon_pubsub.qos_depth = UINT32_MAX;
  EXPECT_TRUE(accepts(msg));
}

}  // namespace
