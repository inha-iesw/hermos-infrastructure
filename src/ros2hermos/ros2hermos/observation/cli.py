"""Shared scope selectors for metadata-only CLI verbs."""

from ros2hermos.observation.scope import (
    current_domain, preferred_snapshots, scoped_snapshots, select_scope,
)


def add_scope_arguments(parser):
    """Allow an explicit choice when several native sessions share a name."""
    parser.add_argument('--local', action='store_true',
                        help='Query only this IPC namespace without a metadata subscription')
    parser.add_argument('--session-id', help='Select a HERM0S supervisor/agent session')
    parser.add_argument('--ipc-namespace', type=int, help='Select an IPC namespace inode')
    parser.add_argument('--host-id', help='Select the source host boot UUID')
    parser.add_argument('--device-id', help='Select a source character device (major:minor)')


def collect_for_args(node, args, *, require_native=False):
    """Collect metadata or explicitly use only the local read-only query."""
    from ros2hermos.discovery import (
        collect_announcements_with_fallback, local_metadata_error, warn_if_using_fallback,
    )
    timeout = 0.0 if getattr(args, 'local', False) else args.gossip_timeout
    strict = require_native or getattr(args, 'local', False)
    try:
        snapshots, fallback = collect_announcements_with_fallback(node, timeout, strict=strict)
    except OSError as exc:
        raise ValueError(local_metadata_error(exc)) from exc
    if strict and fallback and not snapshots:
        raise ValueError('Local HERM0S metadata unavailable: ioctl wrapper missing or incompatible')
    warn_if_using_fallback(node, fallback, timeout, snapshots)
    return snapshots


def bridge_roles_for_domain(snapshots, node):
    """Diagnose bridges across namespaces, independent of display selectors.

    Bridge writers take precedence; otherwise the newest observer is used.
    Explicit display selectors do not remove other routes from bridge diagnosis.
    """
    from ros2hermos.discovery import collect_bridge_roles
    return collect_bridge_roles(preferred_snapshots(snapshots, domain_id=current_domain(node)))


def scopes_for_args(snapshots, node, args, *, topic_name=None, node_name=None):
    """Filter within the CLI context domain while retaining each source scope."""
    domain_id = current_domain(node)
    if getattr(args, 'session_id', None) is None:
        snapshots = preferred_snapshots(snapshots, domain_id=domain_id)
    scopes = scoped_snapshots(
        snapshots, domain_id=domain_id,
        session_id=getattr(args, 'session_id', None),
        ipc_ns_inode=getattr(args, 'ipc_namespace', None))
    host_id = getattr(args, 'host_id', None)
    if host_id is not None:
        scopes = [(scope, state) for scope, state in scopes if scope.host_uuid == host_id]
    device_id = getattr(args, 'device_id', None)
    if device_id is not None:
        scopes = [(scope, state) for scope, state in scopes if scope.device_id == device_id]
    if topic_name is not None:
        scopes = [(scope, state) for scope, state in scopes
                  if any(topic.topic_name == topic_name for topic in state.topics)]
    if node_name is not None:
        scopes = [(scope, state) for scope, state in scopes
                  if any(endpoint.node_name == node_name for topic in state.topics
                         for endpoint in [*topic.publishers, *topic.subscribers])]
    return scopes


def selected_snapshots(snapshots, node, args, **filters):
    """Return one native source for an info query, or no native match."""
    selected = select_scope(scopes_for_args(snapshots, node, args, **filters))
    if selected is None and any(getattr(args, name, None) is not None
                                for name in ('session_id', 'ipc_namespace', 'host_id', 'device_id')):
        raise ValueError('no HERM0S scope matches selectors')
    return (selected[0], [selected[1]]) if selected else (None, [])
