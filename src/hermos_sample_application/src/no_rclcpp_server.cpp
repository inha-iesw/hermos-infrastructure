#include "hermos/hermos.hpp"
#include "hermos_sample_interfaces/srv/sum_int_array.hpp"

class NoRclcppServer : public hermos::Node
{
  using ServiceT = hermos_sample_interfaces::srv::SumIntArray;

  typename hermos::Service<ServiceT>::SharedPtr service_;

public:
  explicit NoRclcppServer() : Node("no_rclcpp_server")
  {
    service_ = this->create_service<ServiceT>(
      "sum_int_array", [this](
                         const hermos::ipc_shared_ptr<typename ServiceT::Request> & request,
                         const hermos::ipc_shared_ptr<typename ServiceT::Response> & response) {
        response->sum = 0;
        for (int64_t value : request->data) {
          response->sum += value;
        }
        RCLCPP_INFO(this->get_logger(), "Sending back response: [%ld]", response->sum);
      });
  }
};

int main(int argc, char * argv[])
{
  hermos::init(argc, argv);
  hermos::HermosOnlySingleThreadedExecutor executor;
  auto node = std::make_shared<NoRclcppServer>();
  executor.add_node(node);
  executor.spin();
  hermos::shutdown();
  return 0;
}
