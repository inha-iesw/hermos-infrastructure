/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#include "test_endpoint_stats.h"
#include "../../observation/endpoint_stats.h"

static const char * topic = "/observation";
static topic_local_id_t notification_ids[MAX_SUBSCRIBER_NUM];

static void process(struct kunit * test, pid_t pid, uint32_t domain, uint64_t * address)
{
  union ioctl_add_process_args args = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_process(
    pid, current->nsproxy->ipc_ns, false, domain, &args), 0);
  if (address) *address = args.ret_addr;
}

static void publisher(struct kunit * test, pid_t pid, topic_local_id_t * id)
{
  union ioctl_add_publisher_args args = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_publisher(
    topic, current->nsproxy->ipc_ns, "/publisher", pid, 1, false, false, &args), 0);
  *id = args.ret_id;
}

static void subscriber(struct kunit * test, pid_t pid, bool bridge, topic_local_id_t * id)
{
  union ioctl_add_subscriber_args args = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_subscriber(
    topic, current->nsproxy->ipc_ns, "/consumer", pid, 1, false, true,
    false, false, bridge, &args), 0);
  *id = args.ret_id;
}

static void snapshot(
  struct kunit * test, uint32_t domain, bool is_publisher, struct endpoint_stats_ret * row)
{
  uint32_t count = 0;
  KUNIT_ASSERT_EQ(test, hermos_get_endpoint_stats(
    topic, current->nsproxy->ipc_ns, domain, is_publisher, row, 1, &count), 0);
  KUNIT_ASSERT_EQ(test, count, 1U);
}

void test_endpoint_stats_history_and_references(struct kunit * test)
{
  topic_local_id_t pub, sub;
  uint64_t address;
  process(test, 6000, 0, &address);
  process(test, 6001, 0, NULL);
  publisher(test, 6000, &pub);
  subscriber(test, 6001, false, &sub);
  union ioctl_publish_msg_args published = {0};
  for (int index = 0; index < 3; index++) {
    KUNIT_ASSERT_EQ(test, hermos_ioctl_publish_msg(
      topic, current->nsproxy->ipc_ns, pub, address + index * 16,
      notification_ids, MAX_SUBSCRIBER_NUM, &published), 0);
  }
  struct endpoint_stats_ret row;
  snapshot(test, 0, true, &row);
  KUNIT_EXPECT_EQ(test, row.published_total, (uint64_t)3);
  KUNIT_EXPECT_EQ(test, row.retained_entries, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, row.supported_fields, HERMOS_STATS_PUBLISHED | HERMOS_STATS_RETAINED);
  snapshot(test, 0, false, &row);
  KUNIT_EXPECT_EQ(test, row.received_total, (uint64_t)0);
  KUNIT_EXPECT_EQ(test, row.dropped_history_total, (uint64_t)2);
  KUNIT_EXPECT_EQ(test, row.queued_entries, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, row.retained_entries, (uint64_t)0);
  KUNIT_EXPECT_EQ(test, hermos_get_entry_rc(
    topic, current->nsproxy->ipc_ns, published.ret_entry_id, sub), 0);

  union ioctl_receive_msg_args received = {0};
  struct publisher_shm_info mappings[2];
  KUNIT_ASSERT_EQ(test, hermos_ioctl_receive_msg(
    topic, current->nsproxy->ipc_ns, sub, mappings, 2, &received), 0);
  KUNIT_ASSERT_EQ(test, received.ret_entry_num, (uint16_t)1);
  snapshot(test, 0, false, &row);
  KUNIT_EXPECT_EQ(test, row.received_total, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, row.queued_entries, (uint64_t)0);
  KUNIT_EXPECT_EQ(test, row.retained_entries, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, hermos_get_entry_rc(
    topic, current->nsproxy->ipc_ns, received.ret_entry_ids[0], sub), 1);
  KUNIT_ASSERT_EQ(test, hermos_ioctl_release_message_entry_reference(
    topic, current->nsproxy->ipc_ns, sub, received.ret_entry_ids[0]), 0);
  snapshot(test, 0, false, &row);
  KUNIT_EXPECT_EQ(test, row.retained_entries, (uint64_t)0);
  KUNIT_EXPECT_EQ(test, row.received_total, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, row.dropped_history_total, (uint64_t)2);
}

void test_endpoint_stats_domain_and_bridge_identity(struct kunit * test)
{
  topic_local_id_t id;
  process(test, 6010, 17, NULL);
  subscriber(test, 6010, true, &id);
  struct endpoint_stats_ret row;
  snapshot(test, 17, false, &row);
  KUNIT_EXPECT_EQ(test, row.pid, (int32_t)6010);
  KUNIT_EXPECT_EQ(test, row.endpoint_id, id);
  KUNIT_EXPECT_EQ(test, row.domain_id, 17U);
  KUNIT_EXPECT_EQ(test, row.is_bridge, (uint8_t)1);
  KUNIT_EXPECT_STREQ(test, row.node_name, "/consumer");
  KUNIT_EXPECT_GT(test, row.sample_time_ns, (uint64_t)0);
  uint32_t count = 42;
  KUNIT_ASSERT_EQ(test, hermos_get_endpoint_stats(
    topic, current->nsproxy->ipc_ns, 0, false, &row, 1, &count), 0);
  KUNIT_EXPECT_EQ(test, count, 0U);
}

void test_endpoint_stats_excludes_exited_endpoints(struct kunit * test)
{
  topic_local_id_t pub, waiting, holding;
  uint64_t address;
  process(test, 6020, 0, &address);
  process(test, 6021, 0, NULL);
  publisher(test, 6020, &pub);
  subscriber(test, 6021, false, &waiting);
  subscriber(test, 6021, false, &holding);
  union ioctl_publish_msg_args published = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_publish_msg(
    topic, current->nsproxy->ipc_ns, pub, address,
    notification_ids, MAX_SUBSCRIBER_NUM, &published), 0);
  union ioctl_receive_msg_args received = {0};
  struct publisher_shm_info mappings[2];
  KUNIT_ASSERT_EQ(test, hermos_ioctl_receive_msg(
    topic, current->nsproxy->ipc_ns, holding, mappings, 2, &received), 0);
  KUNIT_ASSERT_EQ(test, received.ret_entry_num, (uint16_t)1);

  hermos_process_exit_cleanup(6020);
  /* Held data keeps the publisher metadata alive: an empty topic cannot pass this test. */
  KUNIT_ASSERT_TRUE(test, hermos_is_in_publisher_htable(topic, current->nsproxy->ipc_ns, pub));
  KUNIT_ASSERT_EQ(test, hermos_get_entry_rc(
    topic, current->nsproxy->ipc_ns, received.ret_entry_ids[0], holding), 1);
  struct endpoint_stats_ret rows[2];
  uint32_t count = 42;
  KUNIT_ASSERT_EQ(test, hermos_get_endpoint_stats(
    topic, current->nsproxy->ipc_ns, 0, true, rows, 2, &count), 0);
  KUNIT_EXPECT_EQ(test, count, 0U);
  KUNIT_ASSERT_EQ(test, hermos_get_endpoint_stats(
    topic, current->nsproxy->ipc_ns, 0, false, rows, 2, &count), 0);
  KUNIT_ASSERT_EQ(test, count, 2U);
  KUNIT_EXPECT_NE(test, rows[0].endpoint_id, rows[1].endpoint_id);
  for (uint32_t index = 0; index < count; index++) {
    KUNIT_EXPECT_TRUE(test, rows[index].endpoint_id == waiting || rows[index].endpoint_id == holding);
    KUNIT_EXPECT_EQ(test, rows[index].queued_entries, (uint64_t)0);
    KUNIT_EXPECT_EQ(test, rows[index].retained_entries,
      (uint64_t)(rows[index].endpoint_id == holding));
    KUNIT_EXPECT_EQ(test, rows[index].received_total,
      (uint64_t)(rows[index].endpoint_id == holding));
  }
}

void test_endpoint_stats_capacity(struct kunit * test)
{
  topic_local_id_t id;
  process(test, 6030, 0, NULL);
  subscriber(test, 6030, false, &id);
  subscriber(test, 6030, true, &id);
  struct endpoint_stats_ret row;
  uint32_t count = 0;
  KUNIT_EXPECT_EQ(test, hermos_get_endpoint_stats(
    topic, current->nsproxy->ipc_ns, 0, false, &row, 1, &count), -ENOBUFS);
  KUNIT_EXPECT_EQ(test, count, 2U);
  struct endpoint_stats_ret rows[2];
  KUNIT_ASSERT_EQ(test, hermos_get_endpoint_stats(
    topic, current->nsproxy->ipc_ns, 0, false, rows, 2, &count), 0);
  KUNIT_EXPECT_EQ(test, count, 2U);
}

void test_endpoint_stats_removed_publisher_keeps_held_reference(struct kunit * test)
{
  topic_local_id_t pub, sub;
  uint64_t address;
  process(test, 6040, 0, &address);
  process(test, 6041, 0, NULL);
  publisher(test, 6040, &pub);
  subscriber(test, 6041, false, &sub);
  union ioctl_publish_msg_args published = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_publish_msg(
    topic, current->nsproxy->ipc_ns, pub, address,
    notification_ids, MAX_SUBSCRIBER_NUM, &published), 0);
  union ioctl_receive_msg_args received = {0};
  struct publisher_shm_info mappings[2];
  KUNIT_ASSERT_EQ(test, hermos_ioctl_receive_msg(
    topic, current->nsproxy->ipc_ns, sub, mappings, 2, &received), 0);
  KUNIT_ASSERT_EQ(test, received.ret_entry_num, (uint16_t)1);
  KUNIT_ASSERT_EQ(test, hermos_ioctl_remove_publisher(
    topic, current->nsproxy->ipc_ns, pub), 0);
  struct endpoint_stats_ret row;
  uint32_t count = 42;
  KUNIT_ASSERT_EQ(test, hermos_get_endpoint_stats(
    topic, current->nsproxy->ipc_ns, 0, true, &row, 1, &count), 0);
  KUNIT_EXPECT_EQ(test, count, 0U);
  snapshot(test, 0, false, &row);
  KUNIT_EXPECT_EQ(test, row.retained_entries, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, hermos_get_entry_rc(
    topic, current->nsproxy->ipc_ns, received.ret_entry_ids[0], sub), 1);
  KUNIT_EXPECT_EQ(test, hermos_ioctl_release_message_entry_reference(
    topic, current->nsproxy->ipc_ns, sub, received.ret_entry_ids[0]), 0);
}

void test_endpoint_stats_take_same_message_counts_once(struct kunit * test)
{
  topic_local_id_t pub;
  uint64_t address;
  process(test, 6050, 0, &address);
  process(test, 6051, 0, NULL);
  publisher(test, 6050, &pub);
  union ioctl_add_subscriber_args added = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_subscriber(
    topic, current->nsproxy->ipc_ns, "/consumer", 6051, 1, false, true,
    true, false, false, &added), 0);
  union ioctl_publish_msg_args published = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_publish_msg(
    topic, current->nsproxy->ipc_ns, pub, address,
    notification_ids, MAX_SUBSCRIBER_NUM, &published), 0);
  struct publisher_shm_info mappings[2];
  union ioctl_take_msg_args taken = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_take_msg(
    topic, current->nsproxy->ipc_ns, added.ret_id, false, mappings, 2, &taken), 0);
  KUNIT_ASSERT_EQ(test, taken.ret_entry_id, published.ret_entry_id);
  struct endpoint_stats_ret row;
  snapshot(test, 0, false, &row);
  KUNIT_EXPECT_EQ(test, row.received_total, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, row.retained_entries, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, row.queued_entries, (uint64_t)0);

  KUNIT_ASSERT_EQ(test, hermos_ioctl_take_msg(
    topic, current->nsproxy->ipc_ns, added.ret_id, true, mappings, 2, &taken), 0);
  KUNIT_EXPECT_EQ(test, taken.ret_entry_id, published.ret_entry_id);
  snapshot(test, 0, false, &row);
  KUNIT_EXPECT_EQ(test, row.received_total, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, row.retained_entries, (uint64_t)1);
  KUNIT_EXPECT_EQ(test, hermos_get_entry_rc(
    topic, current->nsproxy->ipc_ns, taken.ret_entry_id, added.ret_id), 1);
}

void test_endpoint_stats_ignore_local_publications(struct kunit * test)
{
  topic_local_id_t pub;
  uint64_t address;
  process(test, 6060, 0, &address);
  publisher(test, 6060, &pub);
  union ioctl_add_subscriber_args added = {0};
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_subscriber(
    topic, current->nsproxy->ipc_ns, "/consumer", 6060, 1, false, true,
    false, true, false, &added), 0);
  union ioctl_publish_msg_args published = {0};
  for (int index = 0; index < 3; index++) {
    KUNIT_ASSERT_EQ(test, hermos_ioctl_publish_msg(
      topic, current->nsproxy->ipc_ns, pub, address + index * 16,
      notification_ids, MAX_SUBSCRIBER_NUM, &published), 0);
    KUNIT_EXPECT_EQ(test, published.ret_subscriber_num, 0U);
  }
  struct endpoint_stats_ret row;
  snapshot(test, 0, false, &row);
  KUNIT_EXPECT_EQ(test, row.dropped_history_total, (uint64_t)0);
  KUNIT_EXPECT_EQ(test, row.queued_entries, (uint64_t)0);
}

static void check_domain_direction(struct kunit * test, bool allowed)
{
  KUNIT_ASSERT_EQ(test, hermos_ioctl_add_domain_bridge(
    topic, topic, 1, 2, current->nsproxy->ipc_ns), 0);
  topic_local_id_t pub, sub;
  uint64_t address;
  const uint32_t publisher_domain = allowed ? 1 : 2;
  const uint32_t subscriber_domain = allowed ? 2 : 1;
  process(test, current->tgid, publisher_domain, &address);
  process(test, 6070, subscriber_domain, NULL);
  publisher(test, current->tgid, &pub);
  subscriber(test, 6070, false, &sub);
  union ioctl_publish_msg_args published = {0};
  for (int index = 0; index < 3; index++) {
    KUNIT_ASSERT_EQ(test, hermos_ioctl_publish_msg(
      topic, current->nsproxy->ipc_ns, pub, address + index * 16,
      notification_ids, MAX_SUBSCRIBER_NUM, &published), 0);
    KUNIT_EXPECT_EQ(test, published.ret_subscriber_num, (uint32_t)allowed);
  }
  struct endpoint_stats_ret row;
  snapshot(test, subscriber_domain, false, &row);
  KUNIT_EXPECT_EQ(test, row.dropped_history_total, allowed ? (uint64_t)2 : (uint64_t)0);
  KUNIT_EXPECT_EQ(test, row.queued_entries, (uint64_t)allowed);
}

void test_endpoint_stats_domain_direction_allowed(struct kunit * test)
{
  check_domain_direction(test, true);
}

void test_endpoint_stats_domain_direction_blocked(struct kunit * test)
{
  check_domain_direction(test, false);
}

void test_endpoint_stats_request_validation(struct kunit * test)
{
  const struct ioctl_endpoint_stats_args valid = {
    .capacity = 1,
    .abi_version = HERMOS_ENDPOINT_STATS_ABI_VERSION,
    .row_size = sizeof(struct endpoint_stats_ret),
  };
  struct ioctl_endpoint_stats_args args = valid;
  KUNIT_ASSERT_EQ(test, hermos_validate_endpoint_stats_request(&args), 0);
  args.abi_version = 0;
  KUNIT_EXPECT_EQ(test, hermos_validate_endpoint_stats_request(&args), -EPROTO);
  args.abi_version = HERMOS_ENDPOINT_STATS_ABI_VERSION + 1;
  KUNIT_EXPECT_EQ(test, hermos_validate_endpoint_stats_request(&args), -EPROTO);
  args = valid;
  args.row_size--;
  KUNIT_EXPECT_EQ(test, hermos_validate_endpoint_stats_request(&args), -EPROTO);
  args = valid;
  args.is_publisher = 2;
  KUNIT_EXPECT_EQ(test, hermos_validate_endpoint_stats_request(&args), -EINVAL);
  args = valid;
  args.capacity = 0;
  KUNIT_EXPECT_EQ(test, hermos_validate_endpoint_stats_request(&args), -EINVAL);
  args.capacity = MAX_SUBSCRIBER_NUM + 1;
  KUNIT_EXPECT_EQ(test, hermos_validate_endpoint_stats_request(&args), -EINVAL);
  for (int index = 0; index < 3; index++) {
    args = valid;
    args.reserved[index] = 1;
    KUNIT_EXPECT_EQ(test, hermos_validate_endpoint_stats_request(&args), -EINVAL);
  }
  KUNIT_EXPECT_EQ(test, hermos_ioctl(NULL, _IOC(_IOC_READ | _IOC_WRITE,
    _IOC_TYPE(HERMOS_GET_ENDPOINT_STATS_CMD), _IOC_NR(HERMOS_GET_ENDPOINT_STATS_CMD),
    sizeof(struct ioctl_endpoint_stats_args) - 8), 0), (long)-EPROTO);
}
