#include "hermos/hermos.hpp"
#include "hermos_sample_interfaces/srv/sum_int_array.hpp"
#include "rclcpp/rclcpp.hpp"

class MinimalService : public rclcpp::Node
{
  using Request = hermos_sample_interfaces::srv::SumIntArray::Request;
  using Response = hermos_sample_interfaces::srv::SumIntArray::Response;

  rclcpp::CallbackGroup::SharedPtr callback_group_;
  typename hermos::Service<hermos_sample_interfaces::srv::SumIntArray>::SharedPtr service_;

public:
  explicit MinimalService() : Node("minimal_server")
  {
    callback_group_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    service_ = hermos::create_service<hermos_sample_interfaces::srv::SumIntArray>(
      this, "sum_int_array",
      [this](
        const hermos::ipc_shared_ptr<Request> & request,
        const hermos::ipc_shared_ptr<Response> & response) {
        response->sum = 0;
        for (int64_t value : request->data) {
          response->sum += value;
        }
        RCLCPP_INFO(this->get_logger(), "Sending back response: [%ld]", response->sum);
      },
      rclcpp::ServicesQoS(), callback_group_);
  }
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  hermos::CallbackIsolatedHermosExecutor executor;
  auto node = std::make_shared<MinimalService>();
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}
