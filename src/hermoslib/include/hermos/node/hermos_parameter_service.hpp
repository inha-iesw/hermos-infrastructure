#pragma once

#include "hermos/hermos_public_api.hpp"
#include "hermos/hermos_service.hpp"
#include "hermos/node/node_interfaces/node_base.hpp"
#include "hermos/node/node_interfaces/node_parameters.hpp"
#include "hermos/node/node_interfaces/node_services.hpp"
#include "rcl_interfaces/srv/describe_parameters.hpp"
#include "rcl_interfaces/srv/get_parameter_types.hpp"
#include "rcl_interfaces/srv/get_parameters.hpp"
#include "rcl_interfaces/srv/list_parameters.hpp"
#include "rcl_interfaces/srv/set_parameters.hpp"
#include "rcl_interfaces/srv/set_parameters_atomically.hpp"
#include "rclcpp/qos.hpp"

namespace hermos
{

/// @brief Manage ROS 2 parameter service endpoints for hermos::Node.
HERMOS_PUBLIC
class ParameterService
{
public:
  using SharedPtr = std::shared_ptr<ParameterService>;
  using WeakPtr = std::weak_ptr<ParameterService>;
  using UniquePtr = std::unique_ptr<ParameterService>;

  /// @brief Construct a ParameterService, registering all parameter management services.
  /// @param node The node's pointer
  /// @param node_params The node's parameters interface
  /// @param qos_profile The QoS profile for all parameter services. Defaults to `rclcpp::QoS()`.
  HERMOS_PUBLIC
  ParameterService(
    hermos::Node * node, hermos::node_interfaces::NodeParameters * node_params,
    const rclcpp::QoS & qos_profile = rclcpp::ParametersQoS());

private:
  hermos::Service<rcl_interfaces::srv::GetParameters>::SharedPtr get_parameters_service_;
  hermos::Service<rcl_interfaces::srv::GetParameterTypes>::SharedPtr get_parameter_types_service_;
  hermos::Service<rcl_interfaces::srv::SetParameters>::SharedPtr set_parameters_service_;
  hermos::Service<rcl_interfaces::srv::SetParametersAtomically>::SharedPtr
    set_parameters_atomically_service_;
  hermos::Service<rcl_interfaces::srv::DescribeParameters>::SharedPtr
    describe_parameters_service_;
  hermos::Service<rcl_interfaces::srv::ListParameters>::SharedPtr list_parameters_service_;
};

}  // namespace hermos
