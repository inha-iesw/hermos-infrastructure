"""Read-only local metadata using the discovery agent's domain-aware reader."""

import os
from typing import Optional
import uuid

from ros2hermos_discovery_agent.agent import load_query_library, read_local_topics
from ros2hermos_discovery_agent.type_registry import TypeRegistryReader, _default_base_dir
from ros2hermos_discovery_msgs.msg import HermosDaemonState
from ros2hermos_discovery_agent.observation.protocol import local_device_id, make_snapshot, WRITER_OBSERVER

from ros2hermos.observation.scope import current_domain


BOOT_ID_PATH = '/proc/sys/kernel/random/boot_id'
SELF_IPC_NS_PATH = '/proc/self/ns/ipc'


def _load_lib():
    """Load the shared read-only query bindings, or report unavailable ABI."""
    try:
        return load_query_library(registration=False)
    except (OSError, AttributeError):
        return None


def _local_host_uuid() -> str:
    try:
        with open(BOOT_ID_PATH) as fp:
            return str(uuid.UUID(fp.read().strip()))
    except (OSError, ValueError):
        return ''


def _local_ipc_ns_inode() -> int:
    return os.stat(SELF_IPC_NS_PATH).st_ino


def self_ns_snapshot(domain_id=None) -> Optional[HermosDaemonState]:
    """Query one domain in this IPC namespace without registering a peer.

    An absent wrapper returns None. A standalone local query cannot identify a
    supervisor epoch and leaves session_id unknown unless supplied by runtime.
    Registry rebuilding is read-only; cleanup belongs to the discovery agent.
    """
    lib = _load_lib()
    if lib is None:
        return None
    domain_id = current_domain() if domain_id is None else domain_id
    ipc_ns_inode = _local_ipc_ns_inode()
    registry = TypeRegistryReader(ipc_ns_inode, base_dir=_default_base_dir())
    registry.rebuild()
    topics = read_local_topics(lib, registry=registry, own_domain_id=domain_id)
    return make_snapshot(
        host_uuid=_local_host_uuid(), hostname=os.uname().nodename,
        ipc_ns_inode=ipc_ns_inode, domain_id=domain_id,
        device_id=local_device_id(),
        session_id=os.environ.get('HERMOS_SESSION_ID', ''),
        writer_role=WRITER_OBSERVER,
        topics=topics)
