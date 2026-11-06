# HERM0S metadata observation

The discovery agent publishes topic and endpoint metadata on
`/_hermos_discovery_v2`. Observation does not create a payload subscription, borrow
a message, load the heap hook, or export CUDA handles. The agent queries the
same kernel device selected by `HERMOS_DEVICE_PATH` (default `/dev/hermos`).

For a standalone CPU namespace, source the ROS overlay and select the device
and ROS domain used by the applications. Start without a payload preload:

```bash
env -u LD_PRELOAD HERMOS_DEVICE_PATH=/dev/hermos_rclpy ROS_DOMAIN_ID=7 \
  ros2 run ros2hermos_discovery_agent discovery_agent --observation-only
```

Only `--observation-only` or the supervisor factory selects observation mode.
`HERMOS_BRIDGE_MODE=off` controls the native ROS 2/Hermos bridge manager; it does
not change the discovery agent's role. Auto-forked agents therefore still claim
their singleton and honor `--exit-when-idle` when they inherit that variable.
Observation-only processes never claim the kernel's bridge-agent slot and can
coexist with the normal agent. The normal agent still exits successfully when
another bridge-capable instance owns that slot. Observation processes run until
their caller stops them; `--exit-when-idle` applies only to the normal agent.

Different devices, such as `/dev/hermos` and `/dev/hermos_rclpy`, are separate
observation sources even in the same namespace and domain. Each snapshot carries
the character device's `major:minor` identity, scoped to the source host boot UUID.
Aliases of one device share this identity. CLI lists/stats retain both devices;
use `--device-id MAJOR:MINOR` with host/namespace selectors for ambiguous info
queries. This supports concurrent observation, not cross-device native bridging:
the existing bridge-manager socket is scoped only by namespace and domain.
Do not run bridge managers for both devices in the same namespace/domain.
Device numbers identify currently loaded devices, not module reload generations.

Inside a supervised private namespace, use the installed
`hermos_runtime/hermos_metadata_peer` executable as a `HermosPeer` with
`role='metadata'` and `registered=True`. Its
`ros2hermos_discovery_agent.observation:create(context, args)` factory accepts no
application arguments. The runtime initializes the ROS context, invokes the
factory, registers the direct PID with an empty inventory, waits for ACK, and
then dispatches callbacks. The supervisor rejects multiple metadata peers in
the same group and effective ROS domain before startup; the factory does not
use the kernel's bridge singleton for that purpose.
Initialization failure still fails group startup. The factory has no dependency on
`hermos_runtime`; its caller owns
executor, node, context, signals, and process shutdown. Registration readiness
only means the observation process is ready; it is not application-consumer
readiness.

The supervisor factory supplies `HERMOS_SESSION_ID` explicitly for each epoch.
Standalone agents generate their own session UUID even when they inherit that
environment variable, so a colocated bridge agent cannot duplicate the
supervised observer's writer identity. Snapshot identity is the combination of
host boot UUID, IPC namespace inode, domain, device identity, and session. Identical topic names
in different scopes must remain distinct. Each snapshot includes its domain,
session, writer role (bridge or observer), wall-clock measurement
time, and a 30-second lease. Wall-clock time is
informational: readers expire data using local monotonic arrival time. The
reliable, transient-local DDS writer publishes once per second and sets a
30-second lifespan so a late reader cannot receive indefinitely retained old
data. Destroying the node removes the writer; delivery of a final empty snapshot
is not promised. DDS domain and transport profiles must be configured on both
sides when observing a private IPC namespace from outside it.
The reader profile has no lifespan: it uses local monotonic receipt age rather
than comparing an unsynchronized remote timestamp to its own wall clock. Agent
and CLI use the same contract in `observation.protocol`. Automatic cross-namespace
bridge decisions use only bridge-role writers; adding an observer cannot create
forced payload bridges in another namespace. Default CLI views prefer a
bridge-role writer per host/namespace/domain/device, otherwise an observer, selecting
the latest local receipt within that role. Local ioctl queries use the observer
role and remain inside the CLI process.
This avoids duplicate rows and ambiguous info queries from colocated writers.
Other live sessions remain selectable with `--session-id`; different namespaces
and hosts stay distinct. Daemon status checks the registered bridge agent and
requires its local host/namespace/domain/device snapshot, so an observer cannot hide
a bridge agent that has stopped publishing. A failed local topic query skips
publication, bridge dispatch, and idle exit for that tick, then retries.
An unavailable host or device identity makes daemon publication unverifiable;
the status command reports that separately and returns a nonzero status. Agents
skip publication if their device identity cannot be read.
The v2 lease is fixed at 30 seconds; shorter or longer leases are rejected so
an earlier session cannot outlive a more recently received session.

Schema version 2 requires the same generated ROS message definition on agent
and CLI. Its separate topic prevents v1 readers on `/_hermos_discovery` from
deserializing v2 samples. There is no cross-version bridge discovery: upgrade
communicating agents and CLI tools together. The version field detects
unsupported semantics within this wire definition.

PID attribution requires an exact domain and verified process start identity.
Type-only hints prefer the exact domain and native PID when available, but can
fall back to unambiguous legacy rows or use rows whose `/proc` start identity
is inaccessible. Such hints preserve older applications' bridge discovery;
they do not establish a PID or prove a legacy row's domain. Known PID reuse and
conflicting types remain rejected. Registry filenames assume the writer and
reader share a PID namespace; sharing only the IPC namespace is insufficient.
Native statistics omit exited endpoints; older endpoint ioctls cannot always
distinguish a removed publisher retaining borrowed entries. The CLI never
cleans up registry files. The agent removes a writer's file only when a process
existence check confirms its PID no longer exists, including under `hidepid`.

Native endpoint statistics report availability separately from values. An
unsupported kernel, a permission error, and a failed query are distinct from
zero. `supported_fields` has bits `1` published, `2` received, `4` history drop,
`8` queued, and `16` retained. The sample time is source-host `CLOCK_MONOTONIC`.
The v1 ioctl validates version and 336-byte row size before writing output.
Python uses `hermos_query_endpoint_stats_v1`, starting with 64 rows and
retrying once with the required count returned on `ENOBUFS`. This is the only
statistics wrapper symbol; a wrapper without it reports unsupported statistics.
Future row layouts require a new symbol and ABI version. `EPROTO`/`ENOTTY` mean
unsupported; the wrapper normalizes a legacy kernel's unknown-command `EINVAL`
to `ENOTTY` after validating the request. Locally invalid arguments still return
`EINVAL`.

- Published counts successful native publications.
- Received counts successful kernel reference acquisitions, not callbacks or
  GPU completions.
- History drop counts resident entries beyond the subscriber cursor lost to
  publisher history pruning, before subscriber QoS filtering. It includes an
  unread entry even if subscriber KEEP_LAST would later skip it. It does not
  count application drops or skips performed by receive/take QoS filtering.
- Queued counts eligible resident entries beyond the subscriber cursor before
  QoS filtering. It is not an application's work queue.
- Retained counts publisher resident entries or subscriber-held kernel
  references, according to endpoint role.

The metadata agent itself creates no native payload endpoint. Other bridge
observers may appear among native subscribers; counts alone must not be used to
declare an application consumer ready. GPU completion rate is not reported.
