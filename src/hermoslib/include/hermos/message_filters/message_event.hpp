#pragma once

#include "hermos/hermos_public_api.hpp"
#include "hermos/hermos_smart_pointer.hpp"

#include <rclcpp/rclcpp.hpp>

#include <message_filters/message_event.h>

#include <memory>
#include <type_traits>

namespace hermos
{
namespace message_filters
{

/// Wrapper around a message pointer that carries metadata for message filter pipelines.
HERMOS_PUBLIC
template <typename M>
class MessageEvent
{
  static_assert(
    std::is_const<M>::value,
    "hermos::message_filters::MessageEvent only supports const message types. "
    "Use MessageEvent<YourMessage const> instead of MessageEvent<YourMessage>.");

public:
  using ConstMessage = M;
  using Message = typename std::remove_const<M>::type;
  using ConstMessagePtr = ipc_shared_ptr<ConstMessage>;

  /// Default-construct an empty MessageEvent.
  HERMOS_PUBLIC MessageEvent() = default;

  /// Copy constructor.
  HERMOS_PUBLIC MessageEvent(const MessageEvent & rhs) = default;

  /// Construct from a message pointer, recording the current time as receipt time.
  HERMOS_PUBLIC explicit MessageEvent(const ConstMessagePtr & message)
  : message_(message), receipt_time_(rclcpp::Clock().now())
  {
  }

  /// Construct from a message pointer and an explicit receipt time.
  HERMOS_PUBLIC MessageEvent(const ConstMessagePtr & message, rclcpp::Time receipt_time)
  : message_(message), receipt_time_(receipt_time)
  {
  }

  /// Copy assignment operator.
  HERMOS_PUBLIC MessageEvent & operator=(const MessageEvent & rhs) = default;

  /**
   * \brief Retrieve the message.
   * Returns ipc_shared_ptr<M const> pointing to shared memory.
   */
  HERMOS_PUBLIC const ConstMessagePtr & getMessage() const { return message_; }

  /**
   * \brief Retrieve a const version of the message (same as getMessage() in hermos)
   */
  HERMOS_PUBLIC const ConstMessagePtr & getConstMessage() const { return message_; }

  /**
   * \brief Returns the time at which this message was received
   */
  HERMOS_PUBLIC rclcpp::Time getReceiptTime() const { return receipt_time_; }

  /// Less-than comparison, ordered by pointer then receipt time.
  HERMOS_PUBLIC bool operator<(const MessageEvent & rhs) const
  {
    if (message_.get() != rhs.message_.get()) {
      return message_.get() < rhs.message_.get();
    }

    return receipt_time_ < rhs.receipt_time_;
  }

  /// Equality comparison.
  HERMOS_PUBLIC bool operator==(const MessageEvent & rhs) const
  {
    return message_.get() == rhs.message_.get() && receipt_time_ == rhs.receipt_time_;
  }

  /// Inequality comparison.
  HERMOS_PUBLIC bool operator!=(const MessageEvent & rhs) const { return !(*this == rhs); }

private:
  ConstMessagePtr message_;
  rclcpp::Time receipt_time_;
};

}  // namespace message_filters
}  // namespace hermos
