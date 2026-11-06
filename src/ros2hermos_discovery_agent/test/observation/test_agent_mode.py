"""Metadata observation cannot request payload bridges or dispatch before ACK."""

from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from ros2hermos_discovery_agent import agent


def test_observation_tick_publishes_snapshot_without_bridge_dispatch():
    state = SimpleNamespace(
        _registry=Mock(), _observation_only=True, _exit_when_idle=False,
        _prune_stale_remote_states=Mock(), publish_snapshot=Mock(),
        _dispatch_bridge_requests=Mock())
    agent.DiscoveryAgent._on_tick(state)
    state.publish_snapshot.assert_called_once_with()
    state._dispatch_bridge_requests.assert_not_called()


def test_gossip_lifespan_expires_retained_snapshot():
    qos = agent._gossip_qos()
    assert qos.lifespan.nanoseconds == int(agent.REMOTE_STATE_STALE_SEC * 1e9)


def test_agent_reader_does_not_expire_samples_using_remote_wall_clock(monkeypatch):
    monkeypatch.setattr(agent, 'load_query_library', lambda **kwargs: object())
    state = SimpleNamespace(
        _host_uuid='host', _host_hostname='test', _ipc_ns_inode=1, _domain_id=7,
        _observation_only=False, create_publisher=Mock(), create_subscription=Mock(),
        create_timer=Mock(), get_logger=Mock(), _on_remote_state=Mock(), _on_tick=Mock())
    agent.DiscoveryAgent._initialize(state, Mock(), False)
    writer_qos = state.create_publisher.call_args.args[-1]
    reader_qos = state.create_subscription.call_args.args[-1]
    assert writer_qos.lifespan.nanoseconds == 30_000_000_000
    assert reader_qos.lifespan.nanoseconds == 0


def test_failed_snapshot_skips_dispatch_and_idle_exit_then_recovers():
    state = SimpleNamespace(
        _registry=Mock(), _observation_only=False, _exit_when_idle=True,
        _prune_stale_remote_states=Mock(),
        publish_snapshot=Mock(side_effect=[OSError(105, 'query full'), 'fresh']),
        _dispatch_bridge_requests=Mock(), _maybe_exit_when_idle=Mock(),
        _idle_tracker=Mock(), get_logger=Mock())
    agent.DiscoveryAgent._on_tick(state)
    state._dispatch_bridge_requests.assert_not_called()
    state._maybe_exit_when_idle.assert_not_called()
    state._idle_tracker.reset.assert_called_once_with()
    agent.DiscoveryAgent._on_tick(state)
    state._dispatch_bridge_requests.assert_called_once_with('fresh')
    state._maybe_exit_when_idle.assert_called_once_with()


def test_bridge_dispatch_uses_latest_received_session_per_namespace(monkeypatch):
    from ros2hermos_discovery_msgs.msg import HermosEndpoint, HermosTopic

    old, fresh, local = _state(session='old'), _state(session='fresh'), _state()
    local.host_uuid, local.ipc_ns_inode = 'local', 1
    local.topics = [HermosTopic(topic_name='/x', domain_id=7, type_name='std_msgs/msg/Int32',
                               subscribers=[HermosEndpoint(node_name='/reader')])]
    old.topics = [HermosTopic(topic_name='/x', domain_id=7, type_name='std_msgs/msg/Int32',
                             publishers=[HermosEndpoint(node_name='/writer')])]
    # Deliberately put the newest arrival first: dictionary insertion and wall
    # timestamps are not substitutes for local monotonic receipt order.
    state = SimpleNamespace(_ipc_ns_inode=1, get_logger=Mock(), _remote_states={
        ('remote', 42, 7, 'fresh'): (fresh, 20.0),
        ('remote', 42, 7, 'old'): (old, 10.0)})
    dispatch = Mock()
    monkeypatch.setattr(agent.bridge_decider, 'dispatch_requests', dispatch)
    agent.DiscoveryAgent._dispatch_bridge_requests(state, local)
    dispatch.assert_not_called()
    assert len(state._remote_states) == 2


def test_failed_initialization_destroys_partial_ros_node(monkeypatch):
    monkeypatch.setattr(agent.Node, '__init__', Mock(return_value=None))
    destroy = Mock(return_value=True)
    monkeypatch.setattr(agent.Node, 'destroy_node', destroy)
    monkeypatch.setattr(agent, 'load_query_library', Mock(side_effect=OSError('missing wrapper')))
    with pytest.raises(OSError, match='missing wrapper'):
        agent.DiscoveryAgent(
            context=SimpleNamespace(get_domain_id=lambda: 7), observation_only=True)
    destroy.assert_called_once_with()


def test_standalone_and_supervised_observers_do_not_publish_the_same_scope(monkeypatch):
    monkeypatch.setenv('HERMOS_SESSION_ID', 'supervisor-epoch')
    monkeypatch.setattr(agent.Node, '__init__', Mock(return_value=None))
    monkeypatch.setattr(agent.DiscoveryAgent, '_initialize', Mock())
    context = SimpleNamespace(get_domain_id=lambda: 7)
    bridge = agent.DiscoveryAgent(context=context)
    standalone = agent.DiscoveryAgent(context=context, observation_only=True)
    supervised = agent.DiscoveryAgent(
        context=context, observation_only=True, session_id='supervisor-epoch')
    assert len({bridge._session_id, standalone._session_id, supervised._session_id}) == 3
    assert supervised._session_id == 'supervisor-epoch'


def test_observer_loads_queries_only_and_never_commits_idle_exit(monkeypatch):
    loader = Mock(return_value=object())
    monkeypatch.setattr(agent, 'load_query_library', loader)
    state = SimpleNamespace(
        _host_uuid='host', _host_hostname='test', _ipc_ns_inode=1, _domain_id=7,
        _observation_only=True, create_publisher=Mock(), create_subscription=Mock(),
        create_timer=Mock(), get_logger=Mock(), _on_remote_state=Mock(), _on_tick=Mock())
    agent.DiscoveryAgent._initialize(state, Mock(), True)
    loader.assert_called_once_with(registration=False)
    assert state._exit_when_idle is False
    state.create_subscription.assert_not_called()


@pytest.mark.parametrize('observation_only,role', [(False, 1), (True, 2)])
def test_snapshot_identifies_domain_session_and_measurement_time(monkeypatch, observation_only, role):
    state = SimpleNamespace(
        _hermos_version='test', _host_uuid='host', _host_hostname='host-name',
        _ipc_ns_inode=22, _domain_id=7, _session_id='epoch-2', _lib=object(),
        _observation_only=observation_only,
        _registry=object())
    monkeypatch.setattr(agent, 'read_local_topics', lambda *args: [])
    monkeypatch.setattr(agent, 'local_device_id', lambda: '240:0')
    message = agent.DiscoveryAgent.build_state(state)
    assert message.schema_version == 2
    assert message.writer_role == role
    assert message.device_id == '240:0'
    assert message.domain_id == 7
    assert message.session_id == 'epoch-2'
    assert message.snapshot_time_ns > 0
    assert message.lease_duration_ns == 30_000_000_000


def _state(domain=7, session='epoch-1', schema=2):
    from ros2hermos_discovery_msgs.msg import HermosDaemonState

    return HermosDaemonState(device_id='240:0', writer_role=1,
        schema_version=schema, host_uuid='remote', ipc_ns_inode=42,
        domain_id=domain, session_id=session, lease_duration_ns=30_000_000_000)


def test_remote_sessions_with_same_namespace_do_not_overwrite_each_other():
    receiver = SimpleNamespace(
        _host_uuid='local', _ipc_ns_inode=1, _domain_id=7, _remote_states={})
    agent.DiscoveryAgent._on_remote_state(receiver, _state(session='epoch-1'))
    agent.DiscoveryAgent._on_remote_state(receiver, _state(session='epoch-2'))
    assert set(receiver._remote_states) == {
        ('remote', 42, 7, '240:0', 'epoch-1'), ('remote', 42, 7, '240:0', 'epoch-2')}


@pytest.mark.parametrize('role,requests_expected', [(1, True), (2, False), (3, False)])
def test_only_bridge_writers_can_trigger_cross_namespace_bridges(monkeypatch, role, requests_expected):
    from ros2hermos_discovery_msgs.msg import HermosEndpoint, HermosTopic

    local, remote = _state(), _state(session='remote')
    local.host_uuid, local.ipc_ns_inode = 'local', 1
    local.topics = [HermosTopic(topic_name='/x', domain_id=7, type_name='std_msgs/msg/Int32',
                               publishers=[HermosEndpoint(node_name='/writer')])]
    remote.writer_role = role
    remote.device_id = '241:0'  # Device numbers need not match across hosts/namespaces.
    remote.topics = [HermosTopic(topic_name='/x', domain_id=7, type_name='std_msgs/msg/Int32',
                                subscribers=[HermosEndpoint(node_name='/reader')])]
    state = SimpleNamespace(_host_uuid='local', _ipc_ns_inode=1, _domain_id=7,
                            get_logger=Mock(), _remote_states={})
    dispatch = Mock()
    monkeypatch.setattr(agent.bridge_decider, 'dispatch_requests', dispatch)
    agent.DiscoveryAgent._on_remote_state(state, remote)
    agent.DiscoveryAgent._dispatch_bridge_requests(state, local)
    assert bool(state._remote_states) is requests_expected
    assert dispatch.called is requests_expected
    if requests_expected:
        assert len(dispatch.call_args.args[0]) == 1
        assert dispatch.call_args.args[0][0].topic_name == '/x'


def test_another_device_in_the_same_namespace_does_not_enable_cross_device_bridging():
    receiver = SimpleNamespace(_host_uuid='remote', _ipc_ns_inode=42, _domain_id=7,
                               _remote_states={})
    message = _state()
    message.device_id = '241:0'
    agent.DiscoveryAgent._on_remote_state(receiver, message)
    assert not receiver._remote_states


def test_short_lease_is_rejected_by_the_fixed_v2_freshness_contract():
    receiver = SimpleNamespace(
        _host_uuid='local', _ipc_ns_inode=1, _domain_id=7, _remote_states={})
    message = _state()
    message.lease_duration_ns = 1_000_000_000
    agent.DiscoveryAgent._on_remote_state(receiver, message)
    assert not receiver._remote_states


@pytest.mark.parametrize('message', [_state(domain=8), _state(schema=99)])
def test_foreign_domain_and_unknown_schema_do_not_enter_remote_cache(message):
    receiver = SimpleNamespace(
        _host_uuid='local', _ipc_ns_inode=1, _domain_id=7, _remote_states={})
    agent.DiscoveryAgent._on_remote_state(receiver, message)
    assert not receiver._remote_states


def test_expiration_uses_local_monotonic_arrival_time(monkeypatch):
    receiver = SimpleNamespace(_remote_states={'old': (_state(), 1.0), 'fresh': (_state(), 20.0)})
    monkeypatch.setattr(agent.time, 'monotonic', lambda: 32.0)
    agent.DiscoveryAgent._prune_stale_remote_states(receiver)
    assert list(receiver._remote_states) == ['fresh']


@pytest.mark.parametrize('argv,environment', [(['--observation-only'], ''), (['--observation-only'], 'off')])
def test_standalone_entry_can_disable_bridge_dispatch(monkeypatch, argv, environment):
    library = SimpleNamespace(hermos_discovery_agent_register=Mock(return_value=0))
    node = Mock()
    monkeypatch.setattr(agent, 'load_query_library', lambda: library)
    monkeypatch.setattr(agent, 'DiscoveryAgent', node)
    monkeypatch.setattr(agent.rclpy, 'init', Mock())
    monkeypatch.setattr(agent.rclpy, 'spin', Mock())
    monkeypatch.setattr(agent.rclpy, 'ok', lambda: False)
    monkeypatch.setenv('HERMOS_BRIDGE_MODE', environment)
    assert agent.main(argv) == 0
    assert node.call_args.kwargs['observation_only'] is True
    library.hermos_discovery_agent_register.assert_not_called()


@pytest.mark.parametrize('environment', ['off', 'OFF', '0'])
def test_auto_fork_claims_singleton_and_keeps_idle_exit_with_bridge_manager_off(
        monkeypatch, environment):
    library = SimpleNamespace(hermos_discovery_agent_register=Mock(side_effect=[0, 1]))
    node = Mock()
    monkeypatch.setattr(agent, 'load_query_library', lambda: library)
    monkeypatch.setattr(agent, 'DiscoveryAgent', node)
    monkeypatch.setattr(agent.rclpy, 'init', Mock())
    monkeypatch.setattr(agent.rclpy, 'spin', Mock())
    monkeypatch.setattr(agent.rclpy, 'ok', lambda: False)
    monkeypatch.setenv('HERMOS_BRIDGE_MODE', environment)
    arguments = ['discovery_agent', '--exit-when-idle']
    assert agent.main(arguments) == 0
    assert agent.main(arguments) == 0
    assert library.hermos_discovery_agent_register.call_count == 2
    node.assert_called_once_with(exit_when_idle=True, observation_only=False)
