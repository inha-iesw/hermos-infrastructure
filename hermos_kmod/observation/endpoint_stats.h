/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#pragma once

#include "../hermos_internal.h"

/* The caller holds the topic write lock and global read lock. */
void hermos_observe_history_drop(
  const struct topic_wrapper * wrapper, const struct publisher_info * publisher,
  const struct entry_node * entry);

/**
 * @brief Snapshot live endpoints without acquiring payload references.
 * @param topic_name Non-null topic name; caller retains ownership.
 * @param ipc_ns Non-null namespace identity; caller retains ownership.
 * @param domain_id Exact source domain.
 * @param is_publisher Select publisher rows when true, subscriber rows otherwise.
 * @param output Non-null caller-owned buffer, valid only on success.
 * @param capacity Number of available rows.
 * @param count Non-null output for the total required row count, including on ENOBUFS.
 * @return Zero on success, -ENOBUFS if capacity is insufficient, or -ENOMEM.
 * @note Acquires global then topic read locks. The caller must hold neither lock.
 */
int hermos_get_endpoint_stats(
  const char * topic_name, const struct ipc_namespace * ipc_ns, uint32_t domain_id,
  bool is_publisher, struct endpoint_stats_ret * output, uint32_t capacity,
  uint32_t * count);

/**
 * @brief Validate the fixed-layout request before reading names or writing output.
 * @param args Non-null, borrowed kernel copy of the ioctl request.
 * @return Zero for a valid layout and bounds, -EPROTO or -EINVAL otherwise.
 */
int hermos_validate_endpoint_stats_request(const struct ioctl_endpoint_stats_args * args);

long hermos_endpoint_stats_cmd(struct ioctl_endpoint_stats_args __user * argument);
