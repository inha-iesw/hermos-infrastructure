// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
#include "hermos_kunit_remove_bridge.h"

#include "../hermos.h"

#include <kunit/test.h>

static const char * TOPIC_NAME = "/kunit_test_bridge_remove_topic";
static const pid_t BRIDGE_OWNER_PID = 1000;
static const pid_t OTHER_PID = 1001;

void test_case_remove_bridge_normal(struct kunit * test)
{
  // Arrange
  struct ioctl_add_bridge_args args = {0};
  int ret_setup =
    hermos_ioctl_add_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args);
  KUNIT_ASSERT_EQ(test, ret_setup, 0);

  // Act
  int ret =
    hermos_ioctl_remove_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns);

  // Assert
  KUNIT_EXPECT_EQ(test, ret, 0);
  KUNIT_EXPECT_FALSE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));
}

void test_case_remove_bridge_partial(struct kunit * test)
{
  // Arrange
  struct ioctl_add_bridge_args args = {0};
  hermos_ioctl_add_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args);
  hermos_ioctl_add_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, false, current->nsproxy->ipc_ns, &args);

  // Act
  int ret1 =
    hermos_ioctl_remove_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns);

  // Assert
  KUNIT_EXPECT_EQ(test, ret1, 0);
  KUNIT_EXPECT_TRUE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));

  // Act
  int ret2 =
    hermos_ioctl_remove_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, false, current->nsproxy->ipc_ns);

  // Assert
  KUNIT_EXPECT_EQ(test, ret2, 0);
  KUNIT_EXPECT_FALSE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));
}

void test_case_remove_bridge_not_found(struct kunit * test)
{
  // Arrange
  const char * NON_EXISTENT_TOPIC = "/kunit_non_existent_topic";

  // Act
  int ret = hermos_ioctl_remove_bridge(
    NON_EXISTENT_TOPIC, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns);

  // Assert
  KUNIT_EXPECT_EQ(test, ret, -ENOENT);
}

void test_case_remove_bridge_pid_mismatch(struct kunit * test)
{
  // Arrange
  struct ioctl_add_bridge_args args = {0};
  int ret_setup =
    hermos_ioctl_add_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns, &args);
  KUNIT_ASSERT_EQ(test, ret_setup, 0);

  // Act
  int ret = hermos_ioctl_remove_bridge(TOPIC_NAME, OTHER_PID, true, current->nsproxy->ipc_ns);

  // Assert
  KUNIT_EXPECT_EQ(test, ret, -EPERM);
  KUNIT_EXPECT_TRUE(test, hermos_is_in_bridge_htable(TOPIC_NAME, current->nsproxy->ipc_ns));
  KUNIT_EXPECT_EQ(
    test, hermos_get_bridge_owner_pid(TOPIC_NAME, current->nsproxy->ipc_ns), BRIDGE_OWNER_PID);

  // Clean-up
  hermos_ioctl_remove_bridge(TOPIC_NAME, BRIDGE_OWNER_PID, true, current->nsproxy->ipc_ns);
}