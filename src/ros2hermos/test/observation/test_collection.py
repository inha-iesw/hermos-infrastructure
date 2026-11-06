"""The metadata collector rejects unknown schemas and avoids local DDS work."""

from types import SimpleNamespace
from unittest.mock import MagicMock, patch
import errno

import pytest

from ros2hermos import discovery
from ros2hermos import _bridged_ros2cli
from ros2hermos.observation.cli import collect_for_args
from ros2hermos_discovery_msgs.msg import HermosDaemonState, HermosTopic


def test_zero_timeout_queries_actual_domain_without_creating_metadata_subscription():
    node = MagicMock(spec=['context', 'create_subscription'])
    node.context.get_domain_id.return_value = 7
    with patch.object(discovery, 'collect_announcements') as collect, \
            patch.object(discovery, 'self_ns_snapshot', return_value=None) as local:
        assert discovery.collect_announcements_with_fallback(node, 0) == ([], True)
    collect.assert_not_called()
    local.assert_called_once_with(domain_id=7)
    node.create_subscription.assert_not_called()


def test_collector_ignores_unknown_schema_then_accepts_live_v2_snapshot(capsys):
    valid = HermosDaemonState(device_id='240:0', writer_role=1, schema_version=2, host_uuid='host', ipc_ns_inode=42,
                             domain_id=7, session_id='epoch', lease_duration_ns=30_000_000_000)
    invalid = HermosDaemonState(device_id='240:0', writer_role=1, schema_version=99)
    node = MagicMock(spec=['create_subscription', 'destroy_subscription',
                           'get_publishers_info_by_topic'])
    node.get_publishers_info_by_topic.return_value = [SimpleNamespace(endpoint_gid=[1, 2])]
    messages = iter([invalid, valid])

    def spin_once(spin_node, timeout_sec):
        callback = node.create_subscription.call_args.args[2]
        callback(next(messages))

    with patch.object(discovery.rclpy, 'spin_once', side_effect=spin_once):
        assert discovery.collect_announcements(node, timeout_sec=1) == [valid]
    assert 'unsupported HERM0S metadata schema 99' in capsys.readouterr().err
    node.destroy_subscription.assert_called_once()


@pytest.mark.parametrize('error, status', [
    (OSError(errno.EACCES, 'device denied'), 'permission_denied'),
    (OSError(errno.ENOENT, 'device missing'), 'unavailable'),
])
def test_local_metadata_errors_remain_distinct_from_an_empty_domain(error, status):
    args = SimpleNamespace(local=True, gossip_timeout=2.0)
    with patch.object(discovery, 'collect_announcements_with_fallback', side_effect=error):
        with pytest.raises(ValueError, match=status):
            collect_for_args(None, args)


def test_missing_wrapper_is_reported_as_unavailable_instead_of_an_empty_domain():
    args = SimpleNamespace(local=True, gossip_timeout=2.0)
    with patch.object(discovery, 'collect_announcements_with_fallback', return_value=([], True)):
        with pytest.raises(ValueError, match='unavailable'):
            collect_for_args(None, args)


@pytest.mark.parametrize('error, status', [
    (OSError(errno.EACCES, 'device denied'), 'permission_denied'),
    (OSError(errno.ENOENT, 'device missing'), 'unavailable'),
    (None, 'unavailable'),
])
def test_default_collection_reports_local_failure_and_keeps_dds_available(error, status, capsys):
    args = SimpleNamespace(local=False, gossip_timeout=2.0)
    with patch.object(discovery, 'collect_announcements', return_value=[]), \
            patch.object(discovery, 'self_ns_snapshot', return_value=None, side_effect=error):
        assert collect_for_args(None, args) == []
    assert f'NOTE: Local HERM0S metadata {status}' in capsys.readouterr().err


@pytest.mark.parametrize('error', [OSError(errno.ENOENT, 'device missing'), None])
def test_bridge_type_resolution_without_native_device_returns_none(error, capsys):
    strategy = MagicMock()
    strategy.return_value.__enter__.return_value.get_topic_names_and_types.return_value = []
    with patch.object(_bridged_ros2cli, 'NodeStrategy', strategy), \
            patch.object(discovery, 'collect_announcements', return_value=[]), \
            patch.object(discovery, 'self_ns_snapshot', return_value=None, side_effect=error):
        assert _bridged_ros2cli.resolve_type_name('/missing', 2.0) is None
    assert 'Could not resolve message type' in capsys.readouterr().out


def metadata_type(session, type_name, domain=7, *, ipc=42, role=1):
    return HermosDaemonState(device_id='240:0', writer_role=role,
        schema_version=2, host_uuid='host', ipc_ns_inode=ipc, domain_id=domain,
        session_id=session, lease_duration_ns=30_000_000_000,
        topics=[HermosTopic(topic_name='/same', domain_id=domain, type_name=type_name)])


@pytest.mark.parametrize('snapshots, expected', [
    ([metadata_type('a', 'example/msg/A'), metadata_type('b', 'example/msg/B', ipc=43)], None),
    ([metadata_type('a', 'example/msg/A'), metadata_type('b', 'example/msg/A', ipc=43)], 'example/msg/A'),
    ([metadata_type('bridge', 'example/msg/A'), metadata_type('observer', 'example/msg/B', role=2)], 'example/msg/A'),
    ([metadata_type('a', 'example/msg/A', domain=9),
      metadata_type('b', 'example/msg/B')], 'example/msg/B'),
])
def test_bridge_type_is_resolved_only_when_unique_within_cli_domain(snapshots, expected):
    strategy = MagicMock()
    node = strategy.return_value.__enter__.return_value
    node.get_topic_names_and_types.return_value = []
    node.direct_node.node.context.get_domain_id.return_value = 7
    with patch.object(_bridged_ros2cli, 'NodeStrategy', strategy), \
            patch.object(discovery, 'collect_announcements', return_value=snapshots):
        assert _bridged_ros2cli.resolve_type_name('/same', 2.0) == expected
