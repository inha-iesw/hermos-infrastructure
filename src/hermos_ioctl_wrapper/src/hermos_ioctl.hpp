#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>

#define MAX_PUBLISHER_NUM 1024   // Maximum number of publishers per topic
#define MAX_TOPIC_LOCAL_ID 4096  // Bitmap size for per-entry subscriber reference tracking
#define MAX_SUBSCRIBER_NUM \
  (MAX_TOPIC_LOCAL_ID - MAX_PUBLISHER_NUM)  // Maximum number of subscribers per topic

#define MAX_TOPIC_NUM 1024
#define MAX_TOPIC_INFO_RET_NUM std::max(MAX_PUBLISHER_NUM, MAX_SUBSCRIBER_NUM)

#define TOPIC_NAME_BUFFER_SIZE 256
#define NODE_NAME_BUFFER_SIZE 256

struct name_info
{
  const char * ptr;
  uint64_t len;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
union ioctl_topic_list_args {
  struct
  {
    uint64_t topic_name_buffer_addr;
    // Parallel array of uint32 domain_ids (one per topic name). Pass 0 to skip.
    // Must mirror hermos_kmod/hermos.h so _IOWR encodes the same size.
    uint64_t domain_id_buffer_addr;
    uint32_t topic_name_buffer_size;
  };
  uint32_t ret_topic_num;
};

union ioctl_node_info_args {
  struct
  {
    struct name_info node_name;
    uint64_t topic_name_buffer_addr;
    uint32_t topic_name_buffer_size;
  };
  uint32_t ret_topic_num;
};

struct topic_info_ret
{
  char node_name[NODE_NAME_BUFFER_SIZE];
  uint32_t qos_depth;
  bool qos_is_transient_local;
  bool qos_is_reliable;
  bool is_bridge;
};

union ioctl_topic_info_args {
  struct
  {
    struct name_info topic_name;
    uint64_t topic_info_ret_buffer_addr;
    uint32_t topic_info_ret_buffer_size;
    // Which domain's endpoints to return (0 = default domain). Must mirror
    // hermos_kmod/hermos.h so _IOWR encodes the same size.
    uint32_t domain_id;
  };
  uint32_t ret_topic_info_ret_num;
};
#pragma GCC diagnostic pop

#define VERSION_BUFFER_LEN 32

struct ioctl_get_version_args
{
  char ret_version[VERSION_BUFFER_LEN];
};

// Mirrors hermos_kmod/hermos.h so _IOW encodes the same size.
struct ioctl_add_domain_bridge_args
{
  // topic_name_from / topic_name_to may differ (rename); equal for a plain bridge.
  struct name_info topic_name_from;
  struct name_info topic_name_to;
  uint32_t from_domain;
  uint32_t to_domain;
};

struct ioctl_discovery_agent_should_exit_args
{
  uint32_t domain_id;
  bool commit;
  bool ret_should_exit;
};

struct ioctl_add_discovery_agent_args
{
  uint32_t domain_id;
  bool ret_already_exists;
};

struct ioctl_discovery_agent_exists_args
{
  uint32_t domain_id;
  bool ret_exists;
};

#define HERMOS_GET_VERSION_CMD _IOR(0xA6, 1, struct ioctl_get_version_args)
#define HERMOS_DISCOVERY_AGENT_SHOULD_EXIT_CMD \
  _IOWR(0xA6, 29, struct ioctl_discovery_agent_should_exit_args)
#define HERMOS_ADD_DISCOVERY_AGENT_CMD _IOWR(0xA6, 30, struct ioctl_add_discovery_agent_args)
#define HERMOS_DISCOVERY_AGENT_EXISTS_CMD \
  _IOWR(0xA6, 31, struct ioctl_discovery_agent_exists_args)
#define HERMOS_GET_TOPIC_LIST_CMD _IOWR(0xA6, 20, union ioctl_topic_list_args)
#define HERMOS_GET_TOPIC_SUBSCRIBER_INFO_CMD _IOWR(0xA6, 21, union ioctl_topic_info_args)
#define HERMOS_GET_TOPIC_PUBLISHER_INFO_CMD _IOWR(0xA6, 22, union ioctl_topic_info_args)
#define HERMOS_GET_NODE_SUBSCRIBER_TOPICS_CMD _IOWR(0xA6, 23, union ioctl_node_info_args)
#define HERMOS_GET_NODE_PUBLISHER_TOPICS_CMD _IOWR(0xA6, 24, union ioctl_node_info_args)
#define HERMOS_ADD_DOMAIN_BRIDGE_CMD _IOW(0xA6, 28, struct ioctl_add_domain_bridge_args)


/**
 * @brief Read-only endpoint statistics ABI v1, owned by hermos_kmod/hermos.h.
 *
 * The wrapper and ctypes mirror this fixed layout. A layout change requires a
 * new ABI version and wrapper symbol; counters exclude application/CUDA completion.
 */
#define HERMOS_ENDPOINT_STATS_ABI_VERSION 1U
#define HERMOS_STATS_PUBLISHED 1U
#define HERMOS_STATS_RECEIVED 2U
#define HERMOS_STATS_HISTORY_DROP 4U
#define HERMOS_STATS_QUEUED 8U
#define HERMOS_STATS_RETAINED 16U

struct endpoint_stats_ret
{
  char node_name[NODE_NAME_BUFFER_SIZE];
  int32_t pid;  /* PID visible to the querying process; zero if outside its PID namespace. */
  int32_t endpoint_id;
  uint32_t domain_id;
  uint32_t qos_depth;
  uint8_t is_publisher;
  uint8_t is_bridge;
  uint8_t is_take_sub;
  uint8_t qos_is_transient_local;
  uint8_t qos_is_reliable;
  uint8_t reserved[3];
  uint64_t published_total;  /* Inserted kernel entries, including retained history. */
  uint64_t received_total;  /* Successful subscriber reference acquisitions. */
  uint64_t dropped_history_total;  /* Unread eligible entries removed by publisher history pruning. */
  uint64_t queued_entries;  /* Resident eligible entries after the subscriber cursor, before QoS. */
  uint64_t retained_entries;  /* Publisher: resident entries; subscriber: held kernel references. */
  uint64_t sample_time_ns;  /* CLOCK_MONOTONIC, only comparable within the same host boot. */
  uint32_t supported_fields;
  uint32_t reserved2;
};

struct ioctl_endpoint_stats_args
{
  struct name_info topic_name;
  uint64_t buffer_addr;
  uint32_t capacity;
  uint32_t domain_id;
  uint8_t is_publisher;
  uint8_t reserved[3];
  uint32_t ret_count;  /* Required rows, copied back on success and -ENOBUFS. */
  uint32_t abi_version;  /* Must equal HERMOS_ENDPOINT_STATS_ABI_VERSION. */
  uint32_t row_size;  /* Caller row stride; mismatches fail before output is touched. */
};

#define HERMOS_GET_ENDPOINT_STATS_CMD _IOWR(0xA6, 34, struct ioctl_endpoint_stats_args)

static_assert(sizeof(endpoint_stats_ret) == 336, "endpoint stats ABI v1 row size");
static_assert(offsetof(endpoint_stats_ret, published_total) == 280, "published offset");
static_assert(offsetof(endpoint_stats_ret, sample_time_ns) == 320, "sample time offset");
static_assert(offsetof(endpoint_stats_ret, supported_fields) == 328, "supported fields offset");
static_assert(sizeof(ioctl_endpoint_stats_args) == 48, "endpoint stats args size");
static_assert(offsetof(ioctl_endpoint_stats_args, abi_version) == 40, "ABI version offset");
