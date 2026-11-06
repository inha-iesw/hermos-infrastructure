"""CLI output must not combine identically named native endpoints across scopes."""

from argparse import Namespace
from contextlib import ExitStack
import errno
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import pytest

from ros2hermos.observation.stats import StatsHermosVerb
from ros2hermos.observation.scope import ReceivedSnapshots
from ros2hermos.verb.topic_info_hermos import TopicInfoHermosVerb
from ros2hermos.verb.topic_list_hermos import ListHermosVerb
from ros2hermos.verb.node_info_hermos import NodeInfoHermosVerb
from ros2hermos.verb.node_list_hermos import ListHermosVerb as NodeListHermosVerb
from ros2hermos_discovery_msgs.msg import HermosDaemonState, HermosEndpoint, HermosTopic


def state(session, *, domain=7, ipc=42, role=1, device='240:0', type_name='example/msg/Native'):
    endpoint = HermosEndpoint(node_name='/talker', pid=123, qos_is_reliable=True)
    topic = HermosTopic(topic_name='/same', domain_id=domain, type_name=type_name,
                       publishers=[endpoint])
    return HermosDaemonState(device_id=device, writer_role=role, schema_version=2, host_uuid='host', ipc_ns_inode=ipc,
                             domain_id=domain, session_id=session,
                             lease_duration_ns=30_000_000_000, topics=[topic])


def args(**overrides):
    values = dict(gossip_timeout=2.0, debug=False, verbose=True, local=False,
                  topic_name='/same', node_name='/talker', all=False, count_nodes=False,
                  session_id=None, ipc_namespace=None, host_id=None)
    values.update(overrides)
    return Namespace(**values)


def node_strategy():
    strategy = MagicMock()
    node = strategy.return_value.__enter__.return_value
    node.direct_node.node.context.get_domain_id.return_value = 7
    node.get_publishers_info_by_topic.return_value = []
    node.get_subscriptions_info_by_topic.return_value = []
    return strategy


@pytest.mark.parametrize('verb_type', [
    ListHermosVerb, NodeListHermosVerb, TopicInfoHermosVerb, NodeInfoHermosVerb, StatsHermosVerb,
])
@pytest.mark.parametrize('session', [None, 'observer'])
def test_coexisting_writers_have_one_default_view_and_explicit_observer_access(
        verb_type, session, capsys):
    import importlib

    module = importlib.import_module(verb_type.__module__)
    snapshots = ReceivedSnapshots([
        (state('bridge', role=1), 1), (state('observer', role=2), 2)])
    with ExitStack() as stack:
        stack.enter_context(patch.object(module, 'NodeStrategy', node_strategy()))
        for name in ('get_topic_names_and_types', 'get_node_names'):
            if hasattr(module, name):
                stack.enter_context(patch.object(module, name, return_value=[]))
        stack.enter_context(patch('ros2hermos.discovery.collect_announcements', return_value=snapshots))
        assert verb_type().main(args=args(session_id=session)) is None
    output = capsys.readouterr().out
    chosen = session or 'bridge'
    other = 'bridge' if session else 'observer'
    assert output.count('session=' + chosen) == 1
    assert 'session=' + other not in output


@pytest.mark.parametrize('verb_type', [ListHermosVerb, NodeListHermosVerb, StatsHermosVerb])
def test_default_inventory_keeps_both_devices_visible(verb_type, capsys):
    import importlib

    module = importlib.import_module(verb_type.__module__)
    snapshots = [state('bridge', role=1), state('observer', role=2, device='241:0')]
    with ExitStack() as stack:
        stack.enter_context(patch.object(module, 'NodeStrategy', node_strategy()))
        for name in ('get_topic_names_and_types', 'get_node_names'):
            if hasattr(module, name):
                stack.enter_context(patch.object(module, name, return_value=[]))
        stack.enter_context(patch('ros2hermos.discovery.collect_announcements', return_value=snapshots))
        assert verb_type().main(args=args()) is None
    output = capsys.readouterr().out
    assert output.count('device=240:0') == 1
    assert output.count('device=241:0') == 1


@pytest.mark.parametrize('verb_type', [TopicInfoHermosVerb, NodeInfoHermosVerb])
def test_info_disambiguates_devices_and_can_select_observer(verb_type, capsys):
    import importlib

    module = importlib.import_module(verb_type.__module__)
    snapshots = [state('bridge', role=1), state('observer', role=2, device='241:0')]
    with ExitStack() as stack:
        stack.enter_context(patch.object(module, 'NodeStrategy', node_strategy()))
        for name in ('get_topic_names_and_types', 'get_node_names'):
            if hasattr(module, name):
                stack.enter_context(patch.object(module, name, return_value=[]))
        stack.enter_context(patch('ros2hermos.discovery.collect_announcements', return_value=snapshots))
        assert 'multiple HERM0S scopes' in verb_type().main(args=args())
        assert verb_type().main(args=args(device_id='241:0')) is None
    output = capsys.readouterr().out
    assert 'device=241:0' in output
    assert 'device=240:0' not in output


def test_topic_list_displays_only_latest_session_for_the_same_route(capsys):
    snapshots = [state('epoch-a'), state('epoch-b'), state('other-domain', domain=9)]
    with patch('ros2hermos.verb.topic_list_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.verb.topic_list_hermos.get_topic_names_and_types', return_value=[]), \
            patch('ros2hermos.discovery.collect_announcements', return_value=ReceivedSnapshots((state, index) for index, state in enumerate(snapshots))):
        ListHermosVerb().main(args=args())
    rows = [line for line in capsys.readouterr().out.splitlines() if line.startswith('/same')]
    assert len(rows) == 1
    assert 'session=epoch-b' in rows[0]
    assert all('domain=7' in row for row in rows)


def test_topic_info_requires_scope_selection_before_adding_native_counts():
    with patch('ros2hermos.verb.topic_info_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.discovery.collect_announcements',
                  return_value=[state('epoch-a'), state('epoch-b', ipc=43)]):
        result = TopicInfoHermosVerb().main(args=args())
    assert 'multiple HERM0S scopes' in result


def test_topic_info_selected_scope_preserves_unknown_native_type_and_real_pid(capsys):
    snapshots = [state('epoch-a'), state('epoch-b', type_name='')]
    with patch('ros2hermos.verb.topic_info_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.discovery.collect_announcements', return_value=ReceivedSnapshots((state, index) for index, state in enumerate(snapshots))):
        assert TopicInfoHermosVerb().main(args=args(session_id='epoch-b')) is None
    text = capsys.readouterr().out
    assert 'session=epoch-b' in text
    assert 'session=epoch-a' not in text
    assert 'Type: <UNKNOWN>' in text
    assert 'PID: 123' in text
    assert 'Hermos Publisher count: 1' in text


def test_native_publisher_reliability_is_unknown_instead_of_inferred(capsys):
    with patch('ros2hermos.verb.topic_info_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.discovery.collect_announcements', return_value=[state('epoch-a')]):
        TopicInfoHermosVerb().main(args=args())
    assert 'Reliability: unknown (native publisher does not expose this QoS)' in capsys.readouterr().out


def test_node_list_counts_scopes_instead_of_collapsing_equal_fqns(capsys):
    with patch('ros2hermos.verb.node_list_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.verb.node_list_hermos.get_node_names', return_value=[]), \
            patch('ros2hermos.discovery.collect_announcements',
                  return_value=[state('epoch-a'), state('epoch-b', ipc=43)]):
        NodeListHermosVerb().main(args=args(count_nodes=True))
    assert capsys.readouterr().out.strip() == '2'


def test_node_info_rejects_ambiguous_native_fqn():
    with patch('ros2hermos.verb.node_info_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.verb.node_info_hermos.get_node_names', return_value=[]), \
            patch('ros2hermos.verb.node_info_hermos.get_topic_names_and_types', return_value=[]), \
            patch('ros2hermos.discovery.collect_announcements',
                  return_value=[state('epoch-a'), state('epoch-b', ipc=43)]):
        result = NodeInfoHermosVerb().main(args=args())
    assert 'multiple HERM0S scopes' in result


def test_node_info_selected_scope_shows_native_pid_and_unknown_type(capsys):
    with patch('ros2hermos.verb.node_info_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.verb.node_info_hermos.get_node_names', return_value=[]), \
            patch('ros2hermos.verb.node_info_hermos.get_topic_names_and_types', return_value=[]), \
            patch('ros2hermos.discovery.collect_announcements',
                  return_value=ReceivedSnapshots([(state('epoch-a'), 1), (state('epoch-b', type_name=''), 2)])):
        NodeInfoHermosVerb().main(args=args(session_id='epoch-b'))
    text = capsys.readouterr().out
    assert 'session=epoch-b' in text
    assert 'PIDs: 123' in text
    assert '/same: <UNKNOWN>' in text


def test_explicit_missing_session_does_not_succeed_with_unrelated_dds_topic():
    strategy = node_strategy()
    node = strategy.return_value.__enter__.return_value
    node.get_publishers_info_by_topic.return_value = [
        SimpleNamespace(node_name='dds_talker', topic_type='example/msg/DDS')]
    with patch('ros2hermos.verb.topic_info_hermos.NodeStrategy', strategy), \
            patch('ros2hermos.discovery.collect_announcements', return_value=[state('epoch-a')]):
        result = TopicInfoHermosVerb().main(args=args(session_id='missing', verbose=False))
    assert result == 'no HERM0S scope matches selectors'


def test_explicit_missing_node_scope_fails_before_dds_lookup():
    with patch('ros2hermos.verb.node_info_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.verb.node_info_hermos.get_node_names') as dds_lookup, \
            patch('ros2hermos.discovery.collect_announcements', return_value=[state('epoch-a')]):
        result = NodeInfoHermosVerb().main(args=args(ipc_namespace=999))
    assert result == 'no HERM0S scope matches selectors'
    dds_lookup.assert_not_called()


@pytest.mark.parametrize('bridged, label', [
    (False, 'WARN: one or more necessary bridges are not running'),
    (True, 'Hermos enabled, bridged'),
])
@pytest.mark.parametrize('select_session', [None, 'talker-epoch'])
def test_topic_list_bridge_status_includes_other_namespace_sessions(
        bridged, label, select_session, capsys):
    talker, listener = cross_namespace_states(bridged)
    with patch('ros2hermos.verb.topic_list_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.verb.topic_list_hermos.get_topic_names_and_types', return_value=[]), \
            patch('ros2hermos.discovery.collect_announcements', return_value=[talker, listener]):
        assert ListHermosVerb().main(args=args(session_id=select_session)) is None
    rows = capsys.readouterr().out.splitlines()
    assert len(rows) == (1 if select_session else 2)
    assert all(label in row for row in rows)


def cross_namespace_states(bridged=False):
    talker = state('talker-epoch')
    listener = state('listener-epoch')
    listener.ipc_ns_inode = 99
    listener.topics[0].publishers = []
    listener.topics[0].subscribers = [HermosEndpoint(node_name='/listener', pid=456)]
    if bridged:
        talker.topics[0].subscribers = [HermosEndpoint(node_name='/outbound', is_bridge=True)]
        listener.topics[0].publishers = [HermosEndpoint(node_name='/inbound', is_bridge=True)]
    return talker, listener


@pytest.mark.parametrize('bridged, label', [
    (False, 'WARN: one or more necessary bridges are not running'),
    (True, 'Hermos enabled, bridged'),
])
def test_node_info_bridge_status_includes_other_namespace_sessions(bridged, label, capsys):
    talker, listener = cross_namespace_states(bridged)
    with patch('ros2hermos.verb.node_info_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.verb.node_info_hermos.get_node_names', return_value=[]), \
            patch('ros2hermos.verb.node_info_hermos.get_topic_names_and_types', return_value=[]), \
            patch('ros2hermos.discovery.collect_announcements', return_value=[talker, listener]):
        assert NodeInfoHermosVerb().main(args=args(session_id='talker-epoch')) is None
    assert label in capsys.readouterr().out


def test_topic_bridge_status_excludes_other_domains_and_superseded_sessions(capsys):
    talker, old_listener = cross_namespace_states()
    restarted = HermosDaemonState(device_id='240:0', writer_role=1,
        schema_version=2, host_uuid='host', ipc_ns_inode=99, domain_id=7,
        session_id='a-restarted', lease_duration_ns=30_000_000_000)
    other_domain = state('other-domain', domain=9)
    other_domain.topics[0].publishers = []
    other_domain.topics[0].subscribers = [HermosEndpoint(node_name='/other')]
    with patch('ros2hermos.verb.topic_list_hermos.NodeStrategy', node_strategy()), \
            patch('ros2hermos.verb.topic_list_hermos.get_topic_names_and_types', return_value=[]), \
            patch('ros2hermos.discovery.collect_announcements',
                  return_value=ReceivedSnapshots([(talker, 1), (old_listener, 2), (restarted, 3), (other_domain, 4)])):
        ListHermosVerb().main(args=args(session_id='talker-epoch'))
    text = capsys.readouterr().out
    assert 'Hermos enabled' in text
    assert 'WARN' not in text


@pytest.mark.parametrize('count_nodes', [False, True])
def test_node_list_merges_dds_visibility_into_native_scope_row(count_nodes, capsys):
    strategy = node_strategy()
    strategy.return_value.__enter__.return_value.get_service_names_and_types_by_node.return_value = [
        ('/talker/get_parameters', ['rcl_interfaces/srv/GetParameters'])]
    with patch('ros2hermos.verb.node_list_hermos.NodeStrategy', strategy), \
            patch('ros2hermos.verb.node_list_hermos.get_node_names', return_value=[
                SimpleNamespace(full_name='/talker'), SimpleNamespace(full_name='/plain')]), \
            patch('ros2hermos.discovery.collect_announcements', return_value=[state('epoch-a')]):
        NodeListHermosVerb().main(args=args(count_nodes=count_nodes))
    rows = capsys.readouterr().out.strip().splitlines()
    if count_nodes:
        assert rows == ['2']
    else:
        assert rows[0] == '/plain'
        assert len(rows) == 2
        assert 'Hermos enabled' in rows[1] and 'DDS' in rows[1]


@pytest.mark.parametrize('verb, module, expected', [
    (ListHermosVerb, 'topic_list_hermos', '/same'),
    (TopicInfoHermosVerb, 'topic_info_hermos', 'ROS 2 Publisher count: 1'),
    (NodeListHermosVerb, 'node_list_hermos', '/talker'),
    (NodeInfoHermosVerb, 'node_info_hermos', '/same: example/msg/DDS'),
])
@pytest.mark.parametrize('error, status', [
    (OSError(errno.ENOENT, 'device missing'), 'unavailable'),
    (OSError(errno.EACCES, 'device denied'), 'permission_denied'),
    (None, 'unavailable'),
])
def test_dds_only_verbs_keep_results_when_native_query_unavailable(
        verb, module, expected, error, status, capsys):
    strategy = node_strategy()
    node = strategy.return_value.__enter__.return_value
    node.get_publishers_info_by_topic.return_value = [
        SimpleNamespace(node_name='talker', topic_type='example/msg/DDS')]
    prefix = f'ros2hermos.verb.{module}.'
    with ExitStack() as stack:
        stack.enter_context(patch(prefix + 'NodeStrategy', strategy))
        stack.enter_context(patch('ros2hermos.discovery.collect_announcements', return_value=[]))
        stack.enter_context(patch('ros2hermos.discovery.self_ns_snapshot',
                                  return_value=None, side_effect=error))
        if module in ('topic_list_hermos', 'node_info_hermos'):
            stack.enter_context(patch(prefix + 'get_topic_names_and_types',
                                      return_value=[('/same', ['example/msg/DDS'])]))
        if module.startswith('node_'):
            stack.enter_context(patch(prefix + 'get_node_names',
                                      return_value=[SimpleNamespace(full_name='/talker')]))
        if module == 'node_info_hermos':
            for query in ('get_subscriber_info', 'get_service_server_info',
                          'get_service_client_info', 'get_action_server_info', 'get_action_client_info'):
                stack.enter_context(patch(prefix + query, return_value=[]))
            stack.enter_context(patch(prefix + 'get_publisher_info', return_value=[
                SimpleNamespace(name='/same', types=['example/msg/DDS'])]))
        assert verb().main(args=args(verbose=False)) is None
    captured = capsys.readouterr()
    assert expected in captured.out
    assert f'NOTE: Local HERM0S metadata {status}' in captured.err


@pytest.mark.parametrize('local', [False, True])
def test_stats_requires_native_metadata_even_when_dds_exists(local):
    with patch('ros2hermos.observation.stats.NodeStrategy', node_strategy()), \
            patch('ros2hermos.discovery.collect_announcements', return_value=[]), \
            patch('ros2hermos.discovery.self_ns_snapshot',
                  side_effect=OSError(errno.EACCES, 'device denied')):
        result = StatsHermosVerb().main(args=args(local=local))
    assert 'permission_denied' in result
