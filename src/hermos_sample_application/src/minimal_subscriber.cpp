#include "hermos/hermos.hpp"
#include "hermos_sample_interfaces/msg/dynamic_size_array.hpp"
#include "rclcpp/rclcpp.hpp"

using std::placeholders::_1;

class MinimalSubscriber : public rclcpp::Node
{
  hermos::Subscription<hermos_sample_interfaces::msg::DynamicSizeArray>::SharedPtr sub_dynamic_;

  void callback(
    const hermos::ipc_shared_ptr<hermos_sample_interfaces::msg::DynamicSizeArray> & message)
  {
    RCLCPP_INFO(this->get_logger(), "subscribe message: id=%ld", message->id);
  }

public:
  explicit MinimalSubscriber(const rclcpp::NodeOptions & options) : Node("cie_subscriber", options)
  {
    rclcpp::CallbackGroup::SharedPtr group =
      create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    hermos::SubscriptionOptions hermos_options;
    hermos_options.callback_group = group;

    sub_dynamic_ = hermos::create_subscription<hermos_sample_interfaces::msg::DynamicSizeArray>(
      this, "/my_topic", 1, std::bind(&MinimalSubscriber::callback, this, _1), hermos_options);
  }
};

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(MinimalSubscriber)
