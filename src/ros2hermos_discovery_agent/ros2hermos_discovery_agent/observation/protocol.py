"""The v2 metadata envelope and DDS contract shared by agent and CLI."""

import errno
import os
import re
import stat
import time

from rclpy.duration import Duration
from rclpy.qos import (
    DurabilityPolicy, HistoryPolicy, LivelinessPolicy, QoSProfile, ReliabilityPolicy,
)
from ros2hermos_discovery_msgs.msg import HermosDaemonState


GOSSIP_TOPIC = '/_hermos_discovery_v2'
SCHEMA_VERSION = 2
LEASE_NS = 30_000_000_000
WRITER_BRIDGE = HermosDaemonState.WRITER_BRIDGE
WRITER_OBSERVER = HermosDaemonState.WRITER_OBSERVER
_WRITER_PRIORITY = {WRITER_BRIDGE: 2, WRITER_OBSERVER: 1}


def reader_qos(depth=64):
    """Receive metadata without comparing a remote source stamp to our wall clock.

    Freshness after receipt is bounded by a local monotonic lease. AUTOMATIC
    liveliness also works on Humble, which lacks publisher assert_liveliness.
    """
    return QoSProfile(
        reliability=ReliabilityPolicy.RELIABLE,
        durability=DurabilityPolicy.TRANSIENT_LOCAL,
        history=HistoryPolicy.KEEP_LAST, depth=depth,
        liveliness=LivelinessPolicy.AUTOMATIC,
        liveliness_lease_duration=Duration(nanoseconds=LEASE_NS))


def writer_qos(depth=1):
    """Expire retained samples on the writer's clock before late-reader delivery."""
    qos = reader_qos(depth)
    qos.lifespan = Duration(nanoseconds=LEASE_NS)
    return qos


def validate_snapshot(snapshot):
    """Reject unknown wire schemas and invalid per-domain freshness envelopes."""
    if snapshot.schema_version != SCHEMA_VERSION:
        raise ValueError(f'unsupported HERM0S metadata schema {snapshot.schema_version}')
    if getattr(snapshot, 'writer_role', 0) not in _WRITER_PRIORITY:
        raise ValueError('unsupported HERM0S metadata writer role')
    if not re.fullmatch(r'(0|[1-9][0-9]*):(0|[1-9][0-9]*)', getattr(snapshot, 'device_id', '')):
        raise ValueError('HERM0S metadata requires a canonical major:minor device identity')
    if snapshot.lease_duration_ns != LEASE_NS:
        raise ValueError('HERM0S metadata v2 freshness lease must be 30 seconds')
    if any(topic.domain_id != snapshot.domain_id for topic in snapshot.topics):
        raise ValueError('HERM0S metadata topic and snapshot domain disagree')


def scope_key(snapshot):
    """Return the complete identity, including an unknown session for local queries."""
    return (*route_key(snapshot), snapshot.session_id)


def route_key(snapshot):
    """Identify one host/namespace/domain/device inventory."""
    return (snapshot.host_uuid, snapshot.ipc_ns_inode, snapshot.domain_id, snapshot.device_id)


def local_device_id():
    """Identify the selected character device, following aliases; raise on failure.

    Never publish an unknown identity that could merge unrelated inventories.
    The number is meaningful only alongside the source host's boot UUID.
    """
    path = os.environ.get('HERMOS_DEVICE_PATH') or '/dev/hermos'
    device = os.stat(path)
    if not stat.S_ISCHR(device.st_mode):
        raise OSError(errno.ENODEV, 'HERM0S device is not a character device', path)
    return f'{os.major(device.st_rdev)}:{os.minor(device.st_rdev)}'


def latest_per_route(received):
    """Prefer bridge writers, then the latest receipt within the chosen role.

    Local query lists may omit receipt times when a route has only one writer
    of the preferred role. Selecting among sessions of that role requires them.
    """
    preferred = {}
    for snapshot, received_at in received:
        key = route_key(snapshot)
        if key not in preferred:
            preferred[key] = [(snapshot, received_at)]
            continue
        priority = _WRITER_PRIORITY[snapshot.writer_role]
        previous_priority = _WRITER_PRIORITY[preferred[key][0][0].writer_role]
        if priority == previous_priority:
            preferred[key].append((snapshot, received_at))
        elif priority > previous_priority:
            preferred[key] = [(snapshot, received_at)]
    latest = {}
    for key, candidates in preferred.items():
        if len(candidates) == 1:
            latest[key] = candidates[0][0]
        else:
            if any(stamp is None for _, stamp in candidates):
                raise ValueError('local receipt times are required to select among HERM0S sessions')
            latest[key] = max(reversed(candidates), key=lambda pair: pair[1])[0]
    return latest


def make_snapshot(*, host_uuid, hostname, ipc_ns_inode, domain_id, device_id, session_id,
                  topics, writer_role, hermos_version=''):
    """Build one v2 envelope; local queries may leave session/version unknown."""
    return HermosDaemonState(
        schema_version=SCHEMA_VERSION, writer_role=writer_role, hermos_version=hermos_version,
        host_uuid=host_uuid, host_hostname=hostname, ipc_ns_inode=ipc_ns_inode,
        domain_id=domain_id, device_id=device_id, session_id=session_id, snapshot_time_ns=time.time_ns(),
        lease_duration_ns=LEASE_NS, topics=topics)
