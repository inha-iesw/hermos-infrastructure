# One publisher or subscriber endpoint on an Hermos topic. Field names
# mirror libhermos_ioctl_wrapper's `topic_info_ret` so CLI merge code can
# reuse a single shape.
string node_name
int32 pid
uint32 qos_depth
bool qos_is_transient_local
bool qos_is_reliable
bool is_bridge
# Native endpoint identity is local to (host, IPC namespace, domain, session).
int32 endpoint_id
bool is_take_sub
# available, unsupported, permission_denied, or unavailable. Zero counters are
# meaningful only for fields whose supported_fields bit is set.
string statistics_status
# Bits: published=1, received=2, history drop=4, queued=8, retained=16.
uint32 supported_fields
uint64 published_total
# Successful kernel reference acquisitions; not callbacks or GPU completions.
uint64 received_total
# Unread eligible entries lost to publisher history pruning, before subscriber
# QoS filtering (including entries KEEP_LAST would later skip).
uint64 dropped_history_total
# Eligible resident entries after the subscriber cursor, before QoS filtering.
uint64 queued_entries
# Publisher resident entries, or subscriber-held kernel references.
uint64 retained_entries
# CLOCK_MONOTONIC observation time, meaningful only within the source host boot.
uint64 sample_time_ns
