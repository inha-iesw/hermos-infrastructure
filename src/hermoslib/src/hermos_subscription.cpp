#include "hermos/bridge/hermos_bridge_node.hpp"
#include "hermos/internal/type_registry_writer.hpp"
#include "hermos/node/hermos_node.hpp"
#include "ipc/access.hpp"
#include "rclcpp/typesupport_helpers.hpp"
#include "rclcpp/version.h"
#include "rcpputils/shared_library.hpp"

#include <rmw/rmw.h>
#include <rmw/serialized_message.h>

#include <system_error>

namespace hermos
{

detail::SubscriberRegistration::~SubscriberRegistration()
{
  if (id < 0) {
    return;
  }
  // Pool mappings can be shared by other subscriptions and remain process-owned.
  ioctl_remove_subscriber_args args{};
  args.topic_name = {topic_name.c_str(), topic_name.size()};
  args.subscriber_id = id;
  if (ioctl(hermos_fd, HERMOS_REMOVE_SUBSCRIBER_CMD, &args) < 0) {
    RCLCPP_WARN(logger, "Failed to remove subscriber (id=%d) from kernel.", id);
  }
}

int detail::SubscriptionNotification::open(
  const std::string & topic_name, const std::string & mq_topic_name, topic_local_id_t id)
{
  ioctl_remove_subscriber_args args{};
  args.topic_name = {topic_name.c_str(), topic_name.size()};
  args.subscriber_id = id;
  const int fd = ioctl(hermos_fd, HERMOS_OPEN_SUBSCRIBER_NOTIFY_FD_CMD, &args);
  if (fd >= 0) {
    mq.first = fd;
    return fd;
  }
  // Older HERM0S drivers return EINVAL for an unknown command. A supported bind
  // reports endpoint and resource errors separately so they can reach the caller.
  if (errno != EINVAL && errno != ENOTTY) {
    throw std::system_error(errno, std::generic_category(), "open subscriber notification fd");
  }
  detail::require_notification_token(mq_topic_name);
  RCLCPP_WARN(logger, "Subscriber on '%s' uses generation-scoped MQ notifications because "
    "the kernel module does not support notification fds.", topic_name.c_str());
  return open_mq_for_subscription(mq_topic_name, id, mq);
}

detail::SubscriptionNotification::~SubscriptionNotification()
{
  if (is_driver_fd()) {
    close(mq.first);
  } else if (mq.first != -1) {
    remove_mq(mq);
  }
}

SubscriptionBase::SubscriptionBase(rclcpp::Node * node, const std::string & topic_name)
: topic_name_(node->get_node_topics_interface()->resolve_topic_name(topic_name)),
  registration_(std::make_shared<detail::SubscriberRegistration>(topic_name_))
{
  validate_ld_preload();
}

SubscriptionBase::SubscriptionBase(
  hermos::Node * node, const std::string & topic_name)  // NOLINT(modernize-pass-by-value)
: topic_name_(node->get_node_topics_interface()->resolve_topic_name(topic_name)),
  registration_(std::make_shared<detail::SubscriberRegistration>(topic_name_))
{
  validate_ld_preload();
}

void SubscriptionBase::initialize(
  const rclcpp::QoS & qos, const bool is_take_sub, const bool ignore_local_publications,
  SubscriptionRole role, const std::string & node_name, const std::string & type_name)
{
  // Announce to the per-IPC-namespace discovery agent before the kmod call so
  // the registry line is in place whenever a later snapshot sees the
  // ioctl-side endpoint. Empty `type_name` (e.g. service types) skips this.
  if (!type_name.empty()) {
    internal::TypeRegistryWriter::instance().register_type(
      topic_name_, type_name, "sub", node_name);
  }

  union ioctl_add_subscriber_args add_subscriber_args = {};
  add_subscriber_args.topic_name = {topic_name_.c_str(), topic_name_.size()};
  add_subscriber_args.node_name = {node_name.c_str(), node_name.size()};
  add_subscriber_args.qos_depth = static_cast<uint32_t>(qos.depth());
  add_subscriber_args.qos_is_transient_local =
    qos.durability() == rclcpp::DurabilityPolicy::TransientLocal;
  add_subscriber_args.qos_is_reliable = qos.reliability() == rclcpp::ReliabilityPolicy::Reliable;
  add_subscriber_args.is_take_sub = is_take_sub;
  add_subscriber_args.ignore_local_publications = ignore_local_publications;
  add_subscriber_args.is_bridge = (role == SubscriptionRole::BridgeInternal);
  if (ioctl(hermos_fd, HERMOS_ADD_SUBSCRIBER_CMD, &add_subscriber_args) < 0) {
    RCLCPP_ERROR(logger, "HERMOS_ADD_SUBSCRIBER_CMD failed: %s", strerror(errno));
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  id_ = add_subscriber_args.ret_id;
  registration_->id = id_;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
  mq_topic_name_ = add_subscriber_args.ret_mq_topic_name;

  if (role == SubscriptionRole::Default) {
    if (!type_name.empty()) {
      register_pubsub_bridge_by_type_name(
        topic_name_, id_, type_name, BridgeDirection::ROS2_TO_HERMOS);
    } else {
      RCLCPP_ERROR(
        logger,
        "R2A bridge registration is skipped because the type_name is empty (topic: '%s'). "
        "Please make sure to specify the valid message type in normal use case.",
        topic_name_.c_str());
    }
  }
}

template <typename NodeT>
rclcpp::QoS SubscriptionBase::init_base(
  NodeT * node, const rclcpp::QoS & qos, const std::string & type_name, bool is_take_sub,
  const SubscriptionOptions & options, SubscriptionRole role)
{
  const bool override_qos = !options.qos_overriding_options.get_policy_kinds().empty();
  rclcpp::node_interfaces::NodeParametersInterface::SharedPtr node_parameters =
    override_qos ? node->get_node_parameters_interface() : nullptr;
  const rclcpp::QoS actual_qos = override_qos
                                   ? rclcpp::detail::declare_qos_parameters(
                                       options.qos_overriding_options, node_parameters, topic_name_,
                                       qos, rclcpp::detail::SubscriptionQosParametersTraits{})
                                   : qos;

  validate_subscription_qos(actual_qos);

  const std::string node_name = node->get_fully_qualified_name();
  initialize(
    actual_qos, is_take_sub, options.ignore_local_publications, role, node_name, type_name);

  return actual_qos;
}

template rclcpp::QoS SubscriptionBase::init_base<rclcpp::Node>(
  rclcpp::Node *, const rclcpp::QoS &, const std::string &, bool, const SubscriptionOptions &,
  SubscriptionRole);

template rclcpp::QoS SubscriptionBase::init_base<hermos::Node>(
  hermos::Node *, const rclcpp::QoS &, const std::string &, bool, const SubscriptionOptions &,
  SubscriptionRole);

uint32_t get_publisher_count_core(const std::string & topic_name)
{
  union ioctl_get_publisher_num_args args = {};
  args.topic_name = {topic_name.c_str(), topic_name.size()};
  if (ioctl(hermos_fd, HERMOS_GET_PUBLISHER_NUM_CMD, &args) < 0) {
    RCLCPP_ERROR(logger, "HERMOS_GET_PUBLISHER_NUM_CMD failed: %s", strerror(errno));
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  uint32_t count = args.ret_publisher_num;
  // If an R2A bridge exists, exclude the hermos publisher created by the bridge
  if (args.ret_r2a_bridge_exist && count > 0) {
    count--;
  }

  uint32_t ros2_count = args.ret_ros2_publisher_num;
  // If an A2R bridge exists, exclude the ROS 2 publisher created by the bridge
  if (args.ret_a2r_bridge_exist && ros2_count > 0) {
    ros2_count--;
  }

  return count + ros2_count;
}

mqd_t open_mq_for_subscription(
  const std::string & topic_name, const topic_local_id_t subscriber_id,
  std::pair<mqd_t, std::string> & mq_subscription)
{
  std::string mq_name = create_mq_name_for_hermos_publish(topic_name, subscriber_id);
  struct mq_attr attr = {};
  attr.mq_flags = 0;                        // Blocking queue
  attr.mq_msgsize = sizeof(MqMsgHermos);  // Maximum message size
  attr.mq_curmsgs = 0;  // Number of messages currently in the queue (not set by mq_open)
  attr.mq_maxmsg = 1;

  const int mq_mode = 0600;
  mqd_t mq = mq_open(mq_name.c_str(), O_CREAT | O_EXCL | O_RDONLY | O_NONBLOCK | O_CLOEXEC, mq_mode, &attr);
  if (mq == -1) {
    RCLCPP_ERROR_STREAM(
      logger, "mq_open failed for topic '" << topic_name << "' (subscriber_id=" << subscriber_id
                                           << ", mq_name='" << mq_name
                                           << "'): " << strerror(errno));
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }
  mq_subscription = std::make_pair(mq, mq_name);

  return mq;
}

void remove_mq(const std::pair<mqd_t, std::string> & mq_subscription)
{
  /* The message queue is destroyed after all the publisher processes close it. */
  if (mq_close(mq_subscription.first) == -1) {
    RCLCPP_ERROR_STREAM(
      logger,
      "mq_close failed for mq_name='" << mq_subscription.second << "': " << strerror(errno));
  }
  if (mq_unlink(mq_subscription.second.c_str()) == -1) {
    RCLCPP_ERROR_STREAM(
      logger,
      "mq_unlink failed for mq_name='" << mq_subscription.second << "': " << strerror(errno));
  }
}

rclcpp::CallbackGroup::SharedPtr get_default_callback_group_for_tracepoint(hermos::Node * node)
{
  return node->get_node_base_interface()->get_default_callback_group();
}

GenericSubscription::TypeSupportBundle GenericSubscription::load_typesupport_impl(
  const std::string & topic_type)
{
  TypeSupportBundle result;
  result.library = rclcpp::get_typesupport_library(topic_type, "rosidl_typesupport_cpp");
  // rclcpp::get_typesupport_handle() was deprecated in Jazzy (rclcpp 28) in favor of
  // rclcpp::get_message_typesupport_handle() to distinguish message handles from service handles.
#if RCLCPP_VERSION_MAJOR >= 28
  result.handle =
    rclcpp::get_message_typesupport_handle(topic_type, "rosidl_typesupport_cpp", *result.library);
#else
  result.handle =
    rclcpp::get_typesupport_handle(topic_type, "rosidl_typesupport_cpp", *result.library);
#endif
  return result;
}

bool GenericSubscription::serialize_message(
  const void * raw, const rosidl_message_type_support_t * type_support,
  rclcpp::SerializedMessage & out)
{
  const rmw_ret_t ret = rmw_serialize(raw, type_support, &out.get_rcl_serialized_message());
  if (ret != RMW_RET_OK) {
    RCLCPP_ERROR(
      logger, "rmw_serialize failed (rmw_ret=%d); skipping message", static_cast<int>(ret));
    rmw_reset_error();
    return false;
  }
  return true;
}

}  // namespace hermos
