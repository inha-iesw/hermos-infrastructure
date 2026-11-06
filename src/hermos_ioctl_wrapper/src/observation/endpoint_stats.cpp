// Copyright 2026
// SPDX-License-Identifier: Apache-2.0
#include "../hermos_ioctl.hpp"
#include "../device/path.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

/**
 * @brief Read native endpoint metadata and accounting without receiving a payload.
 * @param topic_name Non-null, non-empty fully qualified topic, copied for the ioctl.
 * @param domain_id Exact ROS domain to query in the caller's IPC namespace.
 * @param is_publisher Select publishers when true, subscribers otherwise.
 * @param output Caller-owned buffer of capacity rows; contents are valid only on success.
 * @param capacity Non-null, caller-owned row capacity, at most MAX_SUBSCRIBER_NUM.
 * On ENOBUFS, replaced with the required row count; output remains invalid.
 * @return Number of rows, or negative errno (ENOBUFS for insufficient capacity).
 * @note Unsupported fields are absent from supported_fields, even though their bytes are zero.
 * ENOTTY means unsupported ioctl (including legacy kernels' unknown-command EINVAL),
 * EPROTO means incompatible ABI. Invalid local arguments return EINVAL.
 * This symbol permanently uses the v1 row layout; future layouts need a new symbol.
 */
extern "C" int hermos_query_endpoint_stats_v1(
  const char * topic_name, uint32_t domain_id, bool is_publisher,
  endpoint_stats_ret * output, uint32_t * capacity)
{
  if (!topic_name || !*topic_name || !output || !capacity ||
      *capacity == 0 || *capacity > MAX_SUBSCRIBER_NUM ||
      strnlen(topic_name, TOPIC_NAME_BUFFER_SIZE) == TOPIC_NAME_BUFFER_SIZE) return -EINVAL;
  const int fd = open(hermos_device_path(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  ioctl_endpoint_stats_args args{};
  args.topic_name = {topic_name, strlen(topic_name)};
  args.buffer_addr = reinterpret_cast<uint64_t>(output);
  args.abi_version = HERMOS_ENDPOINT_STATS_ABI_VERSION;
  args.row_size = sizeof(endpoint_stats_ret);
  args.capacity = *capacity;
  args.domain_id = domain_id;
  args.is_publisher = is_publisher;
  const int result = ioctl(fd, HERMOS_GET_ENDPOINT_STATS_CMD, &args);
  const int error = errno;
  close(fd);
  if (result < 0) {
    if (error == ENOBUFS && args.ret_count > *capacity &&
        args.ret_count <= MAX_SUBSCRIBER_NUM) *capacity = args.ret_count;
    return error == EINVAL ? -ENOTTY : -error;
  }
  if (args.ret_count > *capacity) return -EOVERFLOW;
  return static_cast<int>(args.ret_count);
}

