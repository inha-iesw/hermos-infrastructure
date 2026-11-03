#include "hermos/node/hermos_context.hpp"

#include <gtest/gtest.h>
#include <rcl/error_handling.h>
#include <rcl/logging.h>
#include <rcutils/logging.h>
#include <rcutils/logging_macros.h>

#include <stdexcept>

// =========================================
// hermos::Context --disable-stdout-logs tests
// =========================================
// Note: despite the flag name, rcutils/rcl's default console handler routes all output to stderr
// when stdout is not a TTY (as is the case inside a test process). Tests therefore capture stderr.

class HermosContextStdoutLogsTest : public ::testing::Test
{
protected:
  void SetUp() override { original_handler_ = rcutils_logging_get_output_handler(); }

  void TearDown() override
  {
    // Finalize rcl logging so the next test can call rcl_logging_configure again.
    rcl_ret_t ret = rcl_logging_fini();
    (void)ret;
    // Restore the original handler so other tests are not affected.
    rcutils_logging_set_output_handler(original_handler_);
  }

  rcutils_logging_output_handler_t original_handler_;
};

TEST_F(HermosContextStdoutLogsTest, disable_stdout_logs_suppresses_console_output)
{
  const char * argv[] = {"program", "--ros-args", "--disable-stdout-logs", "--log-level", "debug"};
  int argc = 5;
  hermos::Context ctx;
  ctx.init(argc, argv);

  testing::internal::CaptureStderr();
  RCUTILS_LOG_INFO_NAMED("hermos_test", "test message");

  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_TRUE(output.empty()) << "Expected no stdout output with --disable-stdout-logs, got: "
                              << output;
}

TEST_F(HermosContextStdoutLogsTest, no_flag_emits_console_output)
{
  const char * argv[] = {"program", "--ros-args", "--log-level", "debug"};
  int argc = 4;
  hermos::Context ctx;
  ctx.init(argc, argv);

  testing::internal::CaptureStderr();
  RCUTILS_LOG_INFO_NAMED("hermos_test", "test message");

  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(output.empty()) << "Expected stdout output without --disable-stdout-logs";
}

TEST_F(HermosContextStdoutLogsTest, flag_outside_ros_args_is_ignored)
{
  // --disable-stdout-logs appears before --ros-args, so it is not a ROS argument
  const char * argv[] = {"program", "--disable-stdout-logs", "--ros-args", "--log-level", "debug"};
  int argc = 5;
  hermos::Context ctx;
  ctx.init(argc, argv);

  testing::internal::CaptureStderr();
  RCUTILS_LOG_INFO_NAMED("hermos_test", "test message");

  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(output.empty()) << "Flag outside --ros-args must be ignored; expected stdout output";
}

TEST_F(HermosContextStdoutLogsTest, flag_after_double_dash_terminator_is_ignored)
{
  // --disable-stdout-logs appears after -- (end of ROS args), so it is not a ROS argument
  const char * argv[] = {"program", "--ros-args", "--log-level",
                         "debug",   "--",         "--disable-stdout-logs"};
  int argc = 6;
  hermos::Context ctx;
  ctx.init(argc, argv);

  testing::internal::CaptureStderr();
  RCUTILS_LOG_INFO_NAMED("hermos_test", "test message");

  const std::string output = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(output.empty()) << "Flag after -- must be ignored; expected stdout output";
}

namespace
{
void host_output_handler(
  const rcutils_log_location_t *, int, const char *, rcutils_time_point_value_t, const char *,
  va_list *)
{
}
}  // namespace

class HermosContextLoggingOwnershipTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    hermos::shutdown();
    original_handler_ = rcutils_logging_get_output_handler();
    auto arguments = hermos::parse_arguments({});
    auto allocator = rcl_get_default_allocator();
    ASSERT_EQ(
      rcl_logging_configure_with_output_handler(
        arguments.get(), &allocator, rcl_logging_multiple_output_handler),
      RCL_RET_OK);
    rcutils_logging_set_output_handler(host_output_handler);
  }

  void TearDown() override
  {
    hermos::shutdown();
    EXPECT_EQ(rcl_logging_fini(), RCL_RET_OK);
    rcl_reset_error();
    rcutils_logging_set_output_handler(original_handler_);
  }

  hermos::InitOptions embedded_options()
  {
    hermos::InitOptions options;
    options.configure_logging = false;
    return options;
  }

  rcutils_logging_output_handler_t original_handler_;
};

TEST_F(HermosContextLoggingOwnershipTest, shutdown_without_init_preserves_host_logging)
{
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);
}

TEST_F(HermosContextLoggingOwnershipTest, embedded_lifetime_preserves_host_logging)
{
  hermos::init(0, nullptr, embedded_options());
  EXPECT_TRUE(hermos::ok());
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);

  // A repeated init must not replace the first caller's logging policy.
  hermos::init(0, nullptr);
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);
  hermos::shutdown();
  EXPECT_FALSE(hermos::ok());
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);
}

TEST_F(HermosContextLoggingOwnershipTest, default_init_owns_logging_until_shutdown)
{
  hermos::init(0, nullptr);
  EXPECT_EQ(rcutils_logging_get_output_handler(), rcl_logging_multiple_output_handler);
  hermos::init(0, nullptr, embedded_options());
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), rcutils_logging_console_output_handler);

  rcutils_logging_set_output_handler(host_output_handler);
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);
}

TEST_F(HermosContextLoggingOwnershipTest, stopped_context_still_finalizes_owned_logging)
{
  hermos::init(0, nullptr);
  {
    // The signal processing thread invalidates the context before explicit shutdown.
    std::lock_guard<std::mutex> lock(hermos::g_context_mtx);
    hermos::g_context.shutdown();
  }
  EXPECT_FALSE(hermos::ok());
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), rcutils_logging_console_output_handler);
}

TEST_F(HermosContextLoggingOwnershipTest, parse_failure_preserves_host_logging_and_allows_retry)
{
  const char * argv[] = {"program", "--ros-args", "--log-level"};
  EXPECT_THROW(hermos::init(3, argv, embedded_options()), std::runtime_error);
  rcl_reset_error();
  EXPECT_FALSE(hermos::ok());
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);

  hermos::init(0, nullptr, embedded_options());
  EXPECT_TRUE(hermos::ok());
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);
}

TEST_F(HermosContextLoggingOwnershipTest, new_lifetime_can_change_logging_policy)
{
  hermos::init(0, nullptr, embedded_options());
  hermos::shutdown();
  hermos::init(0, nullptr);
  EXPECT_EQ(rcutils_logging_get_output_handler(), rcl_logging_multiple_output_handler);
  hermos::shutdown();
  rcutils_logging_set_output_handler(host_output_handler);
  hermos::init(0, nullptr, embedded_options());
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);
}

TEST_F(HermosContextLoggingOwnershipTest, failed_default_init_does_not_acquire_host_logging)
{
  const char * argv[] = {"program", "--ros-args", "--log-level"};
  EXPECT_THROW(hermos::init(3, argv), std::runtime_error);
  rcl_reset_error();
  EXPECT_FALSE(hermos::ok());
  hermos::shutdown();
  EXPECT_EQ(rcutils_logging_get_output_handler(), host_output_handler);
}
