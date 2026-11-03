#include "hermos/hermos_publisher.hpp"
#include "rclcpp/rclcpp.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>

extern uint32_t mock_borrowed_publisher_num;

namespace
{

struct ConstructionFailure
{
};

class ThrowingMessage
{
public:
  static inline uint32_t allocation_count = 0;
  static inline uint32_t deallocation_count = 0;

  static void * operator new(std::size_t size)
  {
    ++allocation_count;
    return ::operator new(size);
  }

  static void operator delete(void * ptr) noexcept
  {
    ++deallocation_count;
    ::operator delete(ptr);
  }

  ThrowingMessage()
  {
    throw ConstructionFailure{};
  }
};

class PublisherBorrowExceptionTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
    node_ = std::make_shared<rclcpp::Node>("borrow_exception_node");
    publisher_ = std::make_shared<hermos::Publisher<ThrowingMessage>>(
      node_.get(), "/borrow_exception", rclcpp::QoS{1}, hermos::PublisherOptions{},
      hermos::PublisherRole::HermosOnly);
    mock_borrowed_publisher_num = 0;
    ThrowingMessage::allocation_count = 0;
    ThrowingMessage::deallocation_count = 0;
  }

  void TearDown() override
  {
    mock_borrowed_publisher_num = 0;
    publisher_.reset();
    node_.reset();
    rclcpp::shutdown();
  }

  std::shared_ptr<rclcpp::Node> node_;
  std::shared_ptr<hermos::Publisher<ThrowingMessage>> publisher_;
};

TEST_F(PublisherBorrowExceptionTest, constructor_failure_rolls_back_borrow_state)
{
  EXPECT_THROW(publisher_->borrow_loaned_message(), ConstructionFailure);

  EXPECT_EQ(mock_borrowed_publisher_num, 0U);
  EXPECT_EQ(ThrowingMessage::allocation_count, 1U);
  EXPECT_EQ(ThrowingMessage::deallocation_count, 1U);
}

}  // namespace
