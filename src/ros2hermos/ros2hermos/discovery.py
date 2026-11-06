"""Subscribe to /_hermos_discovery_v2; fall back to ioctl when no agent gossips.

Metadata lifetime is bounded by publisher lifespan and local receipt leases.
"""

import argparse
import sys
import time

import rclpy

from ros2hermos._ioctl import self_ns_snapshot
from ros2hermos.observation.scope import current_domain, SnapshotCache

from ros2hermos_discovery_msgs.msg import HermosDaemonState, HermosEndpoint


from ros2hermos_discovery_agent.observation.protocol import GOSSIP_TOPIC, reader_qos

DEFAULT_COLLECT_TIMEOUT_SEC = 2.0


def add_gossip_timeout_arg(parser) -> None:
    """Add the hidden, transitional ``--gossip-timeout`` flag to a verb's parser.

    Suppressed from ``--help`` because the plan is to replace the fixed
    wait with a ros2-daemon-driven stop condition. Use
    :func:`warn_if_gossip_timeout_overridden` in ``main()`` to nudge any
    operator who passes it explicitly.
    """
    parser.add_argument(
        '--gossip-timeout',
        type=float,
        default=DEFAULT_COLLECT_TIMEOUT_SEC,
        help=argparse.SUPPRESS)


def warn_if_gossip_timeout_overridden(args) -> None:
    """Emit a stderr WARN when ``--gossip-timeout`` is set to a non-default."""
    if args.gossip_timeout != DEFAULT_COLLECT_TIMEOUT_SEC:
        print(
            'WARNING: --gossip-timeout is unsupported and will be '
            'removed once ros2-daemon-driven discovery lands.',
            file=sys.stderr)


def gossip_qos():
    """Use the backend reader contract without a remote wall-clock lifespan."""
    return reader_qos()


def collect_announcements(
    node,
    timeout_sec: float = DEFAULT_COLLECT_TIMEOUT_SEC,
) -> list[HermosDaemonState]:
    """Collect fresh snapshots while preserving host, namespace, domain and session.

    Returns one ``HermosDaemonState`` per gossiping agent, or ``[]`` when no
    agent is reachable within ``timeout_sec``.
    """
    if timeout_sec <= 0:
        return []
    snapshots = SnapshotCache()

    def on_msg(msg: HermosDaemonState, info=None) -> None:
        # Humble invokes callbacks with only the message. Newer rclpy versions
        # may provide MessageInfo; use its writer identity when available.
        raw_gid = (info.get('publisher_gid') if isinstance(info, dict)
                   else getattr(info, 'publisher_gid', None))
        gid = bytes(raw_gid) if raw_gid is not None else None
        try:
            snapshots.update(msg, received_at=time.monotonic(), publisher_gid=gid)
        except ValueError as exc:
            print(f'WARNING: {exc}; ignoring metadata snapshot.', file=sys.stderr)

    spin_node = _resolve_spin_node(node)
    sub = spin_node.create_subscription(
        HermosDaemonState, GOSSIP_TOPIC, on_msg, gossip_qos())
    try:
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            rclpy.spin_once(spin_node, timeout_sec=0.05)
            try:
                publishers = spin_node.get_publishers_info_by_topic(GOSSIP_TOPIC)
            except Exception:
                publishers = None
            live_gids = ({bytes(publisher.endpoint_gid) for publisher in publishers}
                         if publishers is not None else None)
            active = snapshots.active(now=time.monotonic(), live_gids=live_gids)
            # Return as soon as every visible publisher has reported, rather
            # than waiting out timeout_sec (now just an upper bound).
            # One agent = one publisher = one (host, namespace, domain, session) snapshot,
            # so the publisher count is how many snapshots to expect.
            if publishers and len(active) >= len(publishers):
                break
    finally:
        spin_node.destroy_subscription(sub)

    return snapshots.active(now=time.monotonic())


def collect_announcements_with_fallback(
    node,
    timeout_sec: float = DEFAULT_COLLECT_TIMEOUT_SEC,
    *,
    strict: bool = False,
) -> tuple[list[HermosDaemonState], bool]:
    """Gossip first, then ioctl fallback. Returns ``(snapshots, used_fallback)``.

    - ``used_fallback`` is False when at least one agent gossiped; ``snapshots``
      is then every reachable agent's state.
    - Otherwise it is True and ``snapshots`` is the local-NS ioctl view. Empty
      topics mean the query succeeded but found no registered topic.
    - A missing/inaccessible device or wrapper returns ``[]`` with a stderr
      note, so DDS-only commands remain usable. ``strict=True`` instead raises
      OSError for device errors or returns ``[]`` for a missing wrapper, allowing
      explicit native-only queries to report failure.
    """
    snapshots = collect_announcements(node, timeout_sec) if timeout_sec > 0 else []
    if snapshots:
        return snapshots, False
    try:
        fallback = self_ns_snapshot(domain_id=current_domain(node))
    except OSError as exc:
        if strict:
            raise
        print(f'NOTE: {local_metadata_error(exc)}; continuing with DDS information.',
              file=sys.stderr)
        return [], True
    if fallback is None and not strict:
        print('NOTE: Local HERM0S metadata unavailable: ioctl wrapper missing or incompatible; '
              'continuing with DDS information.', file=sys.stderr)
    return ([fallback] if fallback is not None else []), True


def local_metadata_error(error: OSError) -> str:
    """Describe local query failure without confusing it with an empty domain."""
    import errno
    status = ('permission_denied' if error.errno in (errno.EACCES, errno.EPERM)
              else 'unavailable')
    return f'Local HERM0S metadata {status}: {error}'


def _resolve_spin_node(node):
    """Return the underlying ``rclpy.Node`` if ``node`` is a ``NodeStrategy``.

    ``NodeStrategy`` is a ros2cli dispatcher, not an ``rclpy.Node``; passing
    it to executor APIs like ``rclpy.spin_once`` is outside its intended use.
    """
    direct = getattr(node, 'direct_node', None)
    if direct is None:
        return node
    return getattr(direct, 'node', direct)


def warn_if_using_fallback(
    node, used_fallback: bool, timeout_sec: float, snapshots: list[HermosDaemonState]
) -> None:
    """Best-effort stderr note when gossip was unavailable and ioctl fallback ran.

    ``snapshots`` is the fallback result from ``collect_announcements_with_fallback``.
    A successful query with no endpoints returns a state with empty topics.
    Device/wrapper failures return ``[]`` and already emit a separate note.
    """
    if not used_fallback or timeout_sec <= 0:
        return

    spin_node = _resolve_spin_node(node)
    try:
        publishers = spin_node.get_publishers_info_by_topic(GOSSIP_TOPIC)
    except Exception:
        publishers = []

    if not publishers:
        # No agent is gossiping. Only worth a note when this namespace actually
        # has Hermos endpoints to show: ros2 CLI runs constantly in namespaces
        # without Hermos, where "no agent" would be misleading noise.
        if not any(snap.topics for snap in snapshots):
            return
        print(
            'NOTE: no /_hermos_discovery_v2 agent visible; showing local '
            'NS only via ioctl. Start one with '
            '`ros2 run ros2hermos_discovery_agent discovery_agent` to '
            'see other NSes / ECUs.',
            file=sys.stderr)
    else:
        print(
            f'WARNING: /_hermos_discovery_v2 has {len(publishers)} publisher(s) '
            f'visible but no snapshot received in {timeout_sec:.1f}s; falling '
            'back to ioctl (local NS only). Try '
            '`ros2 daemon stop && ros2 daemon start` (the ros2 daemon may be '
            'missing the discovery msg package on its PYTHONPATH).',
            file=sys.stderr)


def all_topic_names(snapshots: list[HermosDaemonState]) -> set[str]:
    """Union of topic names across all snapshots."""
    return {topic.topic_name for snap in snapshots for topic in snap.topics}


def all_nodes(snapshots: list[HermosDaemonState]) -> set[str]:
    """Union of node names across all snapshots."""
    nodes = set()
    for snap in snapshots:
        for topic in snap.topics:
            for endpoint in topic.publishers:
                nodes.add(endpoint.node_name)
            for endpoint in topic.subscribers:
                nodes.add(endpoint.node_name)
    return nodes


def topic_endpoints(
    snapshots: list[HermosDaemonState], topic_name: str
) -> tuple[list[HermosEndpoint], list[HermosEndpoint]]:
    """Return (publishers, subscribers) for ``topic_name`` across all snapshots."""
    publishers = []
    subscribers = []
    for snap in snapshots:
        for topic in snap.topics:
            if topic.topic_name != topic_name:
                continue
            publishers.extend(topic.publishers)
            subscribers.extend(topic.subscribers)
    return publishers, subscribers


def topics_of_node(
    snapshots: list[HermosDaemonState], node_name: str
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    """Return (pub topics, sub topics) for ``node_name`` as {topic_name, type_name} dicts."""
    pubs, subs = [], []
    for snap in snapshots:
        for topic in snap.topics:
            for endpoint in topic.publishers:
                if endpoint.node_name == node_name:
                    pubs.append({'topic_name': topic.topic_name,
                                 'type_name': topic.type_name})
            for endpoint in topic.subscribers:
                if endpoint.node_name == node_name:
                    subs.append({'topic_name': topic.topic_name,
                                 'type_name': topic.type_name})
    return pubs, subs


# Prefixes/namespaces for Hermos-internal nodes. These are implementation
# details (not application nodes), so list verbs hide them by default behind
# ``-d/--debug``, same as the domain bridge nodes.
BRIDGE_NODE_PREFIX = '/hermos_bridge_node_'
DISCOVERY_AGENT_NODE_PREFIX = '/hermos_discovery_agent_'
# No trailing slash: also matches the per-domain helper nodes
# (``/hermos_cie_thread_configurator_domain_<id>``) created by
# ``create_node_for_domain`` in util.cpp, not just the per-process client
# nodes under the namespace.
CIE_THREAD_CONFIGURATOR_NODE_PREFIX = '/hermos_cie_thread_configurator'
# Trailing slash kept for the topic filter, which only needs to match
# topics published under the namespace.
CIE_THREAD_CONFIGURATOR_NAMESPACE = CIE_THREAD_CONFIGURATOR_NODE_PREFIX + '/'

_INTERNAL_NODE_PREFIXES = (
    BRIDGE_NODE_PREFIX,
    DISCOVERY_AGENT_NODE_PREFIX,
    CIE_THREAD_CONFIGURATOR_NODE_PREFIX,
)


def is_internal_node_name(node_name: str) -> bool:
    """True for Hermos-internal nodes (domain bridge, CIE thread configurator,
    discovery agent) that should be hidden unless ``-d/--debug`` is given."""
    name = node_name if node_name.startswith('/') else '/' + node_name
    return name.startswith(_INTERNAL_NODE_PREFIXES)


# Bridge-label states for the CLI verbs. Wording is shared so list/info stay
# consistent.
BRIDGE_ENABLED = 'enabled'
BRIDGE_BRIDGED = 'bridged'
BRIDGE_WARN = 'warn'
BRIDGE_LABEL_TEXT = {
    BRIDGE_ENABLED: 'Hermos enabled',
    BRIDGE_BRIDGED: 'Hermos enabled, bridged',
    BRIDGE_WARN: 'WARN: one or more necessary bridges are not running',
}


def collect_bridge_roles(
    snapshots: list[HermosDaemonState],
) -> dict[str, list[tuple[bool, bool, bool, bool]]]:
    """Map ``topic_name -> per-source bridge roles`` in one pass over snapshots.

    Each value is a list with one entry per namespace/device that has a real (non-bridge)
    endpoint on that topic: ``(real_pub, real_sub, inbound_bridge,
    outbound_bridge)``. A bridge endpoint is directional: an inbound bridge
    (ROS 2 -> Hermos) is a bridge publisher; an outbound bridge
    (Hermos -> ROS 2) is a bridge subscriber.

    Building this once lets the verbs label every topic with an O(1) lookup
    instead of re-scanning the snapshots per topic.
    """
    roles = {}
    for snap in snapshots:
        for topic in snap.topics:
            real_pub = any(not ep.is_bridge for ep in topic.publishers)
            real_sub = any(not ep.is_bridge for ep in topic.subscribers)
            if not (real_pub or real_sub):
                continue
            inbound_bridge = any(ep.is_bridge for ep in topic.publishers)
            outbound_bridge = any(ep.is_bridge for ep in topic.subscribers)
            roles.setdefault(topic.topic_name, []).append(
                (real_pub, real_sub, inbound_bridge, outbound_bridge))
    return roles


def bridge_label_from_roles(ns_roles: list, ros2_has_pub: bool, ros2_has_sub: bool) -> str:
    """Classify a topic's bridge status from its namespace/device source roles.

    Returns ``BRIDGE_ENABLED`` / ``BRIDGE_BRIDGED`` / ``BRIDGE_WARN``. A bridge
    is *expected* only where data must actually cross a boundary: a real
    publisher needs an outbound bridge only if a consumer exists elsewhere
    (another NS or ROS 2), and a real subscriber needs an inbound bridge only
    if a producer exists elsewhere. So WARN fires only when such a bridge is
    expected but missing (e.g. a talker and listener in different NSes with no
    bridges); BRIDGED means every expected bridge is present; ENABLED means no
    bridging is expected (a lone endpoint, or producer/consumer on the same device
    in the same NS). Separate devices must not be treated as natively connected.

    ``ns_roles`` comes from :func:`collect_bridge_roles` (empty if the topic has
    no real Hermos endpoint). ``ros2_has_pub`` / ``ros2_has_sub`` are whether
    the topic has a ROS 2 (DDS) publisher / subscriber in the caller's NS — the
    only NS whose DDS view the CLI can see.
    """
    real_pub_nses = sum(1 for r in ns_roles if r[0])
    real_sub_nses = sum(1 for r in ns_roles if r[1])

    bridge_expected = False
    bridge_missing = False
    for real_pub, real_sub, inbound_bridge, outbound_bridge in ns_roles:
        # "elsewhere" excludes this NS — an intra-NS producer/consumer pair
        # talks directly without a bridge.
        consumer_elsewhere = ros2_has_sub or real_sub_nses - (1 if real_sub else 0) > 0
        producer_elsewhere = ros2_has_pub or real_pub_nses - (1 if real_pub else 0) > 0
        if real_pub and consumer_elsewhere:
            bridge_expected = True
            bridge_missing |= not outbound_bridge
        if real_sub and producer_elsewhere:
            bridge_expected = True
            bridge_missing |= not inbound_bridge

    if not bridge_expected:
        return BRIDGE_ENABLED
    return BRIDGE_WARN if bridge_missing else BRIDGE_BRIDGED
