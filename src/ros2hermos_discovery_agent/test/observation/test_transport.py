"""Host DDS checks for the metadata channel; the native source is controlled."""

import time
from unittest.mock import Mock

from rclpy.context import Context
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node

from ros2hermos_discovery_agent import agent
from ros2hermos_discovery_msgs.msg import HermosDaemonState


def test_observation_node_waits_for_dispatch_and_late_join_receives_snapshot(monkeypatch):
    registry = Mock()
    source = Mock(return_value=[])
    monkeypatch.setattr(agent, 'load_query_library', lambda **kwargs: object())
    monkeypatch.setattr(agent, 'read_local_topics', source)
    monkeypatch.setattr(agent, 'local_device_id', lambda: '240:0')
    monkeypatch.setenv('ROS_DOMAIN_ID', '9')
    context = Context()
    context.init(args=[], domain_id=172)
    observer = listener = executor = None
    try:
        observer = agent.DiscoveryAgent(
            registry=registry, context=context, observation_only=True, session_id='late-join')
        assert observer.context is context
        assert observer._sub is None
        assert observer._domain_id == 172
        source.assert_not_called()
        registry.rebuild.assert_not_called()
        # Emulate the supervisor ACK boundary by explicitly beginning dispatch.
        published = observer.publish_snapshot()
        listener = Node('metadata_late_reader', context=context)
        received = []
        listener.create_subscription(
            HermosDaemonState, agent.GOSSIP_TOPIC, received.append, agent.reader_qos())
        executor = SingleThreadedExecutor(context=context)
        executor.add_node(listener)
        deadline = time.monotonic() + 5.0
        while not received and time.monotonic() < deadline:
            executor.spin_once(timeout_sec=0.05)
        assert received, 'late metadata reader did not receive the retained snapshot'
        message = received[0]
        assert (message.domain_id, message.session_id) == (172, 'late-join')
        assert message.snapshot_time_ns == published.snapshot_time_ns
        assert message.device_id == '240:0'
        assert message.topics == []
        assert observer._remote_states == {}
    finally:
        if executor is not None:
            executor.shutdown()
        if listener is not None:
            listener.destroy_node()
        if observer is not None:
            observer.destroy_node()
        context.shutdown()
