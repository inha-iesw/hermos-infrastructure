"""Expose native counters without subscribing to a payload topic."""

from ros2cli.node.strategy import NodeStrategy
from ros2topic.verb import VerbExtension

from ros2hermos.discovery import add_gossip_timeout_arg, warn_if_gossip_timeout_overridden
from ros2hermos.observation.cli import add_scope_arguments, collect_for_args, scopes_for_args
from ros2hermos.observation.scope import scope_label
from ros2hermos_discovery_agent.observation.snapshot import COUNTERS


def statistics_lines(endpoint, *, is_publisher):
    """Format only supported counters with their native kernel meaning."""
    status = getattr(endpoint, 'statistics_status', '') or 'unsupported'
    pid = endpoint.pid if endpoint.pid > 0 else 'unknown'
    endpoint_id = getattr(endpoint, 'endpoint_id', 'unknown') if status == 'available' else 'unknown'
    kind = 'publisher' if is_publisher else 'subscriber'
    yield f'{kind} node={endpoint.node_name} pid={pid} endpoint={endpoint_id}'
    yield f'  statistics: {status}'
    if status != 'available':
        return
    yield f'  sample_time_ns (source CLOCK_MONOTONIC): {endpoint.sample_time_ns}'
    retained = 'publisher resident entries' if is_publisher else 'held kernel references'
    meanings = {
        'received_total': 'kernel reference acquisitions',
        'dropped_history_total': 'unread history loss',
        'queued_entries': 'unread resident entries before QoS filtering',
        'retained_entries': retained,
    }
    for bit, name in COUNTERS:
        meaning = meanings.get(name, '')
        label = f'{name} ({meaning})' if meaning else name
        value = getattr(endpoint, name) if endpoint.supported_fields & bit else 'unsupported'
        yield f'  {label}: {value}'


class StatsHermosVerb(VerbExtension):
    """Show cumulative native metadata counters, without a payload subscription."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument('topic_name', help='HERM0S topic to inspect')
        add_gossip_timeout_arg(parser)
        add_scope_arguments(parser)

    def main(self, *, args):
        warn_if_gossip_timeout_overridden(args)
        with NodeStrategy(None) as node:
            try:
                scopes = scopes_for_args(
                    collect_for_args(node, args, require_native=True),
                    node, args, topic_name=args.topic_name)
            except ValueError as exc:
                return str(exc)
            if not scopes:
                return f'No HERM0S metadata for topic: {args.topic_name}'
            for scope, state in scopes:
                print(f'HERM0S {scope_label(scope)}')
                timestamp = getattr(state, 'snapshot_time_ns', 0) or 'unknown'
                print(f'snapshot_time_ns (source wall clock): {timestamp}')
                for topic in state.topics:
                    if topic.topic_name != args.topic_name:
                        continue
                    print(f'Topic: {topic.topic_name}  Type: {topic.type_name or "<UNKNOWN>"}')
                    for is_publisher, endpoints in ((True, topic.publishers), (False, topic.subscribers)):
                        for endpoint in endpoints:
                            print('\n'.join(statistics_lines(endpoint, is_publisher=is_publisher)))
