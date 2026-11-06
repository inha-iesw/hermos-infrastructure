from ros2cli.node.strategy import NodeStrategy
from ros2topic.api import get_topic_names_and_types
from ros2topic.verb import VerbExtension

from ros2hermos.discovery import (
    add_gossip_timeout_arg,
    all_topic_names,
    bridge_label_from_roles,
    BRIDGE_LABEL_TEXT,
    CIE_THREAD_CONFIGURATOR_NAMESPACE,
    warn_if_gossip_timeout_overridden,
)
from ros2hermos.observation.cli import (
    add_scope_arguments, bridge_roles_for_domain, collect_for_args, scopes_for_args,
)
from ros2hermos.observation.scope import scope_label

class ListHermosVerb(VerbExtension):
    "Output a list of available topics including Hermos"

    def add_arguments(self, parser, cli_name):
        parser.add_argument(
            '-d', '--debug', action='store_true',
            help='Include internal topics (CIE thread configurator) in the output')
        add_gossip_timeout_arg(parser)
        add_scope_arguments(parser)

    def main(self, *, args):
        warn_if_gossip_timeout_overridden(args)
        with NodeStrategy(None) as node:
            try:
                collected = collect_for_args(node, args)
                scopes = scopes_for_args(collected, node, args)
                roles = bridge_roles_for_domain(collected, node)
            except ValueError as exc:
                return str(exc)
            snapshots = [state for _, state in scopes]

            def divide_ros2_topic_into_pubsub(topic_names):
                pub_topics = []
                sub_topics = []
                for name in topic_names:
                    pubs_info = node.get_publishers_info_by_topic(name)
                    subs_info = node.get_subscriptions_info_by_topic(name)

                    # Remove Hermos bridge nodes from the list
                    pubs_info = [info for info in pubs_info if not info.node_name.startswith("hermos_bridge_node_")]
                    subs_info = [info for info in subs_info if not info.node_name.startswith("hermos_bridge_node_")]

                    if pubs_info:
                        pub_topics.append(name)
                    if subs_info:
                        sub_topics.append(name)
                return pub_topics, sub_topics

            def remove_service_topic(topic_names):
                return [name for name in topic_names if not name.startswith('/HERMOS_SRV_')]

            # Get Hermos topics from gossip (every NS / ECU on the domain).
            hermos_topics = remove_service_topic(list(all_topic_names(snapshots)))

            # Get ros2 topics
            ros2_topics_data = get_topic_names_and_types(node=node)
            ros2_all_topics = set(name for name, _ in ros2_topics_data)

            ########################################################################
            # Print topic list
            ########################################################################
            hermos_topics_set = set(hermos_topics)

            # Non-hermos ROS2 topics cannot have bridge nodes, so no filtering needed.
            ros2_only_topics = ros2_all_topics - hermos_topics_set
            # Only query pub/sub breakdown for topics in both sets (expensive ROS2 API calls).
            overlapping_candidates = list(hermos_topics_set & ros2_all_topics)
            ros2_pub_topics, ros2_sub_topics = divide_ros2_topic_into_pubsub(overlapping_candidates)
            ros2_pub_set = set(ros2_pub_topics)
            ros2_sub_set = set(ros2_sub_topics)
            ros2_topics_set = ros2_only_topics | ros2_pub_set | ros2_sub_set

            merged_topics = hermos_topics_set | ros2_topics_set
            if not args.debug:
                merged_topics = {
                    topic for topic in merged_topics
                    if not topic.startswith(CIE_THREAD_CONFIGURATOR_NAMESPACE)
                }

            for topic in sorted(merged_topics):
                if topic in hermos_topics_set:
                    for scope, state in scopes:
                        if topic in all_topic_names([state]):
                            status = bridge_label_from_roles(
                                roles.get(topic, []), topic in ros2_pub_set, topic in ros2_sub_set)
                            suffix = f" ({BRIDGE_LABEL_TEXT[status]})"
                            print(f"{topic}{suffix} [{scope_label(scope)}]")
                else:
                    print(topic)
