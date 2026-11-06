"""Preserve native endpoint identity and received metadata freshness."""

import copy
from dataclasses import dataclass
import os

from ros2hermos_discovery_agent.observation.protocol import (
    latest_per_route, scope_key, validate_snapshot,
)


@dataclass(frozen=True, order=True)
class Scope:
    """Identity of one observed domain within a host IPC session."""

    host_uuid: str
    ipc_ns_inode: int
    domain_id: int
    device_id: str
    session_id: str


def current_domain(node=None):
    """Read the actual CLI context's domain, falling back to ROS_DOMAIN_ID."""
    direct = getattr(node, 'direct_node', node)
    direct = getattr(direct, 'node', direct)
    context = getattr(direct, 'context', None)
    if context is not None:
        domain = context.get_domain_id()
        if isinstance(domain, int):
            return domain
    raw = os.environ.get('ROS_DOMAIN_ID') or '0'
    try:
        domain = int(raw)
    except ValueError as exc:
        raise ValueError('ROS_DOMAIN_ID must be an integer') from exc
    if not 0 <= domain <= 0xffffffff:
        raise ValueError('ROS_DOMAIN_ID is outside the uint32 range')
    return domain


def scoped_snapshots(snapshots, *, domain_id, session_id=None, ipc_ns_inode=None):
    """Return scoped snapshots without collapsing equal names across sessions.

    The returned topic lists never mutate the received metadata.
    """
    result = []
    for snapshot in snapshots:
        validate_snapshot(snapshot)
        session = snapshot.session_id
        if session_id is not None and session != session_id:
            continue
        if ipc_ns_inode is not None and snapshot.ipc_ns_inode != ipc_ns_inode:
            continue
        if snapshot.domain_id != domain_id:
            continue
        topics = [topic for topic in snapshot.topics if topic.domain_id == domain_id]
        filtered = copy.copy(snapshot)
        filtered.topics = topics
        scope = Scope(*scope_key(snapshot))
        result.append((scope, filtered))
    return sorted(result, key=lambda item: item[0])


def preferred_snapshots(snapshots, *, domain_id):
    """Choose one writer per route before filtering its topic/node inventory."""
    received = (snapshots.received() if isinstance(snapshots, ReceivedSnapshots)
                else ((snapshot, None) for snapshot in snapshots))
    matching = []
    for snapshot, received_at in received:
        validate_snapshot(snapshot)
        if snapshot.domain_id == domain_id:
            matching.append((snapshot, received_at))
    return list(latest_per_route(matching).values())


def scope_label(scope):
    """Format a scope without inventing missing host or session identity."""
    return (f'host={scope.host_uuid or "unknown"} ipc={scope.ipc_ns_inode} '
            f'domain={scope.domain_id} device={scope.device_id} '
            f'session={scope.session_id or "unknown"}')


def select_scope(scoped):
    """Select an unambiguous scope for info queries, or report the choices."""
    if not scoped:
        return None
    if len(scoped) != 1:
        choices = '; '.join(scope_label(scope) for scope, _ in scoped)
        raise ValueError('multiple HERM0S scopes match; select --session-id, '
                         f'--ipc-namespace, --device-id and/or --host-id: {choices}')
    return scoped[0]


class ReceivedSnapshots(list):
    """Snapshot list retaining local receipt times when display order changes."""

    def __init__(self, received):
        received = list(received)
        super().__init__(snapshot for snapshot, _ in received)
        self._received_at = {scope_key(snapshot): stamp for snapshot, stamp in received}

    def received(self):
        """Yield each present snapshot with its original local receipt time."""
        return ((snapshot, self._received_at[scope_key(snapshot)]) for snapshot in self)


class SnapshotCache:
    """Latest metadata per scope, bounded by local receipt age and DDS writers.

    Remote timestamps are informational. Datawriter lifespan expires retained
    samples before receipt; this cache expires already received samples without
    assuming clocks on different hosts agree.
    """

    def __init__(self):
        self._snapshots = {}
        self._conflicts = {}

    def update(self, snapshot, *, received_at, publisher_gid=None):
        """Accept a supported snapshot and replace only its own scope."""
        validate_snapshot(snapshot)
        key = scope_key(snapshot)
        lease = snapshot.lease_duration_ns / 1e9
        if self._conflicts.get(key, 0) > received_at:
            raise ValueError('duplicate HERM0S metadata agents describe the same scope')
        previous = self._snapshots.get(key)
        if (previous is not None and previous[1] > received_at and
                publisher_gid is not None and previous[2] is not None and
                publisher_gid != previous[2]):
            self._conflicts[key] = max(previous[1], received_at + lease)
            del self._snapshots[key]
            raise ValueError('duplicate HERM0S metadata agents describe the same scope')
        # Preserve receipt order for consumers choosing the current session.
        self._snapshots.pop(key, None)
        self._snapshots[key] = (snapshot, received_at + lease, publisher_gid, received_at)

    def active(self, *, now, live_gids=None):
        """Return only unexpired snapshots whose known writer is still visible."""
        expired = [key for key, (_, deadline, gid, _) in self._snapshots.items()
                   if now >= deadline or
                   (gid is not None and live_gids is not None and gid not in live_gids)]
        for key in expired:
            del self._snapshots[key]
        return ReceivedSnapshots((entry[0], entry[3]) for entry in self._snapshots.values())
