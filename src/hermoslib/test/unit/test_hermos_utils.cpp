#include "hermos/hermos_utils.hpp"

#include <gtest/gtest.h>
#include <rcutils/logging.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace
{

std::string captured_log;
int captured_warn_count = 0;

void capture_log_handler(
  const rcutils_log_location_t * /*location*/, int severity, const char * name,
  rcutils_time_point_value_t /*timestamp*/, const char * format, va_list * args)
{
  char buf[1024];
  va_list args_copy;
  va_copy(args_copy, *args);
  vsnprintf(buf, sizeof(buf), format, args_copy);
  va_end(args_copy);
  const bool from_hermos = name != nullptr && std::string(name) == "Hermos";
  if (!from_hermos) {
    return;
  }
  captured_log += buf;
  captured_log += "\n";
  if (severity == RCUTILS_LOG_SEVERITY_WARN) {
    ++captured_warn_count;
  }
}

class LogCapture
{
public:
  LogCapture()
  {
    captured_log.clear();
    captured_warn_count = 0;
    previous_ = rcutils_logging_get_output_handler();
    rcutils_logging_set_output_handler(capture_log_handler);
  }
  ~LogCapture() { rcutils_logging_set_output_handler(previous_); }

private:
  rcutils_logging_output_handler_t previous_;
};

}  // namespace

TEST(HermosUtilsTest, create_mq_name_normal)
{
  EXPECT_EQ(hermos::create_mq_name_for_hermos_publish("/dummy", 0), "/hermos@dummy@0");
}

TEST(HermosUtilsTest, create_mq_name_slash_included)
{
  EXPECT_EQ(
    hermos::create_mq_name_for_hermos_publish("/dummy/dummy", 0), "/hermos@dummy_dummy@0");
}

TEST(HermosUtilsTest, create_mq_name_invalid_topic)
{
  EXPECT_EXIT(
    hermos::create_mq_name_for_hermos_publish("dummy", 0),
    ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

TEST(HermosUtilsTest, create_uds_addr_bridge_manager)
{
  const auto addr = hermos::create_uds_addr_for_bridge();
  ASSERT_FALSE(addr.empty());
  EXPECT_EQ(addr[0], '\0');
  const auto expected =
    "hermos_bridge_manager_" + std::to_string(hermos::get_self_ipc_ns_inode());
  EXPECT_EQ(addr.substr(1), expected);
}

TEST(HermosUtilsTest, validate_ld_preload_normal)
{
  setenv("LD_PRELOAD", "libhermos_heaphook.so:", 1);
  EXPECT_NO_THROW(hermos::validate_ld_preload());
  unsetenv("LD_PRELOAD");
}

TEST(HermosUtilsTest, validate_ld_preload_nothing)
{
  EXPECT_EXIT(hermos::validate_ld_preload(), ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

TEST(HermosUtilsTest, validate_ld_preload_different)
{
  setenv("LD_PRELOAD", "dummy", 1);
  EXPECT_EXIT(hermos::validate_ld_preload(), ::testing::ExitedWithCode(EXIT_FAILURE), "");
  unsetenv("LD_PRELOAD");
}

TEST(HermosUtilsTest, validate_ld_preload_suffix)
{
  setenv("LD_PRELOAD", "libhermos_heaphook.so:dummy", 1);
  EXPECT_NO_THROW(hermos::validate_ld_preload());
  unsetenv("LD_PRELOAD");
}

TEST(HermosUtilsTest, validate_ld_preload_prefix)
{
  setenv("LD_PRELOAD", "dummy:libhermos_heaphook.so", 1);
  EXPECT_NO_THROW(hermos::validate_ld_preload());
  unsetenv("LD_PRELOAD");
}

TEST(HermosUtilsTest, validate_ld_preload_only_libhermos_heaphook)
{
  setenv("LD_PRELOAD", "libhermos_heaphook.so", 1);
  EXPECT_NO_THROW(hermos::validate_ld_preload());
  unsetenv("LD_PRELOAD");
}

TEST(HermosUtilsTest, validate_publisher_qos_default_no_warning)
{
  LogCapture capture;
  hermos::validate_publisher_qos(rclcpp::QoS(10));
  EXPECT_EQ(captured_warn_count, 0) << captured_log;
}

TEST(HermosUtilsTest, validate_subscription_qos_default_no_warning)
{
  LogCapture capture;
  hermos::validate_subscription_qos(rclcpp::QoS(10));
  EXPECT_EQ(captured_warn_count, 0) << captured_log;
}

TEST(HermosUtilsTest, validate_publisher_qos_keep_all_exits)
{
  EXPECT_EXIT(
    hermos::validate_publisher_qos(rclcpp::QoS(rclcpp::KeepAll())),
    ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

TEST(HermosUtilsTest, validate_subscription_qos_keep_all_exits)
{
  EXPECT_EXIT(
    hermos::validate_subscription_qos(rclcpp::QoS(rclcpp::KeepAll())),
    ::testing::ExitedWithCode(EXIT_FAILURE), "");
}

TEST(HermosUtilsTest, validate_publisher_qos_depth_zero_warns)
{
  LogCapture capture;
  hermos::validate_publisher_qos(rclcpp::QoS(0));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("depth=0"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_subscription_qos_depth_zero_warns)
{
  LogCapture capture;
  hermos::validate_subscription_qos(rclcpp::QoS(0));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("depth=0"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_publisher_qos_best_effort_warns)
{
  LogCapture capture;
  hermos::validate_publisher_qos(rclcpp::QoS(10).best_effort());
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("BestEffort"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_subscription_qos_best_effort_silent)
{
  LogCapture capture;
  hermos::validate_subscription_qos(rclcpp::QoS(10).best_effort());
  EXPECT_EQ(captured_warn_count, 0) << captured_log;
}

TEST(HermosUtilsTest, validate_publisher_qos_avoid_ros_namespace_conventions_warns)
{
  LogCapture capture;
  hermos::validate_publisher_qos(rclcpp::QoS(10).avoid_ros_namespace_conventions(true));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("avoid_ros_namespace_conventions"), std::string::npos)
    << captured_log;
}

TEST(HermosUtilsTest, validate_subscription_qos_avoid_ros_namespace_conventions_warns)
{
  LogCapture capture;
  hermos::validate_subscription_qos(rclcpp::QoS(10).avoid_ros_namespace_conventions(true));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("avoid_ros_namespace_conventions"), std::string::npos)
    << captured_log;
}

TEST(HermosUtilsTest, validate_publisher_qos_deadline_warns)
{
  LogCapture capture;
  hermos::validate_publisher_qos(rclcpp::QoS(10).deadline(rclcpp::Duration(1, 0)));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("deadline"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_subscription_qos_deadline_warns)
{
  LogCapture capture;
  hermos::validate_subscription_qos(rclcpp::QoS(10).deadline(rclcpp::Duration(1, 0)));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("deadline"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_publisher_qos_lifespan_warns)
{
  LogCapture capture;
  hermos::validate_publisher_qos(rclcpp::QoS(10).lifespan(rclcpp::Duration(1, 0)));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("lifespan"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_subscription_qos_lifespan_warns)
{
  LogCapture capture;
  hermos::validate_subscription_qos(rclcpp::QoS(10).lifespan(rclcpp::Duration(1, 0)));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("lifespan"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_publisher_qos_liveliness_manual_by_topic_warns)
{
  LogCapture capture;
  hermos::validate_publisher_qos(
    rclcpp::QoS(10).liveliness(rclcpp::LivelinessPolicy::ManualByTopic));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("ManualByTopic"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_subscription_qos_liveliness_manual_by_topic_warns)
{
  LogCapture capture;
  hermos::validate_subscription_qos(
    rclcpp::QoS(10).liveliness(rclcpp::LivelinessPolicy::ManualByTopic));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("ManualByTopic"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_publisher_qos_liveliness_lease_duration_warns)
{
  LogCapture capture;
  hermos::validate_publisher_qos(
    rclcpp::QoS(10).liveliness_lease_duration(rclcpp::Duration(1, 0)));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("liveliness_lease_duration"), std::string::npos) << captured_log;
}

TEST(HermosUtilsTest, validate_subscription_qos_liveliness_lease_duration_warns)
{
  LogCapture capture;
  hermos::validate_subscription_qos(
    rclcpp::QoS(10).liveliness_lease_duration(rclcpp::Duration(1, 0)));
  EXPECT_EQ(captured_warn_count, 1);
  EXPECT_NE(captured_log.find("liveliness_lease_duration"), std::string::npos) << captured_log;
}
