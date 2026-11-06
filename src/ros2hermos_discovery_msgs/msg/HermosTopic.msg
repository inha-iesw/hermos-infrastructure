# One Hermos topic and its endpoints within a single IPC namespace.
string topic_name
# Canonical ROS 2 message type name (e.g. "std_msgs/msg/String"). Empty
# string when the daemon cannot resolve the type (best-effort).
string type_name
# Native ROS domain; identical topic names in different domains stay distinct.
uint32 domain_id
HermosEndpoint[] publishers
HermosEndpoint[] subscribers
