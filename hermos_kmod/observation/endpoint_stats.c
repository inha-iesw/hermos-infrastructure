/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#include "endpoint_stats.h"

#include <linux/ktime.h>
#include <linux/build_bug.h>

/* TODO: Share this predicate with publish/receive/take after timing validation.
 * Until then, changes to domain or ignore-local delivery rules must update both
 * paths; observation KUnit tests cover those two rules. */
static bool eligible(
  const struct topic_struct * topic, const struct publisher_info * publisher,
  const struct subscriber_info * subscriber)
{
  return hermos_domain_delivery_allowed(topic, publisher->domain_id, subscriber->domain_id) &&
         !(subscriber->ignore_local_publications && publisher->pid == subscriber->pid);
}

void hermos_observe_history_drop(
  const struct topic_wrapper * wrapper, const struct publisher_info * publisher,
  const struct entry_node * entry)
{
  struct subscriber_info * subscriber;
  int bucket;
  hash_for_each(wrapper->topic->sub_info_htable, bucket, subscriber, node)
  {
    if (entry->entry_id > subscriber->latest_received_entry_id &&
        eligible(wrapper->topic, publisher, subscriber)) {
      subscriber->dropped_history_total++;
    }
  }
}

static pid_t visible_pid(const struct process_info * process)
{
#ifdef KUNIT_BUILD
  return process->local_pid;
#else
  pid_t result = 0;
  rcu_read_lock();
  struct pid * identity = find_pid_ns(process->global_pid, &init_pid_ns);
  if (identity) result = pid_vnr(identity);
  rcu_read_unlock();
  return result;
#endif
}

static const struct process_info * live_process(pid_t pid)
{
  const struct process_info * process = hermos_find_process_info(pid);
  return process && !process->exited ? process : NULL;
}

static void subscriber_entries(
  const struct topic_wrapper * wrapper, const struct subscriber_info ** subscribers,
  struct endpoint_stats_ret * rows, uint32_t count)
{
  struct rb_node * node;
  /* Read the tree and publisher liveness once per entry, not once per subscriber. */
  for (node = rb_first(&wrapper->topic->entries); node; node = rb_next(node)) {
    const struct entry_node * entry = rb_entry(node, struct entry_node, node);
    const struct publisher_info * publisher =
      hermos_find_publisher_info(wrapper, entry->publisher_id);
    const bool live_publisher = publisher && live_process(publisher->pid);
    for (uint32_t index = 0; index < count; index++) {
      const struct subscriber_info * subscriber = subscribers[index];
      if (test_bit(subscriber->id, entry->referencing_subscribers)) {
        rows[index].retained_entries++;
      }
      if (entry->entry_id > subscriber->latest_received_entry_id && live_publisher &&
          eligible(wrapper->topic, publisher, subscriber)) {
        rows[index].queued_entries++;
      }
    }
  }
}

int hermos_get_endpoint_stats(
  const char * topic_name, const struct ipc_namespace * ipc_ns, uint32_t domain_id,
  bool is_publisher, struct endpoint_stats_ret * output, uint32_t capacity, uint32_t * count)
{
  int result = 0;
  struct topic_wrapper * wrapper;
  const struct subscriber_info ** subscribers = NULL;
  *count = 0;
  if (!is_publisher && capacity) {
    subscribers = kvmalloc_array(capacity, sizeof(*subscribers), GFP_KERNEL);
    if (!subscribers) return -ENOMEM;
  }
  down_read(&global_htables_rwsem);
  wrapper = hermos_find_topic(topic_name, ipc_ns, domain_id);
  if (!wrapper) goto unlock_global;
  down_read(&wrapper->topic->rwsem);
  const uint64_t sample_time = ktime_get_ns();
  if (is_publisher) {
    struct publisher_info * publisher;
    int endpoint_bucket;
    hash_for_each(wrapper->topic->pub_info_htable, endpoint_bucket, publisher, node)
    {
      const struct process_info * process = live_process(publisher->pid);
      if (publisher->domain_id != domain_id || publisher->is_removed || !process) continue;
      if ((*count)++ >= capacity) {result = -ENOBUFS; continue;}
      struct endpoint_stats_ret * row = &output[*count - 1];
      memset(row, 0, sizeof(*row));
      strscpy(row->node_name, publisher->node_name, sizeof(row->node_name));
      row->pid = visible_pid(process);
      row->endpoint_id = publisher->id;
      row->domain_id = domain_id;
      row->qos_depth = publisher->qos_depth;
      row->is_publisher = 1;
      row->is_bridge = publisher->is_bridge;
      row->qos_is_transient_local = publisher->qos_is_transient_local;
      row->published_total = publisher->published_total;
      row->retained_entries = publisher->entries_num;
      row->sample_time_ns = sample_time;
      row->supported_fields = HERMOS_STATS_PUBLISHED | HERMOS_STATS_RETAINED;
    }
  } else {
    struct subscriber_info * subscriber;
    int endpoint_bucket;
    hash_for_each(wrapper->topic->sub_info_htable, endpoint_bucket, subscriber, node)
    {
      const struct process_info * process = live_process(subscriber->pid);
      if (subscriber->domain_id != domain_id || !process) continue;
      if ((*count)++ >= capacity) {result = -ENOBUFS; continue;}
      struct endpoint_stats_ret * row = &output[*count - 1];
      subscribers[*count - 1] = subscriber;
      memset(row, 0, sizeof(*row));
      strscpy(row->node_name, subscriber->node_name, sizeof(row->node_name));
      row->pid = visible_pid(process);
      row->endpoint_id = subscriber->id;
      row->domain_id = domain_id;
      row->qos_depth = subscriber->qos_depth;
      row->is_bridge = subscriber->is_bridge;
      row->is_take_sub = subscriber->is_take_sub;
      row->qos_is_transient_local = subscriber->qos_is_transient_local;
      row->qos_is_reliable = subscriber->qos_is_reliable;
      row->received_total = subscriber->received_total;
      row->dropped_history_total = subscriber->dropped_history_total;
      row->sample_time_ns = sample_time;
      row->supported_fields = HERMOS_STATS_RECEIVED | HERMOS_STATS_HISTORY_DROP |
                              HERMOS_STATS_QUEUED | HERMOS_STATS_RETAINED;
    }
    /* A capacity retry only needs the endpoint count, not an expensive partial snapshot. */
    if (!result && *count) subscriber_entries(wrapper, subscribers, output, *count);
  }
  up_read(&wrapper->topic->rwsem);
unlock_global:
  up_read(&global_htables_rwsem);
  kvfree(subscribers);
  return result;
}

int hermos_validate_endpoint_stats_request(const struct ioctl_endpoint_stats_args * args)
{
  if (args->abi_version != HERMOS_ENDPOINT_STATS_ABI_VERSION ||
      args->row_size != sizeof(struct endpoint_stats_ret)) return -EPROTO;
  if (args->is_publisher > 1 || args->capacity == 0 || args->capacity > MAX_SUBSCRIBER_NUM ||
      args->reserved[0] || args->reserved[1] || args->reserved[2]) return -EINVAL;
  return 0;
}

long hermos_endpoint_stats_cmd(struct ioctl_endpoint_stats_args __user * argument)
{
  BUILD_BUG_ON(sizeof(struct endpoint_stats_ret) != 336);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, node_name) != 0);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, pid) != 256);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, endpoint_id) != 260);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, domain_id) != 264);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, qos_depth) != 268);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, is_publisher) != 272);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, is_bridge) != 273);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, is_take_sub) != 274);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, qos_is_transient_local) != 275);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, qos_is_reliable) != 276);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, reserved) != 277);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, published_total) != 280);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, received_total) != 288);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, dropped_history_total) != 296);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, queued_entries) != 304);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, retained_entries) != 312);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, sample_time_ns) != 320);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, supported_fields) != 328);
  BUILD_BUG_ON(offsetof(struct endpoint_stats_ret, reserved2) != 332);
  BUILD_BUG_ON(sizeof(struct ioctl_endpoint_stats_args) != 48);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, topic_name) != 0);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, buffer_addr) != 16);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, capacity) != 24);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, domain_id) != 28);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, is_publisher) != 32);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, reserved) != 33);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, ret_count) != 36);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, abi_version) != 40);
  BUILD_BUG_ON(offsetof(struct ioctl_endpoint_stats_args, row_size) != 44);
  struct ioctl_endpoint_stats_args args;
  char topic_name[TOPIC_NAME_BUFFER_SIZE];
  if (copy_from_user(&args, argument, sizeof(args))) return -EFAULT;
  int result = hermos_validate_endpoint_stats_request(&args);
  if (result) return result;
  result = copy_name_from_user(topic_name, sizeof(topic_name), &args.topic_name);
  if (result) return result;
  struct endpoint_stats_ret * rows = kvcalloc(args.capacity, sizeof(*rows), GFP_KERNEL);
  if (!rows) return -ENOMEM;
  result = hermos_get_endpoint_stats(
    topic_name, current->nsproxy->ipc_ns, args.domain_id, args.is_publisher,
    rows, args.capacity, &args.ret_count);
  if (!result && copy_to_user(u64_to_user_ptr(args.buffer_addr), rows,
                             args.ret_count * sizeof(*rows))) result = -EFAULT;
  kvfree(rows);
  if ((result == 0 || result == -ENOBUFS) &&
      copy_to_user(argument, &args, sizeof(args))) result = -EFAULT;
  return result;
}
