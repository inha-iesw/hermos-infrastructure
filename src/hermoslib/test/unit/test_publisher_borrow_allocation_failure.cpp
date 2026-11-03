#include "hermos/hermos_publisher.hpp"
#include "rclcpp/rclcpp.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

namespace
{

thread_local bool fail_next_scalar_new = false;
uint32_t borrowed_publisher_num = 0;

}  // namespace

void * operator new(std::size_t size)
{
  if (fail_next_scalar_new) {
    fail_next_scalar_new = false;
    throw std::bad_alloc{};
  }

  size = size == 0 ? 1 : size;
  if (void * ptr = std::malloc(size)) {
    return ptr;
  }
  throw std::bad_alloc{};
}

void operator delete(void * ptr) noexcept
{
  std::free(ptr);
}

void operator delete(void * ptr, std::size_t) noexcept
{
  std::free(ptr);
}

extern "C" uint32_t hermos_get_borrowed_publisher_num()
{
  return borrowed_publisher_num;
}

namespace hermos
{

void increment_borrowed_publisher_num()
{
  ++borrowed_publisher_num;
}

void decrement_borrowed_publisher_num()
{
  if (borrowed_publisher_num > 0) {
    --borrowed_publisher_num;
  }
}

topic_local_id_t initialize_publisher(
  const std::string & topic_name, const std::string &, const rclcpp::QoS &, bool,
  const std::string &, std::string & out_mq_topic_name)
{
  out_mq_topic_name = topic_name;
  return 0;
}

PublisherBase::~PublisherBase() = default;

}  // namespace hermos

namespace
{

class FailControlBlockAllocationMessage
{
public:
  static inline uint32_t live_count = 0;

  FailControlBlockAllocationMessage()
  {
    ++live_count;
    fail_next_scalar_new = true;
  }

  ~FailControlBlockAllocationMessage()
  {
    --live_count;
  }
};

class PublisherBorrowAllocationFailureTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
    node_ = std::make_shared<rclcpp::Node>("borrow_allocation_failure_node");
    publisher_ = std::make_shared<hermos::Publisher<FailControlBlockAllocationMessage>>(
      node_.get(), "/x", rclcpp::QoS{1}, hermos::PublisherOptions{},
      hermos::PublisherRole::HermosOnly);
    borrowed_publisher_num = 0;
    FailControlBlockAllocationMessage::live_count = 0;
  }

  void TearDown() override
  {
    fail_next_scalar_new = false;
    borrowed_publisher_num = 0;
    publisher_.reset();
    node_.reset();
    rclcpp::shutdown();
  }

  std::shared_ptr<rclcpp::Node> node_;
  std::shared_ptr<hermos::Publisher<FailControlBlockAllocationMessage>> publisher_;
};

TEST_F(PublisherBorrowAllocationFailureTest, control_block_failure_releases_message_and_state)
{
  EXPECT_THROW(publisher_->borrow_loaned_message(), std::bad_alloc);

  EXPECT_EQ(borrowed_publisher_num, 0U);
  EXPECT_EQ(FailControlBlockAllocationMessage::live_count, 0U);
}

}  // namespace
