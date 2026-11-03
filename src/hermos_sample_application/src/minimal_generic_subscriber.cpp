#include "hermos/hermos.hpp"
#include "hermos_sample_interfaces/msg/dynamic_size_array.hpp"
#include "rclcpp/rclcpp.hpp"

#include <rclcpp/serialization.hpp>
#include <rclcpp/serialized_message.hpp>

using std::placeholders::_1;

class MinimalGenericSubscriber : public rclcpp::Node
{
  hermos::GenericSubscription::SharedPtr sub_;

  void callback(std::shared_ptr<rclcpp::SerializedMessage> serialized_msg)
  {
    rclcpp::Serialization<hermos_sample_interfaces::msg::DynamicSizeArray> serializer;
    hermos_sample_interfaces::msg::DynamicSizeArray msg;
    serializer.deserialize_message(serialized_msg.get(), &msg);

    RCLCPP_INFO(this->get_logger(), "subscribe message: id=%ld", msg.id);
  }

public:
  explicit MinimalGenericSubscriber(const rclcpp::NodeOptions & options)
  : Node("minimal_generic_subscriber", options)
  {
    rclcpp::CallbackGroup::SharedPtr group =
      create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    hermos::SubscriptionOptions hermos_options;
    hermos_options.callback_group = group;

    sub_ = hermos::create_generic_subscription(
      this, "/my_topic", "hermos_sample_interfaces/msg/DynamicSizeArray", 1,
      std::bind(&MinimalGenericSubscriber::callback, this, _1), hermos_options);
  }
};

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(MinimalGenericSubscriber)
