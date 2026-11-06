"""Per-IPC-namespace discovery agent.

Reads the local Hermos state via the existing NS-scoped ioctl wrapper
(libhermos_ioctl_wrapper.so) and publishes it as HermosDaemonState on
``/_hermos_discovery_v2`` so other namespaces and ECUs running ros2hermos
tooling can observe and make bridge generation decisions.

One bridge-capable daemon is intended to run per (IPC namespace, ROS_DOMAIN_ID):
gossip is published on the agent's DDS domain, so a namespace shared by
launches in different domains needs one agent per domain. To make duplicate
launches (e.g. one node spawning the agent and another ``ros2 launch``
session also trying to spawn one) safe, the agent claims a singleton slot in
the kernel module (the ``add_discovery_agent`` ioctl) before starting;
subsequent instances in the same (ns, domain) lose the claim and exit
cleanly (code 0), leaving exactly one live agent per (ns, domain). The
kmod is the single source of truth for agent liveness, so the idle self-exit
is decided atomically against new processes and can never orphan a namespace
(a process starting during the grace period keeps the agent running).
Observation-only processes do not claim this slot and can coexist with that daemon.
"""

import ctypes
import importlib.metadata
import logging
import os
import socket
import sys
import time
import uuid

import rclpy
from rclpy.executors import ExternalShutdownException
from rclpy.node import Node
from rclpy.parameter import Parameter

from ros2hermos_discovery_msgs.msg import (
    HermosDaemonState,
    HermosEndpoint,
    HermosTopic,
)

from . import bridge_decider
from .type_registry import TypeRegistryReader
from .observation.protocol import (
    GOSSIP_TOPIC, LEASE_NS, latest_per_route, local_device_id, make_snapshot, reader_qos,
    scope_key, validate_snapshot, writer_qos,
    WRITER_BRIDGE, WRITER_OBSERVER,
)


PUBLISH_INTERVAL_SEC = 1.0
# Preferred over /etc/machine-id: avoids the systemd dep and the
# baked-into-image case where every ECU collides on one host_uuid.
BOOT_ID_PATH = '/proc/sys/kernel/random/boot_id'
SELF_IPC_NS_PATH = '/proc/self/ns/ipc'
# Kept separate from TOPIC_NAME_BUFFER_SIZE because the C side (`hermos_kmod`)
# defines NODE_NAME_BUFFER_SIZE and TOPIC_NAME_BUFFER_SIZE as independent
# constants — they happen to share the value 256 today.
NODE_NAME_BUFFER_SIZE = 256
TOPIC_NAME_BUFFER_SIZE = 256
# How long a remote daemon's last snapshot may sit in _remote_states without
# being refreshed before we drop it. Matches the gossip Liveliness lease so
# DDS-side liveliness loss and local prune happen on the same timescale.
REMOTE_STATE_STALE_SEC = LEASE_NS / 1e9
# Opt-in idle-exit (--exit-when-idle CLI flag or the env var): the agent exits
# after its (IPC NS, domain) has had no Hermos node for EXIT_WHEN_IDLE_GRACE_SEC.
# Off by default, so a launch-started agent runs until the launch stops it.
EXIT_WHEN_IDLE_FLAG = '--exit-when-idle'
EXIT_WHEN_IDLE_ENV = 'HERMOS_DISCOVERY_AGENT_EXIT_WHEN_IDLE'
EXIT_WHEN_IDLE_GRACE_SEC = 30.0


def _exit_when_idle_enabled(argv=None) -> bool:
    if argv is None:
        argv = sys.argv
    if EXIT_WHEN_IDLE_FLAG in argv:
        return True
    return os.environ.get(EXIT_WHEN_IDLE_ENV, '').strip().lower() in ('1', 'true', 'yes')


class IdleExitTracker:
    """Reports idle after ``threshold`` consecutive idle ticks.

    ``update()`` returns True on every idle tick at or beyond the threshold
    (on every such tick, not just the first); a single non-idle tick resets the
    count, so a brief gap (e.g. a node restarting) does not trigger an exit.
    """

    def __init__(self, threshold: int):
        self._threshold = max(1, threshold)
        self._idle_count = 0

    def update(self, is_idle: bool) -> bool:
        self._idle_count = self._idle_count + 1 if is_idle else 0
        return self._idle_count >= self._threshold

    def reset(self) -> None:
        self._idle_count = 0


class TopicInfoRet(ctypes.Structure):
    """Mirror of ``struct topic_info_ret`` in hermos_ioctl.hpp."""

    _fields_ = [
        ('node_name', ctypes.c_char * NODE_NAME_BUFFER_SIZE),
        ('qos_depth', ctypes.c_uint32),
        ('qos_is_transient_local', ctypes.c_bool),
        ('qos_is_reliable', ctypes.c_bool),
        ('is_bridge', ctypes.c_bool),
    ]


def load_query_library(*, registration=True):
    """Load query bindings; only agent callers need the singleton registration API."""
    lib = ctypes.CDLL('libhermos_ioctl_wrapper.so')

    lib.get_hermos_topics.argtypes = [
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32)),
    ]
    lib.get_hermos_topics.restype = ctypes.POINTER(ctypes.POINTER(ctypes.c_char))
    lib.free_hermos_topics.argtypes = [
        ctypes.POINTER(ctypes.POINTER(ctypes.c_char)),
        ctypes.c_int,
    ]
    lib.free_hermos_topics.restype = None
    lib.free_hermos_topic_domains.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
    lib.free_hermos_topic_domains.restype = None

    lib.get_hermos_sub_nodes.argtypes = [
        ctypes.c_char_p, ctypes.POINTER(ctypes.c_int), ctypes.c_uint32]
    lib.get_hermos_sub_nodes.restype = ctypes.POINTER(TopicInfoRet)
    lib.get_hermos_pub_nodes.argtypes = [
        ctypes.c_char_p, ctypes.POINTER(ctypes.c_int), ctypes.c_uint32]
    lib.get_hermos_pub_nodes.restype = ctypes.POINTER(TopicInfoRet)
    lib.free_hermos_topic_info_ret.argtypes = [ctypes.POINTER(TopicInfoRet)]
    lib.free_hermos_topic_info_ret.restype = None

    if registration:
        lib.hermos_discovery_agent_register.argtypes = [ctypes.c_uint32]
        lib.hermos_discovery_agent_register.restype = ctypes.c_int
        lib.hermos_discovery_agent_should_exit.argtypes = [ctypes.c_uint32]
        lib.hermos_discovery_agent_should_exit.restype = ctypes.c_int
        lib.hermos_discovery_agent_commit_exit.argtypes = [ctypes.c_uint32]
        lib.hermos_discovery_agent_commit_exit.restype = ctypes.c_int

    from .observation.snapshot import configure_endpoint_stats
    configure_endpoint_stats(lib)

    return lib


def _ioctl_to_endpoint(
        info: TopicInfoRet, topic_name: str, role: str,
        registry: TypeRegistryReader | None = None, *, domain_id,
        statistics_status='unsupported') -> HermosEndpoint:
    """Convert one ``topic_info_ret`` row to an HermosEndpoint msg.

    ``pid`` is looked up from the tmpfs type registry (written by hermoslib
    at Publisher/Subscription construction time) using
    ``(domain_id, topic_name, role, node_name)`` as the join key, with the
    writer's process start identity verified. When no match is found
    the field stays 0, which lets the rest of the pipeline degrade gracefully
    (the gossip publication still flows; just no pid for that endpoint).
    """
    ep = HermosEndpoint()
    ep.node_name = info.node_name.decode('utf-8', errors='replace')
    ep.pid = 0
    if registry is not None:
        entry = registry.lookup(topic_name, role, ep.node_name, domain_id=domain_id)
        if entry is not None:
            ep.pid = entry.pid
    ep.qos_depth = info.qos_depth
    ep.qos_is_transient_local = info.qos_is_transient_local
    ep.qos_is_reliable = info.qos_is_reliable
    ep.is_bridge = info.is_bridge
    ep.endpoint_id = -1
    ep.statistics_status = statistics_status
    return ep


def _native_to_endpoint(info) -> HermosEndpoint:
    """Copy only public counters and identity; no IPC handles leave this process."""
    ep = HermosEndpoint()
    ep.node_name = info.node_name.decode('utf-8', errors='replace')
    ep.pid = info.pid
    ep.endpoint_id = info.endpoint_id
    ep.qos_depth = info.qos_depth
    for field in ('qos_is_transient_local', 'qos_is_reliable', 'is_bridge', 'is_take_sub'):
        setattr(ep, field, bool(getattr(info, field)))
    ep.statistics_status = 'available'
    from .observation.snapshot import COUNTERS
    for field in ('supported_fields', 'sample_time_ns', *(name for _, name in COUNTERS)):
        setattr(ep, field, getattr(info, field))
    return ep


def read_local_topics(
        lib, registry: TypeRegistryReader | None = None,
        own_domain_id: int | None = None) -> list:
    """Snapshot the current namespace's Hermos topics via the ioctl wrapper.

    Returns a list of HermosTopic msgs. The ioctl returns only the caller's
    IPC namespace, so the daemon process just being inside that namespace is
    sufficient to scope the result. The optional ``registry`` argument
    supplies the type names and pids that the ioctl does not expose.

    When ``own_domain_id`` is given, only topics in that domain are returned.
    Each agent is per-(IPC namespace, domain) and gossips on its own DDS domain,
    so it must report only its own domain's topics; otherwise it would leak
    other domains' endpoints onto its channel and let its bridge decider issue
    requests for domains it does not own.
    """
    topic_count = ctypes.c_int()
    # The wrapper allocates the domain array (sized to match the name buffer) and
    # returns it here; we own it and free it below.
    domain_ids_ptr = ctypes.POINTER(ctypes.c_uint32)()
    topic_names_ptr = lib.get_hermos_topics(
        ctypes.byref(topic_count), ctypes.pointer(domain_ids_ptr))
    topics = []
    if not topic_names_ptr:
        if topic_count.value < 0:
            code = -topic_count.value
            raise OSError(code, 'HERM0S topic query failed: ' + os.strerror(code))
        return topics

    try:
        for i in range(topic_count.value):
            topic_name_b = ctypes.cast(topic_names_ptr[i], ctypes.c_char_p).value
            topic_name = topic_name_b.decode('utf-8', errors='replace')
            # The same topic name can occur once per domain; each row is a
            # distinct (name, domain) pair that becomes its own HermosTopic.
            domain_id = domain_ids_ptr[i]
            if own_domain_id is not None and domain_id != own_domain_id:
                continue

            hermos_topic = HermosTopic()
            hermos_topic.topic_name = topic_name
            hermos_topic.type_name = ''
            hermos_topic.domain_id = domain_id
            hermos_topic.publishers = _collect_endpoints(
                lib.get_hermos_pub_nodes, lib, topic_name_b, topic_name, domain_id, 'pub',
                registry)
            hermos_topic.subscribers = _collect_endpoints(
                lib.get_hermos_sub_nodes, lib, topic_name_b, topic_name, domain_id, 'sub',
                registry)
            # Never choose one type from conflicting process announcements.
            if registry is not None:
                resolved = _resolve_topic_type(hermos_topic, registry)
                if resolved:
                    hermos_topic.type_name = resolved
            topics.append(hermos_topic)
    finally:
        lib.free_hermos_topics(topic_names_ptr, topic_count.value)
        lib.free_hermos_topic_domains(domain_ids_ptr)

    return topics


def _resolve_topic_type(
        hermos_topic: HermosTopic, registry: TypeRegistryReader) -> str:
    types = set()
    for role, endpoints in (('pub', hermos_topic.publishers), ('sub', hermos_topic.subscribers)):
        for ep in endpoints:
            type_name = registry.lookup_type(
                hermos_topic.topic_name, role, ep.node_name, domain_id=hermos_topic.domain_id,
                pid=ep.pid or None)
            if type_name:
                types.add(type_name)
    return next(iter(types)) if len(types) == 1 else ''


def _collect_endpoints(
        getter, lib, topic_name_b: bytes, topic_name: str, domain_id: int, role: str,
        registry: TypeRegistryReader | None = None) -> list:
    from .observation.snapshot import read_endpoint_stats

    status, rows = read_endpoint_stats(lib, topic_name, domain_id, role == 'pub')
    if status == 'available':
        return [_native_to_endpoint(row) for row in rows]
    count = ctypes.c_int()
    array = getter(topic_name_b, ctypes.byref(count), domain_id)
    endpoints = []
    if not array:
        return endpoints
    try:
        for i in range(count.value):
            endpoints.append(_ioctl_to_endpoint(
                array[i], topic_name, role, registry, domain_id=domain_id,
                statistics_status=status))
    finally:
        lib.free_hermos_topic_info_ret(array)
    return endpoints


def _read_host_uuid() -> str:
    """Return the boot UUID; fall back to a random UUID with a WARN log."""
    try:
        with open(BOOT_ID_PATH) as fp:
            return str(uuid.UUID(fp.read().strip()))
    except (OSError, ValueError) as exc:
        fallback = str(uuid.uuid4())
        logging.warning(
            'hermos_discovery_agent: failed to read %s (%s); '
            'falling back to random host_uuid=%s',
            BOOT_ID_PATH, exc, fallback)
        return fallback


def _read_ipc_ns_inode() -> int:
    """Return the inode number of the daemon's own IPC namespace."""
    return os.stat(SELF_IPC_NS_PATH).st_ino


def _read_ros_domain_id() -> int:
    """Return ROS_DOMAIN_ID from the environment (0 if unset, empty,
    unparsable, or outside the uint32 range), matching ROS 2's default and
    the kmod's get_ros_domain_id. An out-of-range value must not slip through:
    it would wrap into an unintended domain when passed via the uint32 ioctl."""
    raw = os.environ.get('ROS_DOMAIN_ID')
    if not raw:
        return 0
    try:
        value = int(raw)
    except ValueError:
        return 0
    if value < 0 or value > 0xFFFFFFFF:
        return 0
    return value


def _gossip_qos():
    """Writer profile; subscriptions must use the shared reader profile."""
    return writer_qos()


def _read_agent_version() -> str:
    """Return this package's installed version, or '' if running from source."""
    try:
        return importlib.metadata.version('ros2hermos_discovery_agent')
    except importlib.metadata.PackageNotFoundError:
        return ''


class DiscoveryAgent(Node):
    """rclpy Node that publishes the local Hermos state every PUBLISH_INTERVAL_SEC.

    The default mode also listens to gossip and requests cross-namespace
    bridges. ``observation_only`` creates only the metadata writer; a supervisor
    owns its executor and must acknowledge registration before dispatch.
    """

    def __init__(
        self,
        registry: TypeRegistryReader | None = None,
        *,
        exit_when_idle: bool | None = None,
        context=None,
        observation_only: bool = False,
        session_id: str | None = None,
    ):
        self._host_uuid = _read_host_uuid()
        self._host_hostname = socket.gethostname()
        self._ipc_ns_inode = _read_ipc_ns_inode()
        self._domain_id = context.get_domain_id() if context is not None else _read_ros_domain_id()
        # The supervisor factory supplies its epoch explicitly. Standalone
        # writers need their own identity even when they inherit that environment.
        self._session_id = session_id or str(uuid.uuid4())
        self._observation_only = observation_only
        # Multiple agents may run on one ECU (one per (IPC NS, domain)); disambiguate.
        host_short = self._host_uuid.replace('-', '')[:8]
        observer_suffix = f'_observer_{os.getpid()}' if observation_only else ''
        # Force use_sim_time=False so the 1 Hz timer + clock reads are
        # wall-clock regardless of /clock; sim-time playback would otherwise
        # stretch the publish cadence past the liveliness lease window.
        super().__init__(
            f'hermos_discovery_agent_{host_short}_{self._ipc_ns_inode}_d{self._domain_id}'
            f'{observer_suffix}',
            parameter_overrides=[Parameter('use_sim_time', value=False)],
            context=context,
        )
        try:
            self._initialize(registry, exit_when_idle)
        except BaseException:
            self.destroy_node()
            raise

    def _initialize(self, registry, exit_when_idle):
        self._lib = load_query_library(registration=not self._observation_only)
        self._hermos_version = _read_agent_version()
        self._registry = registry if registry is not None else TypeRegistryReader(
            self._ipc_ns_inode, logger=self.get_logger())

        qos = _gossip_qos()
        self._pub = self.create_publisher(HermosDaemonState, GOSSIP_TOPIC, qos)
        self._sub = None
        if not self._observation_only:
            self._sub = self.create_subscription(
                HermosDaemonState, GOSSIP_TOPIC, self._on_remote_state, reader_qos())
        self._remote_states = {}

        if exit_when_idle is None:
            exit_when_idle = _exit_when_idle_enabled()
        # Atomic idle-exit belongs to the registered bridge agent. Observers are
        # controlled by their caller and must never clear another agent's slot.
        self._exit_when_idle = exit_when_idle and not self._observation_only
        self._idle_tracker = IdleExitTracker(
            threshold=round(EXIT_WHEN_IDLE_GRACE_SEC / PUBLISH_INTERVAL_SEC))

        self._timer = self.create_timer(PUBLISH_INTERVAL_SEC, self._on_tick)

        self.get_logger().info(
            f'hermos_discovery_agent up: host_uuid={self._host_uuid} '
            f'hostname={self._host_hostname} ipc_ns_inode={self._ipc_ns_inode} '
            f'version={self._hermos_version}')

    def _on_tick(self) -> None:
        self._registry.cleanup_dead_pids()
        self._registry.rebuild()
        self._prune_stale_remote_states()
        try:
            snapshot = self.publish_snapshot()
        except OSError as exc:
            self.get_logger().warning(f'local metadata unavailable; skipping snapshot: {exc}')
            self._idle_tracker.reset()
            return
        if not self._observation_only:
            self._dispatch_bridge_requests(snapshot)
        if self._exit_when_idle:
            self._maybe_exit_when_idle()

    def _maybe_exit_when_idle(self) -> None:
        """Exit once this (IPC NS, domain) has had no Hermos node for the grace period.

        A query error counts as "not idle", so a transient failure never exits. When the grace
        period elapses the exit is gated on the kmod's atomic commit, which only deregisters (and
        clears us to exit) if the domain is still empty -- so a process that started during the
        grace period vetoes the exit and the agent keeps serving it instead of orphaning it.
        """
        ret = self._lib.hermos_discovery_agent_should_exit(self._domain_id)
        if ret < 0:
            self._idle_tracker.reset()
            return
        if self._idle_tracker.update(ret == 1):
            if self._lib.hermos_discovery_agent_commit_exit(self._domain_id) == 1:
                self.get_logger().info(
                    f'no Hermos node in (ipc_ns={self._ipc_ns_inode}, domain={self._domain_id}) '
                    f'for {EXIT_WHEN_IDLE_GRACE_SEC:.0f}s; exiting.')
                raise ExternalShutdownException()
            # A process raced in during the grace period; the kmod vetoed the exit.
            self._idle_tracker.reset()

    def _prune_stale_remote_states(
            self, now_sec: float | None = None,
            stale_after_sec: float = REMOTE_STATE_STALE_SEC) -> None:
        """Drop ``_remote_states`` entries whose local arrival time is too old.

        Mirrors the DDS Liveliness lease so dead-peer snapshots don't leak
        memory. Cross-ECU clocks aren't synced, so local arrival time is
        the freshness reference rather than the publisher's stamp.
        """
        if now_sec is None:
            now_sec = time.monotonic()
        stale_keys = [
            key for key, (_msg, received_at) in self._remote_states.items()
            if now_sec - received_at >= min(stale_after_sec, _msg.lease_duration_ns / 1e9)
        ]
        for key in stale_keys:
            del self._remote_states[key]

    def publish_snapshot(self) -> HermosDaemonState:
        """Build and publish the current local HermosDaemonState."""
        msg = self.build_state()
        self._pub.publish(msg)
        return msg

    def _dispatch_bridge_requests(self, local_state: HermosDaemonState) -> None:
        if not self._remote_states:
            return
        remote_states = latest_per_route(self._remote_states.values())
        requests = bridge_decider.decide_bridges(local_state, remote_states)
        if requests:
            bridge_decider.dispatch_requests(
                requests, self._ipc_ns_inode, logger=self.get_logger())

    def build_state(self) -> HermosDaemonState:
        return make_snapshot(
            hermos_version=self._hermos_version, host_uuid=self._host_uuid,
            hostname=self._host_hostname, ipc_ns_inode=self._ipc_ns_inode,
            domain_id=self._domain_id, session_id=self._session_id,
            device_id=local_device_id(),
            writer_role=WRITER_OBSERVER if self._observation_only else WRITER_BRIDGE,
            topics=read_local_topics(self._lib, self._registry, self._domain_id))

    def _on_remote_state(self, msg: HermosDaemonState) -> None:
        try:
            validate_snapshot(msg)
        except ValueError:
            return
        if msg.writer_role != WRITER_BRIDGE or msg.domain_id != self._domain_id:
            return
        if msg.host_uuid == self._host_uuid and msg.ipc_ns_inode == self._ipc_ns_inode:
            return
        received_at = time.monotonic()
        key = scope_key(msg)
        self._remote_states[key] = (msg, received_at)

    @property
    def remote_states(self) -> dict:
        """Map of ``(host, namespace, domain, device, session)`` to message and arrival time."""
        return self._remote_states


def main(argv=None) -> int:
    arguments = sys.argv if argv is None else argv
    observation_only = '--observation-only' in arguments
    if not observation_only:
        # Claim the per-(IPC namespace, domain) singleton in the kmod before any DDS / ioctl work.
        # The kmod decides the claim atomically, so a duplicate loses and exits cleanly (0); an
        # ioctl error exits 1.
        #
        # A duplicate that exits early *could* make launch_test treat it as a crashed node and tear
        # down the launch tree. That's not a risk here: the launch emits one agent per tree, so a lost
        # claim always means a separate launch/run — never a same-tree sibling — which exiting can't
        # affect.
        domain_id = _read_ros_domain_id()
        try:
            lib = load_query_library()
            claim = lib.hermos_discovery_agent_register(domain_id)
        except (OSError, AttributeError) as e:
            # Missing library or symbol (e.g. version skew): fail cleanly instead of a traceback.
            sys.stderr.write(f'hermos_discovery_agent: cannot use the ioctl wrapper: {e}\n')
            return 1
        if claim == 1:
            sys.stderr.write(
                'hermos_discovery_agent: another instance is already running in this '
                f'(IPC namespace, domain={domain_id}); exiting.\n')
            return 0
        if claim < 0:
            # Don't masquerade as "already running" — propagate failure so supervisors can detect it.
            return 1

    rclpy.init(args=[arg for arg in arguments if arg != '--observation-only'])
    node = None
    try:
        node = DiscoveryAgent(
            exit_when_idle=_exit_when_idle_enabled(arguments), observation_only=observation_only)
        rclpy.spin(node)
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    except (OSError, AttributeError) as exc:
        sys.stderr.write(f'hermos_discovery_agent: initialization/query failed: {exc}\n')
        return 1
    finally:
        if node is not None:
            node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
