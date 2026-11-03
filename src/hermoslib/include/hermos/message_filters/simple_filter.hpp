#pragma once

#include "hermos/hermos_public_api.hpp"
#include "hermos/message_filters/message_event.hpp"
#include "hermos/message_filters/signal1.hpp"

#include <message_filters/connection.h>

#include <functional>
#include <memory>
#include <string>

namespace hermos
{
namespace message_filters
{

using ::message_filters::Connection;
using ::message_filters::noncopyable;

/// Base class for simple one-output filters. Provides callback registration and signal dispatch.
HERMOS_PUBLIC
template <class M>
class SimpleFilter : public noncopyable
{
public:
  using MConstPtr = ipc_shared_ptr<M const>;
  using Callback = std::function<void(const MConstPtr &)>;
  using EventType = MessageEvent<M const>;
  using EventCallback = std::function<void(const EventType &)>;

  /// Register a callback to be invoked when a message passes through this filter.
  /// @param callback Callback to register.
  /// @return Connection object for disconnecting.
  template <typename C>
  HERMOS_PUBLIC Connection registerCallback(const C & callback)
  {
    typename CallbackHelper1<M>::Ptr helper = signal_.addCallback(Callback(callback));
    return Connection(std::bind(&Signal::removeCallback, &signal_, helper));
  }

  template <typename P>
  HERMOS_PUBLIC Connection registerCallback(const std::function<void(P)> & callback)
  {
    return Connection(std::bind(&Signal::removeCallback, &signal_, signal_.addCallback(callback)));
  }

  template <typename P>
  HERMOS_PUBLIC Connection registerCallback(void (*callback)(P))
  {
    typename CallbackHelper1<M>::Ptr helper =
      signal_.template addCallback<P>(std::bind(callback, std::placeholders::_1));
    return Connection(std::bind(&Signal::removeCallback, &signal_, helper));
  }

  template <typename T, typename P>
  HERMOS_PUBLIC Connection registerCallback(void (T::*callback)(P), T * t)
  {
    typename CallbackHelper1<M>::Ptr helper =
      signal_.template addCallback<P>(std::bind(callback, t, std::placeholders::_1));
    return Connection(std::bind(&Signal::removeCallback, &signal_, helper));
  }

  /// Set the name of this filter (for debugging).
  /// @param name Filter name.
  HERMOS_PUBLIC void setName(const std::string & name) { name_ = name; }

  /// Return the name of this filter.
  /// @return Filter name string.
  HERMOS_PUBLIC const std::string & getName() const { return name_; }

protected:
  // Call all registered callbacks, passing them the specified message
  void signalMessage(const MConstPtr & msg)
  {
    MessageEvent<M const> event(msg);
    signal_.call(event);
  }

  // Call all registered callbacks, passing them the specified message
  void signalMessage(const MessageEvent<M const> & event) { signal_.call(event); }

private:
  using Signal = Signal1<M>;

  Signal signal_;
  std::string name_;
};

}  // namespace message_filters
}  // namespace hermos
