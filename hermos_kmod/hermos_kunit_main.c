// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
#include "hermos.h"
#include "hermos_kunit/hermos_kunit_add_bridge.h"
#include "hermos_kunit/hermos_kunit_add_process.h"
#include "hermos_kunit/hermos_kunit_add_publisher.h"
#include "hermos_kunit/hermos_kunit_add_subscriber.h"
#include "hermos_kunit/hermos_kunit_bridge_shutdown.h"
#include "hermos_kunit/hermos_kunit_check_and_request_bridge_shutdown.h"
#include "hermos_kunit/hermos_kunit_do_exit.h"
#include "hermos_kunit/hermos_kunit_domain_bridge.h"
#include "hermos_kunit/hermos_kunit_get_node_publisher_topics.h"
#include "hermos_kunit/hermos_kunit_get_node_subscriber_topics.h"
#include "hermos_kunit/hermos_kunit_get_publisher_num.h"
#include "hermos_kunit/hermos_kunit_get_publisher_qos.h"
#include "hermos_kunit/hermos_kunit_get_subscriber_num.h"
#include "hermos_kunit/hermos_kunit_get_subscriber_qos.h"
#include "hermos_kunit/hermos_kunit_get_topic_publisher_info.h"
#include "hermos_kunit/hermos_kunit_get_topic_subscriber_info.h"
#include "hermos_kunit/hermos_kunit_get_version.h"
#include "hermos_kunit/hermos_kunit_init_memory_allocator.h"
#include "hermos_kunit/hermos_kunit_publish_msg.h"
#include "hermos_kunit/hermos_kunit_receive_msg.h"
#include "hermos_kunit/hermos_kunit_release_sub_ref.h"
#include "hermos_kunit/hermos_kunit_remove_bridge.h"
#include "hermos_kunit/hermos_kunit_remove_publisher.h"
#include "hermos_kunit/hermos_kunit_remove_subscriber.h"
#include "hermos_kunit/hermos_kunit_set_ros2_publisher_num.h"
#include "hermos_kunit/hermos_kunit_set_ros2_subscriber_num.h"
#include "hermos_kunit/hermos_kunit_take_msg.h"
#include "hermos_memory_allocator.h"

#include <kunit/test.h>

MODULE_LICENSE("Dual BSD/GPL");

struct kunit_case hermos_test_cases[] = {
  TEST_CASES_ADD_SUBSCRIBER,
  TEST_CASES_ADD_PUBLISHER,
  TEST_CASES_REMOVE_SUBSCRIBER,
  TEST_CASES_REMOVE_PUBLISHER,
  TEST_CASES_RELEASE_SUB_REF,
  TEST_CASES_RECEIVE_MSG,
  TEST_CASES_PUBLISH_MSG,
  TEST_CASES_TAKE_MSG,
  TEST_CASES_ADD_PROCESS,
  TEST_CASES_BRIDGE_SHUTDOWN,
  TEST_CASES_CHECK_AND_REQUEST_BRIDGE_SHUTDOWN,
  TEST_CASES_GET_SUBSCRIBER_NUM,
  TEST_CASES_GET_SUBSCRIBER_QOS,
  TEST_CASES_GET_PUBLISHER_NUM,
  TEST_CASES_GET_PUBLISHER_QOS,
  TEST_CASES_ADD_BRIDGE,
  TEST_CASES_REMOVE_BRIDGE,
  TEST_CASES_DOMAIN_BRIDGE,
  TEST_CASES_SET_ROS2_SUBSCRIBER_NUM,
  TEST_CASES_SET_ROS2_PUBLISHER_NUM,
  TEST_CASES_DO_EXIT,
  TEST_CASES_GET_NODE_SUBSCRIBER_TOPICS,
  TEST_CASES_GET_NODE_PUBLISHER_TOPICS,
  TEST_CASES_GET_TOPIC_SUBSCRIBER_INFO,
  TEST_CASES_GET_TOPIC_PUBLISHER_INFO,
  TEST_CASES_GET_VERSION,
  TEST_CASES_INIT_MEMORY_ALLOCATOR,
  {},
};

static int hermos_test_init(struct kunit * test)
{
  return 0;
}

static void hermos_test_exit(struct kunit * test)
{
  hermos_exit_free_data();
  exit_memory_allocator();
}

static int hermos_test_suite_init(struct kunit_suite * test_suite)
{
  int ret;

  ret = hermos_init_device();
  if (ret < 0) return ret;

  ret = hermos_init_kthread();
  if (ret < 0) {
    hermos_exit_device();
    return ret;
  }

  ret = hermos_init_exit_hook();
  if (ret < 0) {
    hermos_exit_kthread();
    hermos_exit_device();
    return ret;
  }

  ret = init_memory_allocator();
  if (ret < 0) {
    hermos_exit_exit_hook();
    hermos_exit_kthread();
    hermos_exit_device();
    return ret;
  }

  return 0;
}

static void hermos_test_suite_exit(struct kunit_suite * test_suite)
{
  hermos_exit_kthread();
  hermos_exit_exit_hook();
  cleanup_memory_allocator();
  hermos_exit_device();
}

struct kunit_suite hermos_test_suite = {
  .name = "hermos_test_suite",
  .init = hermos_test_init,
  .exit = hermos_test_exit,
  .suite_init = hermos_test_suite_init,
  .suite_exit = hermos_test_suite_exit,
  .test_cases = hermos_test_cases,
};

kunit_test_suite(hermos_test_suite);
