// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
#include "hermos_kunit_add_bridge.h"

#include "../hermos.h"

#include <kunit/test.h>

static const char * TOPIC_NAME = "/kunit_test_bridge_topic";
static const pid_t BRIDGE_OWNER_PID = 1000;
static const pid_t OTHER_PID = 1001;

static void setup_process(struct kunit * test, pid_t pid)
{
  union ioctl_add_process_args process = {};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_process(
    pid, current->nsproxy->ipc_ns, false, 0, &process), 0);
}

void test_case_add_bridge_normal(struct kunit * test)
{
  // Arrange
  setup_process(test, BRIDGE_OWNER_PID);
  struct ioctl_add_bridge_args args = {0};

  int ret =
    hermos_ioctl_add_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args);

  // Assert
  KUNIT_EXPECT_EQ(test, ret, 0);
  KUNIT_EXPECT_TRUE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));
  KUNIT_EXPECT_EQ(test, args.ret_pid, BRIDGE_OWNER_PID);
  KUNIT_EXPECT_TRUE(test, args.ret_has_r2a);
  KUNIT_EXPECT_FALSE(test, args.ret_has_a2r);
}

void test_case_add_bridge_update_flags(struct kunit * test)
{
  setup_process(test, BRIDGE_OWNER_PID);
  struct ioctl_add_bridge_args args = {0};
  int ret =
    hermos_ioctl_add_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args);
  KUNIT_ASSERT_EQ(test, ret, 0);
  KUNIT_EXPECT_TRUE(test, args.ret_has_r2a);
  KUNIT_EXPECT_FALSE(test, args.ret_has_a2r);

  int ret1 =
    hermos_ioctl_add_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, false, current->nsproxy->ipc_ns, &args);

  // Assert
  KUNIT_EXPECT_EQ(test, ret1, 0);
  KUNIT_EXPECT_EQ(test, args.ret_pid, BRIDGE_OWNER_PID);
  KUNIT_EXPECT_TRUE(test, args.ret_has_r2a);
  KUNIT_EXPECT_TRUE(test, args.ret_has_a2r);
}

void test_case_add_bridge_already_exists_diff_pid(struct kunit * test)
{
  setup_process(test, BRIDGE_OWNER_PID);
  setup_process(test, OTHER_PID);
  struct ioctl_add_bridge_args args = {0};
  int ret =
    hermos_ioctl_add_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args);
  KUNIT_ASSERT_EQ(test, ret, 0);

  memset(&args, 0, sizeof(args));
  int ret1 =
    hermos_ioctl_add_bridge(TOPIC_NAME, OTHER_PID, false, current->nsproxy->ipc_ns, &args);
  // Assert
  KUNIT_EXPECT_EQ(test, ret1, -EEXIST);
  KUNIT_EXPECT_EQ(test, args.ret_pid, BRIDGE_OWNER_PID);
  KUNIT_EXPECT_TRUE(test, args.ret_has_r2a);
  KUNIT_EXPECT_FALSE(test, args.ret_has_a2r);
}

void test_case_add_bridge_requires_registration(struct kunit * test)
{
  struct ioctl_add_bridge_args args = {};
  KUNIT_EXPECT_EQ(test, hermos_ioctl_add_bridge(
    TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args), -EINVAL);
  KUNIT_EXPECT_FALSE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));

  setup_process(test, BRIDGE_OWNER_PID);
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_bridge(
    TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args), 0);
  // Registration is required even when lookup would find another process's bridge.
  KUNIT_EXPECT_EQ(test, hermos_ioctl_add_bridge(
    TOPIC_NAME, OTHER_PID, false, current->nsproxy->ipc_ns, &args), -EINVAL);
  KUNIT_EXPECT_EQ(test, hermos_get_bridge_owner_pid(
    TOPIC_NAME, current->nsproxy->ipc_ns), BRIDGE_OWNER_PID);
}

void test_case_add_bridge_rejects_namespace_mismatch(struct kunit * test)
{
  struct ipc_namespace * other_ns = kunit_kzalloc(test, sizeof(*other_ns), GFP_KERNEL);
  KUNIT_ASSERT_NOT_NULL(test, other_ns);
  setup_process(test, BRIDGE_OWNER_PID);
  struct ioctl_add_bridge_args args = {};
  KUNIT_EXPECT_EQ(test, hermos_ioctl_add_bridge(
    TOPIC_NAME, BRIDGE_OWNER_PID, true, other_ns, &args), -EINVAL);
  KUNIT_EXPECT_FALSE(test, hermos_is_in_bridge_htable(TOPIC_NAME, other_ns));
  KUNIT_EXPECT_FALSE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));
}

void test_case_add_bridge_exit_cleanup(struct kunit * test)
{
  setup_process(test, BRIDGE_OWNER_PID);
  struct ioctl_add_bridge_args args = {};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_bridge(
    TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args), 0);
  KUNIT_ASSERT_TRUE(test, is_hermos_pid(BRIDGE_OWNER_PID));
  hermos_process_exit_cleanup(BRIDGE_OWNER_PID);
  KUNIT_ASSERT_TRUE(test, hermos_is_proc_exited(BRIDGE_OWNER_PID));
  KUNIT_EXPECT_FALSE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));
  KUNIT_EXPECT_EQ(test, hermos_ioctl_add_bridge(
    TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args), -EINVAL);
  KUNIT_EXPECT_FALSE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));
}
