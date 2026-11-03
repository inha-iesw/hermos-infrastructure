#include "hermos/hermos.hpp"
#include "hermos_sample_interfaces/msg/dynamic_size_array.hpp"

#include <chrono>

using namespace std::chrono_literals;

class NoRclcppTakeSubscriber : public hermos::Node
{
  hermos::PollingSubscriber<hermos_sample_interfaces::msg::DynamicSizeArray>::SharedPtr sub_;
  hermos::TimerBase::SharedPtr timer_;

  void timer_callback()
  {
    auto message = sub_->take_data();
    if (message) {
      RCLCPP_INFO(
        get_logger(), "I heard dynamic size array message with id: %ld, size: %zu", message->id,
        message->data.size());
    }
  }

public:
  explicit NoRclcppTakeSubscriber() : hermos::Node("no_rclcpp_take_subscriber")
  {
    sub_ = this->create_subscription<hermos_sample_interfaces::msg::DynamicSizeArray>(
      "/my_topic", rclcpp::QoS(rclcpp::KeepLast(1)));

    timer_ = this->create_wall_timer(1s, std::bind(&NoRclcppTakeSubscriber::timer_callback, this));

    RCLCPP_INFO(get_logger(), "NoRclcppTakeSubscriber started");
  }
};

int main(int argc, char ** argv)
{
  hermos::init(argc, argv);
  hermos::HermosOnlySingleThreadedExecutor executor;
  auto node = std::make_shared<NoRclcppTakeSubscriber>();
  executor.add_node(node);
  executor.spin();
  return 0;
}
