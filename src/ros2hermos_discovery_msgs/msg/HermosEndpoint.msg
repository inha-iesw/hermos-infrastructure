# One publisher or subscriber endpoint on an Hermos topic. Field names
# mirror libhermos_ioctl_wrapper's `topic_info_ret` so CLI merge code can
# reuse a single shape.
string node_name
int32 pid
uint32 qos_depth
bool qos_is_transient_local
bool qos_is_reliable
bool is_bridge
