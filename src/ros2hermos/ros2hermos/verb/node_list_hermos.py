import sys

from ros2cli.node.strategy import add_arguments
from ros2cli.node.strategy import NodeStrategy
from ros2node.api import get_node_names
from ros2topic.verb import VerbExtension

from ros2hermos.discovery import (
    add_gossip_timeout_arg,
    all_nodes,
    is_internal_node_name,
    warn_if_gossip_timeout_overridden,
)
from ros2hermos.observation.cli import add_scope_arguments, collect_for_args, scopes_for_args
from ros2hermos.observation.scope import scope_label

def split_fqn(fqn):
    namespace, _, name = fqn.rpartition('/')
    return (namespace or '/'), name

class ListHermosVerb(VerbExtension):
    "Output a list of available nodes including Hermos::Node"

    def add_arguments(self, parser, cli_name):
        add_arguments(parser)
        parser.add_argument(
            '-a', '--all', action='store_true',
            help='Display all nodes even hidden ones')
        parser.add_argument(
            '-c', '--count-nodes', action='store_true',
            help='Only display the number of nodes discovered')
        parser.add_argument(
            '-d', '--debug', action='store_true',
            help='Include internal nodes (domain bridge, CIE thread configurator, '
                 'discovery agent) in the output')
        add_gossip_timeout_arg(parser)
        add_scope_arguments(parser)

    def main(self, *, args):
        warn_if_gossip_timeout_overridden(args)
        with NodeStrategy(None) as node:
            try:
                scopes = scopes_for_args(collect_for_args(node, args), node, args)
            except ValueError as exc:
                return str(exc)
            snapshots = [state for _, state in scopes]

            # Get node names which contains Hermos topics from gossip.
            hermos_node_name = all_nodes(snapshots)

            # TODO(bdm-k): The current impl determines shadow nodes in a heuristic way. We need to
            # invent a deterministic way to identify shadow nodes.

            def likely_shadow_node(fqn):
                if fqn not in hermos_node_name:
                    return False

                ns, name = split_fqn(fqn)
                try:
                    # A normal rclcpp node is likely to have parameter services, so start by testing
                    # services.
                    if (
                        len(node.get_service_names_and_types_by_node(name, ns)) != 0
                        or len(node.get_publisher_names_and_types_by_node(name, ns)) != 0
                        or len(node.get_subscriber_names_and_types_by_node(name, ns)) != 0
                        or len(node.get_client_names_and_types_by_node(name, ns)) != 0
                    ):
                        return False
                    return True
                except Exception:
                    return False

            # Get ros2 node names.
            # The RMW layer can raise (e.g. "empty node name returned by the RMW
            # layer" when a participant without a valid node name is present in the
            # DDS graph). Don't let that abort the whole command: fall back to the
            # Hermos nodes gathered from gossip so the output is still useful.
            try:
                ros2_node_name_list = get_node_names(node=node, include_hidden_nodes=args.all)
            except Exception as e:
                print(
                    f'WARNING: failed to enumerate ROS 2 nodes ({e}); showing '
                    'Hermos nodes only. A participant with an empty node name may '
                    'be present in the DDS graph; try '
                    '`ros2 daemon stop && ros2 daemon start`.',
                    file=sys.stderr)
                ros2_node_name_list = []
            # Exclude shadow nodes so that the corresponding Hermos nodes are listed with "(Hermos enabled)"
            ros2_node_name = {n.full_name for n in ros2_node_name_list if not likely_shadow_node(n.full_name)}

            ########################################################################
            # Print node list
            ########################################################################
            merged_node_name = hermos_node_name | ros2_node_name
            if not args.all and not args.debug:
                merged_node_name = {node for node in merged_node_name if not is_internal_node_name(node)}
            rows = []
            for node_name in sorted(merged_node_name):
                if node_name in ros2_node_name and node_name not in hermos_node_name:
                    rows.append(node_name)
                for scope, state in scopes:
                    if node_name in all_nodes([state]):
                        label = 'Hermos enabled, DDS' if node_name in ros2_node_name else 'Hermos enabled'
                        rows.append(f'{node_name} ({label}) [{scope_label(scope)}]')
            if args.count_nodes:
                print(len(rows))
            else:
                for row in rows:
                    print(row)
