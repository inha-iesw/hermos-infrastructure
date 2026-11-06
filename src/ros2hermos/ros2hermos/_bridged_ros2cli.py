import contextlib
import os
import time
from typing import Callable

import rclpy
from ros2cli.node.strategy import NodeStrategy
from ros2hermos.bridge_utils import create_dummy_subscription, load_msg_class
from ros2hermos.discovery import (
    DEFAULT_COLLECT_TIMEOUT_SEC,
    add_gossip_timeout_arg,
    collect_announcements_with_fallback,
    warn_if_gossip_timeout_overridden,
    warn_if_using_fallback,
)
from ros2hermos.observation.scope import current_domain, preferred_snapshots

DEFAULT_BRIDGE_TIMEOUT = 10.0   # seconds
POLL_INTERVAL = 0.2             # seconds between publisher-count checks

_HERMOS_BRIDGE_NOTE = (
    '\n\nnote:\n'
    '  A dummy ros2 subscriber is first created to trigger the A2R bridge, '
    'then the standard ros2 topic command begins.'
)


def add_bridge_arguments(parser) -> None:
    """Add --bridge-timeout argument and append an Hermos note to parser.description."""
    if parser.description:
        parser.description += _HERMOS_BRIDGE_NOTE
    else:
        parser.description = _HERMOS_BRIDGE_NOTE.lstrip()
    parser.add_argument(
        '--bridge-timeout',
        dest='bridge_timeout', type=float, default=DEFAULT_BRIDGE_TIMEOUT,
        metavar='SEC',
        help='Seconds to wait for the Hermos bridge (ROS 2 publisher) to appear '
             '(default: %.1f). '
             'If no publisher appears within this time, the topic is assumed to not '
             'exist in Hermos.' % DEFAULT_BRIDGE_TIMEOUT)
    add_gossip_timeout_arg(parser)


def wait_for_ros2_publisher(node, topic_name: str, bridge_timeout: float, *, context=None) -> bool:
    """Poll until a ROS 2 publisher appears on topic_name.

    Returns True if a publisher is detected, False on timeout.
    """
    deadline = time.monotonic() + bridge_timeout
    while rclpy.ok(context=context) and time.monotonic() < deadline:
        if node.get_publishers_info_by_topic(topic_name):
            return True
        time.sleep(POLL_INTERVAL)
    return False


def resolve_type_name(topic_name: str, gossip_timeout: float):
    """Resolve the ROS type string for topic_name.

    First checks the live ROS 2 graph for the topic type.  If the topic is not
    found there, falls back to /_hermos_discovery_v2 gossip to resolve the type
    for Hermos-only topics.

    Returns the type string on success, or None if the type name cannot be
    resolved (error is printed to stdout).
    """
    with NodeStrategy(None) as proxy_node:
        # First, check if the topic already exists in the ROS 2 graph.
        type_names = {type_name
                      for name, types in proxy_node.get_topic_names_and_types()
                      if name == topic_name for type_name in types if type_name}

        if not type_names:
            # If not found, fall back to /_hermos_discovery_v2 to look up the Hermos topic.
            snapshots, used_fallback = collect_announcements_with_fallback(proxy_node, timeout_sec=gossip_timeout)
            warn_if_using_fallback(proxy_node, used_fallback, gossip_timeout, snapshots)
            selected = preferred_snapshots(snapshots, domain_id=current_domain(proxy_node))
            type_names = {topic.type_name
                          for snap in selected for topic in snap.topics
                          if topic.topic_name == topic_name and topic.type_name}

    if len(type_names) > 1:
        print(f"ERROR: Conflicting message types for '{topic_name}': "
              f"{', '.join(sorted(type_names))}. Specify a message type explicitly.")
        return None
    if not type_names:
        print(
            "ERROR: Could not resolve message type for '%s'. "
            'The topic was not found in the ROS 2 graph or via /_hermos_discovery_v2. '
            'Make sure the topic exists and, for Hermos topics, the discovery agent is running.'
            % topic_name)
        return None

    return next(iter(type_names))


@contextlib.contextmanager
def _create_dummy_subscription(topic_name: str, msg_type):
    """Context manager that registers a dummy ROS 2 subscriber to trigger the A2R bridge.

    The subscription only needs to exist; no spinning is required to keep it alive.
    Uses a private rclpy context so the default context remains free for action_fn.

    Yields (node, ctx).
    """
    ctx = rclpy.Context()
    rclpy.init(context=ctx)
    node = None
    try:
        node = rclpy.create_node(
            '_ros2hermos_cli_%d' % os.getpid(),
            context=ctx)
        create_dummy_subscription(node, topic_name, msg_type)
        yield node, ctx
    finally:
        if node is not None:
            try:
                node.destroy_node()
            except Exception:
                pass
        rclpy.try_shutdown(context=ctx)


def spawn_bridge_and_run(
        args, topic_name: str, action_fn: Callable, *, message_type: str | None = None) -> int:
    """Resolve the message type for topic_name, trigger the A2R bridge, and run action_fn(args).

    message_type: if provided, skip discovery and use this type string directly.
    Returns the exit code from action_fn, or 1 on failure.
    """
    warn_if_gossip_timeout_overridden(args)
    print(
        "[*] Triggering A2R bridge for '%s'. "
        'This may take a few seconds...' % topic_name)

    # Phase 1: resolve type name.
    # If the caller already supplied a type string (e.g. ros2 topic echo_hermos <topic> <type>),
    # use it directly; otherwise look it up via the ROS 2 graph or /_hermos_discovery_v2 gossip.
    if message_type is not None:
        type_name = message_type
    else:
        type_name = resolve_type_name(topic_name, args.gossip_timeout)
        if type_name is None:
            return 1

    # Phase 2: load message class from type name.
    msg_type = load_msg_class(type_name)
    if msg_type is None:
        return 1

    # Phase 3: start dummy subscription and wait for a ROS 2 publisher to appear.
    # TODO: Ideally, we should wait until bridges have come up for all Hermos publishers
    #       across every namespace.  For simplicity, we currently only wait for a single
    #       publisher to appear.
    with _create_dummy_subscription(topic_name, msg_type) as (node, ctx):
        ready = wait_for_ros2_publisher(
            node, topic_name, args.bridge_timeout, context=ctx)

        if not ready:
            print(
                "ERROR: No ROS 2 publisher appeared on '%s' within %.1f s. "
                'The topic may not exist in Hermos or the bridge failed to start.'
                % (topic_name, args.bridge_timeout))
            return 1

        print('[*] Starting ros2cli command...')

        # The dummy subscription stays alive (via the with block) while action_fn runs.
        return action_fn(args)
