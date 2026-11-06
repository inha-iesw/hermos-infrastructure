## How to use ros2 command for Hermos

Currently, Hermos supports the following `ros2` commands:

- `ros2 topic list_hermos`
- `ros2 topic info_hermos /topic_name`
- `ros2 topic stats_hermos /topic_name`
- `ros2 topic hz_hermos /topic_name`
- `ros2 node list_hermos`
- `ros2 node info_hermos /node_name`

### Observation scope

The list, info, and stats commands collect metadata from `/_hermos_discovery_v2` in the current ROS domain. Native rows retain their source identity: host boot UUID, IPC namespace inode, ROS domain, character device identity (`major:minor`), and agent session. List commands append `[host=... ipc=... domain=... device=... session=...]`; info and stats commands print a `HERM0S host=...` header. The UUIDs, PIDs, and inode below are illustrative.

These commands accept `--gossip-timeout SEC` (default: 2 seconds) and the following selectors:

| Option | Effect |
| :--- | :--- |
| `--local` | Read native metadata from the current IPC namespace without subscribing to discovery metadata. DDS graph queries can still occur. |
| `--session-id ID` | Select one agent/supervisor session. |
| `--ipc-namespace INODE` | Select one IPC namespace. |
| `--host-id UUID` | Select one host boot UUID. |
| `--device-id MAJOR:MINOR` | Select one device on that host. |

Use `ROS_DOMAIN_ID` to choose the domain. Within one host/namespace/domain/device, default commands prefer the bridge agent's snapshot, falling back to the latest observer if no bridge writer is available. Multiple writers in that route produce one default view; `--session-id` explicitly selects an observer or another live session. When different hosts, namespaces, or devices contain the same topic or node name, info commands still require selectors, and list/stats keep those routes separate. A local ioctl fallback may report `session=unknown`, because it has no agent session identity. A local-only query fails if native metadata is unavailable.

`ros2 hermos discovery-daemon-status` checks the registered bridge discovery agent, not observation-only writers. A healthy observer therefore cannot mask a bridge agent that stopped publishing. Its gossip check requires the local host boot UUID, IPC namespace, ROS domain, selected device, and bridge writer role. If local host or device identity is unavailable, it reports that publication could not be verified rather than declaring the agent silent.

```bash
ros2 topic info_hermos /my_topic --host-id boot-uuid --ipc-namespace 4026531839 --session-id agent-uuid
ros2 topic stats_hermos /my_topic --local
```

### Topic List

To list all topics including Hermos, use `ros2 topic list_hermos`.

If a topic is Hermos enabled, it will be shown with a "(Hermos enabled)" suffix.

```bash
$ ros2 topic list_hermos
/topic_name1
/topic_name2 (Hermos enabled) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
/topic_name3
```

If you want to get only Hermos related topics, use `ros2 topic list_hermos | grep Hermos`:

```bash
$ ros2 topic list_hermos | grep Hermos
/topic_name2 (Hermos enabled) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
```

If an Hermos topic is bridged to ROS 2 publishers or subscribers, it is shown with the "(Hermos enabled, bridged)" suffix.

```bash
$ ros2 topic list_hermos
/topic_name1 (Hermos enabled) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
/topic_name2 (Hermos enabled, bridged) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
/topic_name3
```

This does not necessarily mean that a bridge process is currently running. If there is only one Hermos publisher and no ROS 2 subscriber, the topic will be displayed simply as "(Hermos enabled)" rather than including the "bridged" suffix. The "bridged" status indicates that communication has been successfully established between Hermos and ROS 2.

#### Table 1: Pub/Sub situations and display names

The table omits the native scope suffix for readability.

| pub | sub | bridge | display |
| :--- | :--- | :--- | :--- |
| rclcpp::publisher | rclcpp::subscription | off / on | /my_topic |
| hermos::publisher | rclcpp::subscription | off | /my_topic (WARN: Hermos and ROS 2 endpoints exist but bridge is not active) |
| hermos::publisher | rclcpp::subscription | on | /my_topic (Hermos enabled, bridged) |
| rclcpp::publisher | hermos::subscription | off | /my_topic (WARN: Hermos and ROS 2 endpoints exist but bridge is not active) |
| rclcpp::publisher | hermos::subscription | on | /my_topic (Hermos enabled, bridged) |
| hermos::publisher | hermos::subscription | off / on | /my_topic (Hermos enabled) |
| hermos::publisher | non | off / on | /my_topic (Hermos enabled) |
| non | hermos::subscription | off / on | /my_topic (Hermos enabled) |

#### Notes

- If an Hermos topic and a ROS 2 topic share the same name but have different message types, the status will still be displayed as (Hermos enabled, bridged). However, in this case, the actual communication will not be established.

### Topic Info

To show the topic info including Hermos, use `ros2 topic info_hermos /topic_name`.

```bash
$ ros2 topic info_hermos /my_topic
HERM0S host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid
Type: hermos_sample_interfaces/msg/DynamicSizeArray
ROS 2 Publisher count: 0
Hermos Publisher count: 1
ROS 2 Subscription count: 0
Hermos Subscription count: 1
```

For more details, use `--verbose` or `-v`.

```bash
$ ros2 topic info_hermos /my_topic -v
HERM0S host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid
Type: hermos_sample_interfaces/msg/DynamicSizeArray

ROS 2 Publisher count: 0
Hermos Publisher count: 1

Node name: talker_node
Node namespace: /
PID: 86051
Topic type: hermos_sample_interfaces/msg/DynamicSizeArray
Endpoint type: PUBLISHER (Hermos enabled)
QoS profile:
  Reliability: unknown (native publisher does not expose this QoS)
  History (Depth): KEEP_LAST (1)
  Durability: VOLATILE

ROS 2 Subscription count: 0
Hermos Subscription count: 1

Node name: listener_node
Node namespace: /
PID: 86052
Topic type: hermos_sample_interfaces/msg/DynamicSizeArray
Endpoint type: SUBSCRIPTION (Hermos enabled)
QoS profile:
  Reliability: BEST_EFFORT
  History (Depth): KEEP_LAST (1)
  Durability: VOLATILE
```

#### Debug Mode

By default, `ros2 topic info_hermos` hides internal bridge nodes and endpoints to provide a cleaner view. To include these internal details, use the `--debug` or `-d` flag.

```bash
$ ros2 topic info_hermos /my_topic -d
HERM0S host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid
Type: hermos_sample_interfaces/msg/DynamicSizeArray
ROS 2 Publisher count: 1
Hermos Publisher count: 2
ROS 2 Subscription count: 1
Hermos Subscription count: 2
```

You can combine `-d` with `-v` for full verbose output including bridge node details:

```bash
$ ros2 topic info_hermos /my_topic -v -d
HERM0S host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid
Type: hermos_sample_interfaces/msg/DynamicSizeArray

ROS 2 Publisher count: 1
Hermos Publisher count: 2

Node name: hermos_bridge_node_86050
Node namespace: /
Topic type: hermos_sample_interfaces/msg/DynamicSizeArray
Endpoint type: PUBLISHER
GID: 01.10.b2.d2.ef.55.13.0d.74.cc.0c.e6.00.00.16.03.00.00.00.00.00.00.00.00
QoS profile:
  Reliability: RELIABLE
  History (Depth): KEEP_LAST (10)
  Durability: TRANSIENT_LOCAL
  Lifespan: Infinite
  Deadline: Infinite
  Liveliness: AUTOMATIC
  Liveliness lease duration: Infinite

Node name: hermos_bridge_node_86050
Node namespace: /
PID: 86050
Topic type: hermos_sample_interfaces/msg/DynamicSizeArray
Endpoint type: PUBLISHER (Hermos enabled)
QoS profile:
  Reliability: unknown (native publisher does not expose this QoS)
  History (Depth): KEEP_LAST (10)
  Durability: TRANSIENT_LOCAL

Node name: talker_node
Node namespace: /
PID: 86051
Topic type: hermos_sample_interfaces/msg/DynamicSizeArray
Endpoint type: PUBLISHER (Hermos enabled)
QoS profile:
  Reliability: unknown (native publisher does not expose this QoS)
  History (Depth): KEEP_LAST (1)
  Durability: VOLATILE

ROS 2 Subscription count: 1
Hermos Subscription count: 2

Node name: hermos_bridge_node_86050
Node namespace: /
Topic type: hermos_sample_interfaces/msg/DynamicSizeArray
Endpoint type: SUBSCRIPTION
GID: 01.10.b2.d2.ef.55.13.0d.74.cc.0c.e6.00.00.15.04.00.00.00.00.00.00.00.00
QoS profile:
  Reliability: RELIABLE
  History (Depth): KEEP_LAST (1)
  Durability: VOLATILE
  Lifespan: Infinite
  Deadline: Infinite
  Liveliness: AUTOMATIC
  Liveliness lease duration: Infinite

Node name: listener_node
Node namespace: /
PID: 86052
Topic type: hermos_sample_interfaces/msg/DynamicSizeArray
Endpoint type: SUBSCRIPTION (Hermos enabled)
QoS profile:
  Reliability: BEST_EFFORT
  History (Depth): KEEP_LAST (1)
  Durability: VOLATILE

Node name: hermos_bridge_node_86050
Node namespace: /
PID: 86050
Topic type: hermos_sample_interfaces/msg/DynamicSizeArray
Endpoint type: SUBSCRIPTION (Hermos enabled)
QoS profile:
  Reliability: RELIABLE
  History (Depth): KEEP_LAST (1)
  Durability: VOLATILE
```

### Node List

To list all nodes including those implemented with Hermos, use `ros2 node list_hermos`.

Standard ROS 2 nodes are displayed normally, while native nodes are marked with "(Hermos enabled)" and their scope. A name also present in the DDS graph is marked "(Hermos enabled, DDS)"; this does not establish that both endpoints belong to the same process.

```bash
$ ros2 node list_hermos
/ros2_talker_node
/hermos_listener_node (Hermos enabled) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
/mixed_node (Hermos enabled, DDS) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
```

#### Debug Mode

By default, `ros2 node list_hermos` hides internal bridge nodes and endpoints to provide a cleaner view. To include these internal details, use the `--debug` or `-d` flag.

```bash
$ ros2 node list_hermos -d
/ros2_talker_node
/hermos_bridge_node_86050 (Hermos enabled, DDS) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
/hermos_listener_node (Hermos enabled) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
/mixed_node (Hermos enabled, DDS) [host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid]
```

#### Notes

Detection of hermos::Node instances depends on the presence of Hermos-enabled endpoints. A node without at least one Hermos publisher or subscriber will be omitted from the output.

`ros2 node list_hermos -c` counts displayed rows after filtering. The same node name in two native scopes contributes two rows; the result is not a unique-name or process count.

### Node Info

To show the node info including Hermos, use `ros2 node info_hermos /node_name`.

```bash
$ ros2 node info_hermos /listener_node
HERM0S host=boot-uuid ipc=4026531839 domain=0 device=240:0 session=agent-uuid
PIDs: 86052
  Subscribers:
    /parameter_events: rcl_interfaces/msg/ParameterEvent
    /my_topic: DDS=hermos_sample_interfaces/msg/DynamicSizeArray; HERM0S=hermos_sample_interfaces/msg/DynamicSizeArray (Hermos enabled, bridged)
  Publishers:
    /my_topic2: hermos_sample_interfaces/msg/DynamicSizeArray (Hermos enabled)
    /parameter_events: rcl_interfaces/msg/ParameterEvent
    /rosout: rcl_interfaces/msg/Log
  Service Servers:
    /listener_node/describe_parameters: rcl_interfaces/srv/DescribeParameters
    /listener_node/get_parameter_types: rcl_interfaces/srv/GetParameterTypes
    /listener_node/get_parameters: rcl_interfaces/srv/GetParameters
    /listener_node/list_parameters: rcl_interfaces/srv/ListParameters
    /listener_node/set_parameters: rcl_interfaces/srv/SetParameters
    /listener_node/set_parameters_atomically: rcl_interfaces/srv/SetParametersAtomically
  Service Clients:
  Action Servers:
  Action Clients:
```

Similar to `ros2 topic list_hermos`, the (Hermos enabled, bridged) suffix in the node info indicates that communication has been successfully established between Hermos and ROS 2 for that specific topic.

If a topic is Hermos-enabled but not currently bridged (e.g., there is no corresponding ROS 2 publisher/subscriber or the bridge process is not active), it will be displayed simply as (Hermos enabled). This allows you to verify the connectivity status of each topic directly from the node's perspective.

For a topic present in both native metadata and the node's DDS graph, the `DDS=...; HERM0S=...` fields preserve both type labels. Native PID values are shown as `unknown` when unavailable; native publisher reliability is also explicitly unknown rather than inferred from DDS QoS.

DDS type labels come from the queried node's graph. A matching topic on another
node is not used to fill in a missing native type.

### Native Topic Statistics

`ros2 topic stats_hermos /my_topic` reports cumulative native counters grouped by source scope, without subscribing to the payload topic or starting a bridge. Use `--local` to query the current namespace directly, or the scope selectors above to inspect a particular metadata source.

The output includes the snapshot wall-clock timestamp and each available endpoint's monotonic sample timestamp. These source timestamps are not a shared clock across machines. Unsupported statistics and individual fields are shown as `unsupported`, not as zero.

| Field | Native meaning |
| :--- | :--- |
| `published_total` | Publisher's cumulative publish count. |
| `received_total` | Subscriber's kernel reference acquisitions, not completed callbacks. |
| `dropped_history_total` | Unread samples lost from publisher history. |
| `queued_entries` | Unread resident entries before subscriber QoS filtering. |
| `retained_entries` | Publisher resident entries, or subscriber-held kernel references. |

### DDS Reception Frequency

`ros2 topic hz_hermos /my_topic` measures reception through DDS. It may create a dummy subscriber to trigger the bridge before running the standard ROS 2 frequency command, and prints this notice on stdout:

```text
Measuring DDS bridge reception frequency. Native counters: ros2 topic stats_hermos.
```

Use `stats_hermos` for native counters. The `hz_hermos` result includes bridge and DDS effects and is not a native publish-rate measurement.
