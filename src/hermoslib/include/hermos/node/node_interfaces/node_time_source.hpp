#pragma once

#include "hermos/hermos_subscription.hpp"
#include "hermos/node/hermos_only_single_threaded_executor.hpp"
#include "builtin_interfaces/msg/time.hpp"
#include "rclcpp/logging.hpp"
#include "rclcpp/node_interfaces/node_base_interface.hpp"
#include "rclcpp/node_interfaces/node_clock_interface.hpp"
#include "rclcpp/node_interfaces/node_parameters_interface.hpp"
#include "rclcpp/node_interfaces/node_time_source_interface.hpp"
#include "rclcpp/qos.hpp"

#include "rosgraph_msgs/msg/clock.hpp"

#include <mutex>
#include <thread>

namespace hermos
{
class Node;
}

namespace hermos::node_interfaces
{

class NodeTimeSource : public rclcpp::node_interfaces::NodeTimeSourceInterface
{
public:
  using SharedPtr = std::shared_ptr<NodeTimeSource>;
  using WeakPtr = std::weak_ptr<NodeTimeSource>;

  NodeTimeSource(
    const rclcpp::node_interfaces::NodeClockInterface::SharedPtr & node_clock,
    hermos::Node * node, const rclcpp::QoS & qos = rclcpp::ClockQoS(),
    bool use_clock_thread = true);

  ~NodeTimeSource() override;

private:
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base_;
  rclcpp::node_interfaces::NodeParametersInterface::SharedPtr node_parameters_;
  hermos::Node * hermos_node_{nullptr};

  bool use_clock_thread_;
  rclcpp::QoS qos_;
  hermos::Subscription<rosgraph_msgs::msg::Clock>::SharedPtr clock_subscription_;
  rclcpp::CallbackGroup::SharedPtr clock_callback_group_;
  rclcpp::Clock::SharedPtr clock_;
  bool ros_time_active_{false};
  std::mutex clock_sub_lock_;
  rclcpp::Logger logger_{rclcpp::get_logger("hermos")};

  std::unique_ptr<HermosOnlySingleThreadedExecutor> clock_executor_;
  std::thread clock_executor_thread_;

  void enable_ros_time();
  void disable_ros_time();
  void set_clock(const builtin_interfaces::msg::Time & msg, bool set_ros_time_enabled);
  void create_clock_sub();
  void destroy_clock_sub();
  void clock_cb(const hermos::ipc_shared_ptr<rosgraph_msgs::msg::Clock> & msg);

  void attachNode(hermos::Node * node);
  void detachNode();
  void attachClock(rclcpp::Clock::SharedPtr clock);
};

}  // namespace hermos::node_interfaces
