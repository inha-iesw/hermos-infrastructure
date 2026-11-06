# Snapshot of the Hermos state of a single IPC namespace, published by
# the discovery agent on /_hermos_discovery_v2. One message per domain/session.
#
# Schema v2 identifies a domain and supervisor epoch explicitly. ROS message
# definitions must match on both sides; schema_version detects unknown semantic
# versions, not ROS/DDS wire compatibility with a different message definition.

uint32 schema_version
# Writer responsibility is separate from its session/epoch identity.
uint8 WRITER_BRIDGE=1
uint8 WRITER_OBSERVER=2
uint8 writer_role
# Hermos library / runtime version of the publisher. Redundant with
# `schema_version` for correctness but useful for debugging library versions
# using the same v2 wire definition. V1 uses a separate topic.
string hermos_version
string host_uuid                   # ECU identity (per-boot, from /proc/sys/kernel/random/boot_id)
string host_hostname               # Human-readable hostname
uint64 ipc_ns_inode                # NS this message describes
uint32 domain_id
# Character device major:minor, scoped to host_uuid; aliases share this identity.
string device_id
string session_id                  # Supervisor epoch, or standalone agent UUID
# Wall-clock sample time for display only. Do not compare clocks across hosts
# to expire data: readers use local monotonic arrival time and this lease.
uint64 snapshot_time_ns
uint64 lease_duration_ns
HermosTopic[] topics
