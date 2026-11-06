"""The local CLI snapshot uses the same domain-aware reader as the agent."""

from unittest.mock import MagicMock, patch

from ros2hermos import _ioctl


def test_local_snapshot_preserves_domain_session_and_shared_reader_endpoints(monkeypatch):
    monkeypatch.setenv('HERMOS_SESSION_ID', 'epoch-test')
    monkeypatch.setattr(_ioctl, 'local_device_id', lambda: '240:0')
    registry = MagicMock()
    with patch.object(_ioctl, '_load_lib', return_value=object()), \
            patch.object(_ioctl, '_local_ipc_ns_inode', return_value=42), \
            patch.object(_ioctl, 'TypeRegistryReader', return_value=registry), \
            patch.object(_ioctl, 'read_local_topics', return_value=[]) as read:
        state = _ioctl.self_ns_snapshot(domain_id=7)
    assert state.schema_version == 2
    assert state.writer_role == 2
    assert state.device_id == '240:0'
    assert state.domain_id == 7
    assert state.session_id == 'epoch-test'
    assert state.ipc_ns_inode == 42
    assert state.snapshot_time_ns > 0
    assert state.lease_duration_ns == 30_000_000_000
    assert read.call_args.kwargs['own_domain_id'] == 7
    assert read.call_args.kwargs['registry'] is registry
    registry.rebuild.assert_called_once_with()
    registry.cleanup_dead_pids.assert_not_called()


def test_unavailable_wrapper_does_not_create_a_fabricated_empty_snapshot():
    with patch.object(_ioctl, '_load_lib', return_value=None):
        assert _ioctl.self_ns_snapshot(domain_id=7) is None
