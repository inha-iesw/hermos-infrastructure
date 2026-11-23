#include "affinity/deadline.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <vector>

namespace detail = hermos_cie_thread_configurator::detail;

class DeadlineAffinity : public ::testing::Test
{
protected:
  void SetUp() override
  {
    requested.size = sizeof(requested);
    requested.sched_policy = SCHED_DEADLINE;
    requested.sched_runtime = 100;
    requested.sched_deadline = 1000;
    requested.sched_period = 1000;
    previous.size = sizeof(previous);
    previous.sched_policy = SCHED_FIFO;
    previous.sched_priority = 20;
    previous.sched_flags = SCHED_FLAG_RESET_ON_FORK;
  }

  detail::DeadlineAffinityOperations operations()
  {
    return {
      [this]() { return step("prepare"); },
      [this](sched_attr & attr) {
        EXPECT_EQ(attr.size, sizeof(attr));
        attr = previous;
        return step("read");
      },
      [this](const sched_attr & attr) {
        if (attr.sched_policy == SCHED_DEADLINE) {
          EXPECT_EQ(attr.sched_runtime, requested.sched_runtime);
          EXPECT_EQ(attr.sched_deadline, requested.sched_deadline);
          EXPECT_EQ(attr.sched_period, requested.sched_period);
          return step("set");
        }
        EXPECT_EQ(attr.sched_policy, previous.sched_policy);
        EXPECT_EQ(attr.sched_priority, previous.sched_priority);
        EXPECT_EQ(attr.sched_flags, previous.sched_flags);
        return step("restore");
      },
      [this]() { return step("attach"); },
      [this]() { calls.push_back("cleanup"); errno = EBUSY; }};
  }

  bool step(const std::string & name)
  {
    calls.push_back(name);
    if (name == failed_step || (name == "restore" && fail_restore)) {
      errno = name == "restore" ? EPERM : EIO;
      return false;
    }
    return true;
  }

  sched_attr requested{};
  sched_attr previous{};
  std::vector<std::string> calls;
  std::string failed_step;
  bool fail_restore = false;
};

TEST_F(DeadlineAffinity, AppliesPolicyBeforeNarrowingAffinity)
{
  const auto result = detail::apply_deadline_affinity(requested, operations());
  EXPECT_EQ(result.failed_operation, nullptr);
  EXPECT_EQ(calls, (std::vector<std::string>{"prepare", "read", "set", "attach"}));
}

TEST_F(DeadlineAffinity, UnavailableControllerLeavesPolicyUnchanged)
{
  failed_step = "prepare";
  const auto result = detail::apply_deadline_affinity(requested, operations());
  EXPECT_STREQ(result.failed_operation, "prepare cpuset");
  EXPECT_EQ(result.error, EIO);
  EXPECT_EQ(calls, (std::vector<std::string>{"prepare"}));
}

TEST_F(DeadlineAffinity, FailedPolicySnapshotCleansOnlyItsPreparedGroup)
{
  failed_step = "read";
  const auto result = detail::apply_deadline_affinity(requested, operations());
  EXPECT_STREQ(result.failed_operation, "read scheduling policy");
  EXPECT_EQ(result.error, EIO);
  EXPECT_EQ(calls, (std::vector<std::string>{"prepare", "read", "cleanup"}));
}

TEST_F(DeadlineAffinity, FailedAdmissionNeverMovesTheThread)
{
  failed_step = "set";
  const auto result = detail::apply_deadline_affinity(requested, operations());
  EXPECT_STREQ(result.failed_operation, "apply DEADLINE policy");
  EXPECT_EQ(result.error, EIO);
  EXPECT_EQ(calls, (std::vector<std::string>{"prepare", "read", "set", "cleanup"}));
}

TEST_F(DeadlineAffinity, FailedAttachRestoresOriginalPolicyBeforeCleanup)
{
  failed_step = "attach";
  const auto result = detail::apply_deadline_affinity(requested, operations());
  EXPECT_STREQ(result.failed_operation, "attach cpuset");
  EXPECT_EQ(result.error, EIO);
  EXPECT_EQ(result.restore_error, 0);
  EXPECT_EQ(calls, (std::vector<std::string>{
    "prepare", "read", "set", "attach", "restore", "cleanup"}));
}

TEST_F(DeadlineAffinity, FailedRestoreIsReportedAlongsideAttachFailure)
{
  failed_step = "attach";
  fail_restore = true;
  const auto result = detail::apply_deadline_affinity(requested, operations());
  EXPECT_STREQ(result.failed_operation, "attach cpuset");
  EXPECT_EQ(result.error, EIO);
  EXPECT_EQ(result.restore_error, EPERM);
  EXPECT_EQ(calls, (std::vector<std::string>{
    "prepare", "read", "set", "attach", "restore", "cleanup"}));
}
