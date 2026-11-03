#pragma once

#include "hermos/hermos_public_api.hpp"
#include "hermos/hermos_subscription.hpp"
#include "hermos/bridge/hermos_bridge_node.hpp"
#include "hermos/message_filters/simple_filter.hpp"
#include "hermos/node/hermos_node.hpp"

#include <rclcpp/rclcpp.hpp>

#include <memory>
#include <string>

namespace hermos
{
namespace message_filters
{

namespace detail
{
inline rclcpp::QoS to_rclcpp_qos(const rmw_qos_profile_t & rmw_qos)
{
  rclcpp::QoS qos(rclcpp::QoSInitialization::from_rmw(rmw_qos));
  qos.get_rmw_qos_profile() = rmw_qos;
  return qos;
}
}  // namespace detail

/// Base class for Subscriber, allowing subscription management without knowing the message type.
/// Used for type-erased subscriber collections.
HERMOS_PUBLIC
template <class NodeType = rclcpp::Node>
class SubscriberBase
{
public:
  using NodePtr = std::shared_ptr<NodeType>;

  virtual ~SubscriberBase() = default;

  /**
   * \brief Subscribe to a topic.
   *
   * If this Subscriber is already subscribed to a topic, this function will first unsubscribe.
   *
   * \param node The rclcpp::Node::SharedPtr to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos (optional) The rmw qos profile to use to subscribe.
   */
  HERMOS_PUBLIC virtual void subscribe(
    NodePtr node, const std::string & topic,
    const rmw_qos_profile_t qos = rmw_qos_profile_default) = 0;

  /**
   * \brief Subscribe to a topic.
   *
   * If this Subscriber is already subscribed to a topic, this function will first unsubscribe.
   *
   * \param node The rclcpp::Node to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos (optional) The rmw qos profile to use to subscribe.
   */
  HERMOS_PUBLIC virtual void subscribe(
    NodeType * node, const std::string & topic,
    const rmw_qos_profile_t qos = rmw_qos_profile_default) = 0;

  /**
   * \brief Subscribe to a topic.
   *
   * If this Subscriber is already subscribed to a topic, this function will first unsubscribe.
   * This override allows SubscriptionOptions to be passed into the class without changing API.
   *
   * \param node The rclcpp::Node::SharedPtr to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos The rmw qos profile to use to subscribe.
   * \param options The subscription options to use to subscribe.
   */
  HERMOS_PUBLIC virtual void subscribe(
    NodePtr node, const std::string & topic, const rmw_qos_profile_t qos,
    hermos::SubscriptionOptions options)
  {
    this->subscribe(node.get(), topic, qos, options);
  }

  /**
   * \brief Subscribe to a topic.
   *
   * If this Subscriber is already subscribed to a topic, this function will first unsubscribe.
   *
   * \param node The rclcpp::Node to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos The rmw qos profile to use to subscribe.
   * \param options The subscription options to use to subscribe.
   */
  HERMOS_PUBLIC virtual void subscribe(
    NodeType * node, const std::string & topic, const rmw_qos_profile_t qos,
    hermos::SubscriptionOptions options)
  {
    (void)options;
    subscribe(node, topic, qos);
  }

  /**
   * \brief Re-subscribe to a topic.  Only works if this subscriber has previously been subscribed
   * to a topic.
   */
  HERMOS_PUBLIC virtual void subscribe() = 0;

  /**
   * \brief Force immediate unsubscription of this subscriber from its topic
   */
  HERMOS_PUBLIC virtual void unsubscribe() = 0;
};

template <typename T>
using SubscriberBasePtr = std::shared_ptr<SubscriberBase<T>>;

/**
 * \brief Hermos subscription filter.
 *
 * This class acts as a highest-level filter, simply passing messages from an hermos subscription
 * through to the filters which have connected to it.
 *
 * When this object is destroyed it will unsubscribe from the hermos subscription.
 *
 * The Subscriber object is templated on the type of message being subscribed to.
 *
 * \section connections CONNECTIONS
 *
 * Subscriber has no input connection.
 *
 * The output connection for the Subscriber object is the same signature as for hermos
 * subscription callbacks, ie.
\verbatim
void callback(const hermos::ipc_shared_ptr<M const>&);
\endverbatim
 */
HERMOS_PUBLIC
template <class M, class NodeType = rclcpp::Node>
class Subscriber : public SubscriberBase<NodeType>, public SimpleFilter<M>
{
public:
  using NodePtr = std::shared_ptr<NodeType>;
  using MConstPtr = ipc_shared_ptr<M const>;
  using EventType = MessageEvent<M const>;

  /**
   * \brief Constructor
   *
   * See the hermos::create_subscription() variants for more information on the parameters
   *
   * \param node The rclcpp::Node::SharedPtr to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos (optional) The rmw qos profile to use to subscribe.
   */
  HERMOS_PUBLIC Subscriber(
    // cppcheck-suppress passedByValue  // shared_ptr by-value is intentional (shared ownership)
    NodePtr node, const std::string & topic,
    const rmw_qos_profile_t qos = rmw_qos_profile_default)  // NOLINT
  {
    subscribe(node, topic, qos);
  }

  HERMOS_PUBLIC Subscriber(
    NodeType * node, const std::string & topic,
    const rmw_qos_profile_t qos = rmw_qos_profile_default)  // NOLINT
  {
    subscribe(node, topic, qos);
  }

  /**
   * \brief Constructor
   *
   * See the hermos::create_subscription() variants for more information on the parameters
   *
   * \param node The rclcpp::Node::SharedPtr to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos The rmw qos profile to use to subscribe.
   * \param options The subscription options to use to subscribe.
   */
  HERMOS_PUBLIC Subscriber(
    // cppcheck-suppress passedByValue  // shared_ptr by-value is intentional (shared ownership)
    NodePtr node, const std::string & topic, const rmw_qos_profile_t qos,
    hermos::SubscriptionOptions options)
  {
    subscribe(node, topic, qos, options);
  }

  HERMOS_PUBLIC Subscriber(
    NodeType * node, const std::string & topic, const rmw_qos_profile_t qos,
    hermos::SubscriptionOptions options)
  {
    subscribe(node, topic, qos, options);
  }

  /**
   * \brief Empty constructor, use subscribe() to subscribe to a topic
   */
  HERMOS_PUBLIC Subscriber() = default;

  ~Subscriber() override { unsubscribe(); }

  /**
   * \brief Subscribe to a topic.
   *
   * If this Subscriber is already subscribed to a topic, this function will first unsubscribe.
   *
   * \param node The rclcpp::Node::SharedPtr to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos (optional) The rmw qos profile to use to subscribe.
   */
  // cppcheck-suppress virtualCallInConstructor  // Subscriber is not intended to be derived
  HERMOS_PUBLIC void subscribe(
    NodePtr node, const std::string & topic,
    const rmw_qos_profile_t qos = rmw_qos_profile_default) override
  {
    subscribe(node.get(), topic, qos, hermos::SubscriptionOptions{});
    node_raw_ = nullptr;
    node_shared_ = node;
  }

  /**
   * \brief Subscribe to a topic.
   *
   * If this Subscriber is already subscribed to a topic, this function will first unsubscribe.
   *
   * \param node The rclcpp::Node to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos (optional) The rmw qos profile to use to subscribe.
   */
  // cppcheck-suppress virtualCallInConstructor  // Subscriber is not intended to be derived
  HERMOS_PUBLIC void subscribe(
    NodeType * node, const std::string & topic,
    const rmw_qos_profile_t qos = rmw_qos_profile_default) override
  {
    subscribe(node, topic, qos, hermos::SubscriptionOptions{});
  }

  /**
   * \brief Subscribe to a topic.
   *
   * If this Subscriber is already subscribed to a topic, this function will first unsubscribe.
   *
   * \param node The rclcpp::Node::SharedPtr to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos The rmw qos profile to use to subscribe.
   * \param options The subscription options to use to subscribe.
   */
  // cppcheck-suppress virtualCallInConstructor  // Subscriber is not intended to be derived
  HERMOS_PUBLIC void subscribe(
    NodePtr node, const std::string & topic, const rmw_qos_profile_t qos,
    hermos::SubscriptionOptions options) override
  {
    subscribe(node.get(), topic, qos, options);
    node_raw_ = nullptr;
    node_shared_ = node;
  }

  /**
   * \brief Subscribe to a topic.
   *
   * If this Subscriber is already subscribed to a topic, this function will first unsubscribe.
   *
   * \param node The rclcpp::Node to use to subscribe.
   * \param topic The topic to subscribe to.
   * \param qos The rmw qos profile to use to subscribe.
   * \param options The subscription options to use to subscribe.
   */
  // cppcheck-suppress virtualCallInConstructor  // Subscriber is not intended to be derived
  HERMOS_PUBLIC void subscribe(
    NodeType * node, const std::string & topic, const rmw_qos_profile_t qos,
    hermos::SubscriptionOptions options) override
  {
    unsubscribe();

    if (!topic.empty()) {
      topic_ = topic;
      qos_ = qos;
      options_ = options;
      sub_ = std::make_shared<Subscription<M>>(
        node, topic, detail::to_rclcpp_qos(qos),
        [this](ipc_shared_ptr<M> msg) { this->cb(std::move(msg)); }, options);
      node_raw_ = node;
    }
  }

  /**
   * \brief Re-subscribe to a topic.  Only works if this subscriber has previously been subscribed
   * to a topic.
   */
  HERMOS_PUBLIC void subscribe() override
  {
    if (!topic_.empty()) {
      if (node_raw_ != nullptr) {
        subscribe(node_raw_, topic_, qos_, options_);
      } else if (node_shared_ != nullptr) {
        subscribe(node_shared_, topic_, qos_, options_);
      }
    }
  }

  /**
   * \brief Force immediate unsubscription of this subscriber from its topic
   */
  // cppcheck-suppress virtualCallInConstructor  // Subscriber is not intended to be derived
  HERMOS_PUBLIC void unsubscribe() override { sub_.reset(); }

  /// Return the topic name this subscriber is subscribed to.
  /// @return Topic name string.
  HERMOS_PUBLIC std::string getTopic() const { return topic_; }

  /**
   * \brief Returns the internal hermos::Subscription<M>::SharedPtr object
   */
  HERMOS_PUBLIC const typename hermos::Subscription<M>::SharedPtr getSubscriber() const
  {
    return sub_;
  }

  /// No-op. Provided for compatibility with message_filters::Chain.
  template <typename F>
  HERMOS_PUBLIC void connectInput(F & f)
  {
    (void)f;
  }

  /// No-op. Provided for compatibility with message_filters::Chain.
  HERMOS_PUBLIC void add(const EventType & e) { (void)e; }

private:
  void cb(ipc_shared_ptr<M> msg) { this->signalMessage(EventType(std::move(msg))); }

  typename hermos::Subscription<M>::SharedPtr sub_;

  NodePtr node_shared_;
  NodeType * node_raw_{nullptr};

  std::string topic_;
  rmw_qos_profile_t qos_{rmw_qos_profile_default};
  hermos::SubscriptionOptions options_;
};

}  // namespace message_filters
}  // namespace hermos
