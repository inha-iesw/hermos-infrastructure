// Hermos (zero-copy) subscriber for the memory evaluation.
//
// Holds the most recently received message so its shared-memory pages stay referenced
// while the driver samples memory. Because the payload lives once in the publisher's
// shared pool, every holder maps the same physical pages. Prints SUBSCRIBED (ready) and
// HELD (payload resident).

#include "hermos/hermos.hpp"
#include "hermos_sample_interfaces/msg/dynamic_size_array.hpp"
#include "rclcpp/rclcpp.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using std::placeholders::_1;
using Msg = hermos_sample_interfaces::msg::DynamicSizeArray;

class HermosHoldListener : public rclcpp::Node
{
  hermos::Subscription<Msg>::SharedPtr sub_;
  hermos::ipc_shared_ptr<Msg> held_;  // keeps the shared payload referenced

  void callback(const hermos::ipc_shared_ptr<Msg> & message)
  {
    held_ = message;  // copy-assign shares the control block (refcount++), holding the pages
    std::printf(
      "HELD id=%ld bytes=%zu\n", static_cast<long>(message->id),
      message->data.size() * sizeof(int64_t));
    std::fflush(stdout);
  }

public:
  HermosHoldListener() : Node("hermos_hold_listener")
  {
    const char * topic = std::getenv("EVAL_TOPIC");
    rclcpp::CallbackGroup::SharedPtr group =
      create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    hermos::SubscriptionOptions options;
    options.callback_group = group;
    sub_ = hermos::create_subscription<Msg>(
      this, topic ? topic : "/eval_topic", 1, std::bind(&HermosHoldListener::callback, this, _1),
      options);
    std::printf("SUBSCRIBED\n");
    std::fflush(stdout);
  }
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  hermos::SingleThreadedHermosExecutor executor;
  auto node = std::make_shared<HermosHoldListener>();
  executor.add_node(node);
  executor.spin();
  rclcpp::shutdown();
  return 0;
}
