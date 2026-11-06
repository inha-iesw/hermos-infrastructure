// Copyright 2026
// SPDX-License-Identifier: Apache-2.0
#include "hermos_ioctl.hpp"
#include "device/path.hpp"

#include <gtest/gtest.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdarg>
#include <cerrno>
#include <cstdlib>
#include <string>

extern "C" int hermos_query_endpoint_stats_v1(
  const char *, uint32_t, bool, endpoint_stats_ret *, uint32_t *);
extern "C" char ** get_hermos_topics(int *, uint32_t **);

namespace
{
bool intercept_stats = false;
int stats_error = 0;
uint32_t stats_count = 0;
}  // namespace

// Interpose only the statistics command; other ioctl calls retain their real behavior.
extern "C" int ioctl(int fd, unsigned long request, ...) noexcept
{
  va_list arguments;
  va_start(arguments, request);
  void * argument = va_arg(arguments, void *);
  va_end(arguments);
  if (intercept_stats && request == HERMOS_GET_ENDPOINT_STATS_CMD) {
    static_cast<ioctl_endpoint_stats_args *>(argument)->ret_count = stats_count;
    errno = stats_error;
    return stats_error ? -1 : 0;
  }
  return static_cast<int>(syscall(SYS_ioctl, fd, request, argument));
}

namespace
{
class EndpointStatsTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    const char * value = std::getenv("HERMOS_DEVICE_PATH");
    was_set_ = value != nullptr;
    original_ = value ? value : "";
  }
  void TearDown() override
  {
    intercept_stats = false;
    if (was_set_) setenv("HERMOS_DEVICE_PATH", original_.c_str(), 1);
    else unsetenv("HERMOS_DEVICE_PATH");
  }
  bool was_set_{};
  std::string original_;
};

TEST_F(EndpointStatsTest, ExplicitDeviceIsUsedWithoutFallback)
{
  setenv("HERMOS_DEVICE_PATH", "/dev/hermos-e3-does-not-exist", 1);
  EXPECT_STREQ(hermos_device_path(), "/dev/hermos-e3-does-not-exist");
  endpoint_stats_ret row{};
  uint32_t capacity = 1;
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 17, false, &row, &capacity), -ENOENT);
}

TEST_F(EndpointStatsTest, UnsetAndEmptyDeviceKeepTheExistingDefault)
{
  unsetenv("HERMOS_DEVICE_PATH");
  EXPECT_STREQ(hermos_device_path(), "/dev/hermos");
  setenv("HERMOS_DEVICE_PATH", "", 1);
  EXPECT_STREQ(hermos_device_path(), "/dev/hermos");
}

TEST_F(EndpointStatsTest, UnsupportedDevicePreservesErrnoAsNegativeResult)
{
  setenv("HERMOS_DEVICE_PATH", "/dev/null", 1);
  endpoint_stats_ret row{};
  uint32_t capacity = 1;
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 17, false, &row, &capacity), -ENOTTY);
}

TEST_F(EndpointStatsTest, TopicListingDistinguishesUnavailableDeviceFromEmptyGraph)
{
  setenv("HERMOS_DEVICE_PATH", "/dev/hermos-e3-does-not-exist", 1);
  int count = 42;
  uint32_t * domains = nullptr;
  EXPECT_EQ(get_hermos_topics(&count, &domains), nullptr);
  EXPECT_EQ(count, -ENOENT);
  EXPECT_EQ(domains, nullptr);
  setenv("HERMOS_DEVICE_PATH", "/dev/null", 1);
  EXPECT_EQ(get_hermos_topics(&count, &domains), nullptr);
  EXPECT_EQ(count, -ENOTTY);
}

TEST_F(EndpointStatsTest, InvalidBufferOrTopicIsRejectedBeforeOpeningDevice)
{
  setenv("HERMOS_DEVICE_PATH", "/dev/hermos-e3-does-not-exist", 1);
  endpoint_stats_ret row{};
  uint32_t capacity = 1;
  EXPECT_EQ(hermos_query_endpoint_stats_v1(nullptr, 0, false, &row, &capacity), -EINVAL);
  EXPECT_EQ(hermos_query_endpoint_stats_v1("", 0, false, &row, &capacity), -EINVAL);
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 0, false, nullptr, &capacity), -EINVAL);
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 0, false, &row, nullptr), -EINVAL);
  capacity = 0;
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 0, false, &row, &capacity), -EINVAL);
  capacity = MAX_SUBSCRIBER_NUM + 1;
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 0, false, &row, &capacity), -EINVAL);
  capacity = 1;
  EXPECT_EQ(hermos_query_endpoint_stats_v1(
    std::string(256, 'x').c_str(), 0, false, &row, &capacity), -EINVAL);
}

TEST_F(EndpointStatsTest, CapacityNegotiationPreservesRowsAndReportsRequiredCount)
{
  setenv("HERMOS_DEVICE_PATH", "/dev/null", 1);
  intercept_stats = true;
  stats_error = ENOBUFS;
  stats_count = 73;
  endpoint_stats_ret row{};
  row.received_total = 123;
  uint32_t capacity = 1;
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 17, false, &row, &capacity), -ENOBUFS);
  EXPECT_EQ(capacity, 73U);
  EXPECT_EQ(row.received_total, 123U);
}

TEST_F(EndpointStatsTest, LegacyUnknownCommandIsUnsupportedAfterLocalValidation)
{
  setenv("HERMOS_DEVICE_PATH", "/dev/null", 1);
  intercept_stats = true;
  stats_error = EINVAL;
  stats_count = 0;
  endpoint_stats_ret row{};
  uint32_t capacity = 1;
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 0, false, &row, &capacity), -ENOTTY);
  EXPECT_EQ(hermos_query_endpoint_stats_v1("", 0, false, &row, &capacity), -EINVAL);
}

TEST_F(EndpointStatsTest, KernelCountBeyondCallerBufferIsRejected)
{
  setenv("HERMOS_DEVICE_PATH", "/dev/null", 1);
  intercept_stats = true;
  stats_error = 0;
  stats_count = 2;
  endpoint_stats_ret row{};
  uint32_t capacity = 1;
  EXPECT_EQ(hermos_query_endpoint_stats_v1("/topic", 0, false, &row, &capacity), -EOVERFLOW);
}

TEST(EndpointStatsLayout, MatchesTheKernelAndCtypesWireContract)
{
  EXPECT_EQ(sizeof(endpoint_stats_ret), 336U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, node_name), 0U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, pid), 256U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, endpoint_id), 260U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, domain_id), 264U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, qos_depth), 268U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, is_publisher), 272U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, is_bridge), 273U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, is_take_sub), 274U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, qos_is_transient_local), 275U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, qos_is_reliable), 276U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, reserved), 277U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, published_total), 280U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, received_total), 288U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, dropped_history_total), 296U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, queued_entries), 304U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, retained_entries), 312U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, sample_time_ns), 320U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, supported_fields), 328U);
  EXPECT_EQ(offsetof(endpoint_stats_ret, reserved2), 332U);
  EXPECT_EQ(sizeof(ioctl_endpoint_stats_args), 48U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, topic_name), 0U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, buffer_addr), 16U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, capacity), 24U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, domain_id), 28U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, is_publisher), 32U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, reserved), 33U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, ret_count), 36U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, abi_version), 40U);
  EXPECT_EQ(offsetof(ioctl_endpoint_stats_args, row_size), 44U);
  EXPECT_NE(HERMOS_GET_ENDPOINT_STATS_CMD, _IOC(_IOC_READ | _IOC_WRITE, 0xA6, 34, 40));
  EXPECT_EQ(_IOC_SIZE(HERMOS_GET_ENDPOINT_STATS_CMD), 48U);
}
}  // namespace
