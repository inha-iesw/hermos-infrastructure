#include "hermos/hermos.hpp"
#include "hermos_sample_interfaces/msg/dynamic_size_array.hpp"
#include "rclcpp/rclcpp.hpp"

using namespace std::chrono_literals;
const long long MESSAGE_SIZE = 1000ll * 1024;

class MinimalPublisher : public rclcpp::Node
{
  int64_t count_;
  rclcpp::TimerBase::SharedPtr timer_;
  hermos::Publisher<hermos_sample_interfaces::msg::DynamicSizeArray>::SharedPtr
    publisher_dynamic_;

  void timer_callback()
  {
    hermos::ipc_shared_ptr<hermos_sample_interfaces::msg::DynamicSizeArray> message =
      publisher_dynamic_->borrow_loaned_message();

    message->id = count_;
    message->data.reserve(MESSAGE_SIZE / sizeof(uint64_t));
    for (size_t i = 0; i < MESSAGE_SIZE / sizeof(uint64_t); i++) {
      message->data.push_back(i + count_);
    }

    publisher_dynamic_->publish(std::move(message));
    RCLCPP_INFO(this->get_logger(), "publish message: id=%ld", count_++);
  }

public:
  MinimalPublisher() : Node("cie_publisher")
  {
    count_ = 0;

    publisher_dynamic_ =
      hermos::create_publisher<hermos_sample_interfaces::msg::DynamicSizeArray>(
        this, "/my_topic", 1);

    timer_ = this->create_wall_timer(100ms, std::bind(&MinimalPublisher::timer_callback, this));
  }
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  hermos::SingleThreadedHermosExecutor executor;
  auto node = std::make_shared<MinimalPublisher>();
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}
