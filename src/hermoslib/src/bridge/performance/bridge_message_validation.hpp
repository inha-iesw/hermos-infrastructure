#pragma once

#include "hermos/bridge/hermos_bridge_msg.hpp"

#include <algorithm>
#include <cstring>

namespace hermos::detail
{

template <std::size_t N>
inline bool terminated(const char (&field)[N])
{
  return std::memchr(field, '\0', N) != nullptr;
}

inline bool valid_direction(BridgeDirection direction)
{
  return direction == BridgeDirection::ROS2_TO_HERMOS ||
         direction == BridgeDirection::HERMOS_TO_ROS2;
}

inline bool valid_wire_bool(const bool & value)
{
  // Inspect the representation without loading an invalid bool value.
  return *reinterpret_cast<const unsigned char *>(&value) <= 1;
}

// Decode before any consumer interprets the fixed-size wire fields as C strings.
inline bool decode_bridge_message(const void * data, std::size_t size, BridgeMsg & msg)
{
  if (size < offsetof(BridgeMsg, payload)) {
    return false;
  }
  msg = {};
  std::memcpy(&msg, data, std::min(size, sizeof(msg)));
  switch (msg.type) {
    case BridgeMsgType::PubSub: {
      const auto & payload = msg.payload.pubsub;
      return size >= bridge_msg_wire_size<BridgeMsgPubSubPayload>() &&
             terminated(payload.message_type) && terminated(payload.topic_name) &&
             valid_direction(payload.direction);
    }
    case BridgeMsgType::Service: {
      const auto & payload = msg.payload.service;
      return size >= bridge_msg_wire_size<BridgeMsgServicePayload>() &&
             terminated(payload.service_type) && terminated(payload.service_name) &&
             terminated(payload.shadow_node_namespace) && terminated(payload.shadow_node_name) &&
             valid_direction(payload.direction) && valid_wire_bool(payload.create_shadow_node);
    }
    case BridgeMsgType::DaemonPubSub: {
      const auto & payload = msg.payload.daemon_pubsub;
      return size >= bridge_msg_wire_size<BridgeMsgDaemonPubSubPayload>() &&
             payload.qos_depth > 0 &&
             terminated(payload.topic_name) && terminated(payload.type_name) &&
             valid_direction(payload.direction) && valid_wire_bool(payload.qos_is_transient_local) &&
             valid_wire_bool(payload.qos_is_reliable);
    }
    default:
      return false;
  }
}

}  // namespace hermos::detail
