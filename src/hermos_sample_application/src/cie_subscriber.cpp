#include "hermos/hermos.hpp"
#include "hermos_sample_interfaces/msg/dynamic_size_array.hpp"
#include "rclcpp/rclcpp.hpp"

using std::placeholders::_1;

class CieSubscriber : public rclcpp::Node
{
  hermos::Subscription<hermos_sample_interfaces::msg::DynamicSizeArray>::SharedPtr sub_dynamic_;
  hermos::Subscription<hermos_sample_interfaces::msg::DynamicSizeArray>::SharedPtr
    sub_dynamic2_;

  rclcpp::Subscription<hermos_sample_interfaces::msg::DynamicSizeArray>::SharedPtr sub_ros_;
  rclcpp::Subscription<hermos_sample_interfaces::msg::DynamicSizeArray>::SharedPtr sub_ros2_;

public:
  explicit CieSubscriber(const rclcpp::NodeOptions & options) : Node("minimal_subscriber", options)
  {
    rclcpp::CallbackGroup::SharedPtr group1 =
      create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

    rclcpp::CallbackGroup::SharedPtr group2 =
      create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

    hermos::SubscriptionOptions hermos_options;
    hermos_options.callback_group = group1;
    sub_dynamic_ = hermos::create_subscription<hermos_sample_interfaces::msg::DynamicSizeArray>(
      this, "/my_topic", 1,
      [](const hermos::ipc_shared_ptr<hermos_sample_interfaces::msg::DynamicSizeArray> &
           message) {
        RCLCPP_INFO(
          rclcpp::get_logger("hermos_sample_application"),
          "receive message id=%ld in group1 from /my_topic", message->id);
      },
      hermos_options);

    rclcpp::SubscriptionOptions ros_options;
    ros_options.callback_group = group1;
    sub_ros_ = this->create_subscription<hermos_sample_interfaces::msg::DynamicSizeArray>(
      "/ros_topic", 1,
      [](const hermos_sample_interfaces::msg::DynamicSizeArray::SharedPtr message) {
        RCLCPP_INFO(
          rclcpp::get_logger("hermos_sample_application"),
          "receive message id=%ld in group1 from /ros_topic", message->id);
      },
      ros_options);

    rclcpp::SubscriptionOptions ros_options2;
    ros_options2.callback_group = group2;
    sub_ros2_ = this->create_subscription<hermos_sample_interfaces::msg::DynamicSizeArray>(
      "/ros_other_topic", 1,
      [](const hermos_sample_interfaces::msg::DynamicSizeArray::SharedPtr message) {
        RCLCPP_INFO(
          rclcpp::get_logger("hermos_sample_application"),
          "receive message id=%ld in group2 from /ros_other_topic", message->id);
      },
      ros_options2);
  }
};

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(CieSubscriber)
