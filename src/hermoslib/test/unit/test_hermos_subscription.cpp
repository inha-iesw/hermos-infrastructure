#include "hermos/hermos_subscription.hpp"

#include <gtest/gtest.h>

class GetValidCallbackGroupTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
    node = std::make_shared<rclcpp::Node>("dummy");
  }

  void TearDown() override { rclcpp::shutdown(); }

  std::shared_ptr<rclcpp::Node> node;
};

TEST_F(GetValidCallbackGroupTest, get_valid_callback_group_normal)
{
  hermos::SubscriptionOptions options;
  options.callback_group =
    node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  auto result = hermos::get_valid_callback_group(node.get(), options);

  EXPECT_EQ(result, options.callback_group);
}

TEST_F(GetValidCallbackGroupTest, get_valid_callback_group_not_in_node)
{
  auto other_node = std::make_shared<rclcpp::Node>("other_node");
  hermos::SubscriptionOptions options;
  options.callback_group =
    other_node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  EXPECT_EXIT(
    hermos::get_valid_callback_group(node.get(), options),
    ::testing::ExitedWithCode(EXIT_FAILURE),
    "Cannot create hermos subscription, callback group not in node.");
}

TEST_F(GetValidCallbackGroupTest, get_valid_callback_group_nullptr)
{
  hermos::SubscriptionOptions options;

  auto result = hermos::get_valid_callback_group(node.get(), options);
  EXPECT_EQ(result, node->get_node_base_interface()->get_default_callback_group());
}
