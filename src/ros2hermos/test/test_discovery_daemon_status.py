"""Pure-logic tests for the discovery_daemon_status verb.

The verb itself talks to /proc and DDS, but the small helpers and the verdict
logic are exercised here with tmp dirs and patched checks.
"""

import os
import tempfile
from types import SimpleNamespace
from argparse import Namespace
from unittest.mock import MagicMock, patch
import pytest

from ros2hermos.verb import hermos_discovery_daemon_status as ds


@pytest.fixture(autouse=True)
def selected_device(monkeypatch):
    monkeypatch.setattr(ds, 'local_device_id', lambda: '240:0')


@pytest.mark.parametrize('host,ipc,domain,role,expected', [
    ('host', 42, 7, 1, True),
    ('host', 42, 7, 2, False),
    ('other-host', 42, 7, 1, False),
    ('host', 99, 7, 1, False),
    ('host', 42, 8, 1, False),
])
def test_gossip_requires_local_bridge_writer(host, ipc, domain, role, expected):
    state = SimpleNamespace(host_uuid=host, ipc_ns_inode=ipc, domain_id=domain,
                            device_id='240:0', writer_role=role)
    with patch.object(ds, 'NodeStrategy', MagicMock()), \
            patch.object(ds, '_local_host_uuid', return_value='host'), \
            patch.object(ds, 'collect_announcements', return_value=[state]):
        result, _ = ds._check_gossip(42, 7, 0.1)
    assert result is expected


def test_bridge_on_another_device_cannot_satisfy_status():
    state = SimpleNamespace(host_uuid='host', ipc_ns_inode=42, domain_id=7,
                            device_id='241:0', writer_role=1)
    with patch.object(ds, 'NodeStrategy', MagicMock()), \
            patch.object(ds, '_local_host_uuid', return_value='host'), \
            patch.object(ds, 'collect_announcements', return_value=[state]):
        result, _ = ds._check_gossip(42, 7, 0.1)
    assert result is False


def test_missing_device_identity_makes_gossip_unverifiable():
    with patch.object(ds, '_local_host_uuid', return_value='host'), \
            patch.object(ds, 'local_device_id', side_effect=OSError('device missing')), \
            patch.object(ds, 'collect_announcements') as collect:
        result, reason = ds._check_gossip(42, 7, 0.1)
    assert result is None
    assert 'device identity unavailable' in reason
    collect.assert_not_called()


def test_observer_cannot_hide_a_registered_bridge_agent_that_stopped_publishing(capsys):
    state = SimpleNamespace(host_uuid='host', ipc_ns_inode=42, domain_id=7,
                            device_id='240:0', writer_role=2)
    with patch.object(ds, 'NodeStrategy', MagicMock()), \
            patch.object(ds, '_self_ipc_ns_inode', return_value=42), \
            patch.object(ds, '_read_ros_domain_id', return_value=7), \
            patch.object(ds, '_local_host_uuid', return_value='host'), \
            patch.object(ds, '_check_daemon_process', return_value=(True, 'registered')), \
            patch.object(ds, 'collect_announcements', return_value=[state]):
        result = ds.DiscoveryDaemonStatusVerb().main(args=Namespace(gossip_timeout=2.0, verbose=False))
    assert result == 1
    assert 'running but not publishing' in capsys.readouterr().out


# --- type_registry: informational description (no OK/NG) --------------------

def test_describe_type_registry_missing_dir():
    with tempfile.TemporaryDirectory() as tmpdir:
        with patch.object(ds, '_type_registry_base', return_value=tmpdir):
            detail = ds._describe_type_registry(999999)
        assert 'no Hermos process has registered yet' in detail


def test_describe_type_registry_empty_dir():
    with tempfile.TemporaryDirectory() as tmpdir:
        ns_inode = 12345
        os.makedirs(os.path.join(tmpdir, str(ns_inode)))
        with patch.object(ds, '_type_registry_base', return_value=tmpdir):
            detail = ds._describe_type_registry(ns_inode)
        assert 'no Hermos process has registered yet' in detail


def test_describe_type_registry_with_live_pid_reports_count():
    with tempfile.TemporaryDirectory() as tmpdir:
        ns_inode = 12345
        ns_dir = os.path.join(tmpdir, str(ns_inode))
        os.makedirs(ns_dir)
        # Name the file after our own PID so /proc/<pid> is guaranteed live.
        with open(os.path.join(ns_dir, f'{os.getpid()}.txt'), 'w') as fp:
            fp.write('/topic\ttype\tpub\t/node\n')
        with patch.object(ds, '_type_registry_base', return_value=tmpdir):
            detail = ds._describe_type_registry(ns_inode)
        assert '1 live registration' in detail


def test_describe_type_registry_stale_pid_noted():
    """A <pid>.txt whose process is gone is noted as stale, not live."""
    with tempfile.TemporaryDirectory() as tmpdir:
        ns_inode = 12345
        ns_dir = os.path.join(tmpdir, str(ns_inode))
        os.makedirs(ns_dir)
        # PID 999999999 is unlikely to be alive.
        with open(os.path.join(ns_dir, '999999999.txt'), 'w') as fp:
            fp.write('/topic\ttype\tpub\t/node\n')
        with patch.object(ds, '_type_registry_base', return_value=tmpdir):
            detail = ds._describe_type_registry(ns_inode)
        assert 'stale' in detail
        assert 'no Hermos process has registered yet' in detail


# --- daemon process: query the kmod agent registry -------------------------

def _patch_exists(ret):
    """Patch the ioctl wrapper so hermos_discovery_agent_exists returns ``ret``."""
    lib = MagicMock()
    lib.hermos_discovery_agent_exists.return_value = ret
    return patch('ctypes.CDLL', return_value=lib)


def test_check_daemon_process_registered_is_ok():
    with _patch_exists(1):
        ok, reason = ds._check_daemon_process(0)
    assert ok is True
    assert 'registered in the kmod' in reason


def test_check_daemon_process_not_registered_is_ng():
    with _patch_exists(0):
        ok, reason = ds._check_daemon_process(3)
    assert ok is False
    assert 'no agent registered' in reason


def test_check_daemon_process_kmod_error_is_ng():
    """A negative ioctl return (module unloaded / ioctl failure) reads as not-running."""
    with _patch_exists(-1):
        ok, reason = ds._check_daemon_process(0)
    assert ok is False
    assert 'kmod query failed' in reason


def test_check_daemon_process_missing_symbol_is_ng():
    """An older/mismatched wrapper lacking the symbol reads as NG, not a CLI traceback."""
    lib = MagicMock(spec=[])  # any attribute access raises AttributeError, like an absent export
    with patch('ctypes.CDLL', return_value=lib):
        ok, reason = ds._check_daemon_process(0)
    assert ok is False
    assert 'version skew' in reason


def test_read_ros_domain_id(monkeypatch):
    monkeypatch.delenv('ROS_DOMAIN_ID', raising=False)
    assert ds._read_ros_domain_id() == 0
    monkeypatch.setenv('ROS_DOMAIN_ID', '')
    assert ds._read_ros_domain_id() == 0
    monkeypatch.setenv('ROS_DOMAIN_ID', '5')
    assert ds._read_ros_domain_id() == 5
    monkeypatch.setenv('ROS_DOMAIN_ID', 'notanint')
    assert ds._read_ros_domain_id() == 0


@pytest.mark.parametrize('raw', ['-1', '4294967296'])
def test_status_domain_does_not_wrap_invalid_values_into_another_domain(monkeypatch, raw):
    monkeypatch.setenv('ROS_DOMAIN_ID', raw)
    assert ds._read_ros_domain_id() == 0


def test_missing_boot_id_makes_gossip_unverifiable():
    with patch.object(ds, '_local_host_uuid', return_value=''), \
            patch.object(ds, 'collect_announcements') as collect:
        result, reason = ds._check_gossip(42, 7, 0.1)
    assert result is None
    assert 'host boot UUID' in reason
    collect.assert_not_called()


def test_type_registry_base_honors_hermos_tmpfs_dir(monkeypatch):
    """`HERMOS_TMPFS_DIR` overrides the `/dev/shm` default consistently with the writer."""
    monkeypatch.setenv('HERMOS_TMPFS_DIR', '/run/custom')
    assert ds._type_registry_base() == '/run/custom/hermos_type_registry'

    monkeypatch.delenv('HERMOS_TMPFS_DIR', raising=False)
    assert ds._type_registry_base() == '/dev/shm/hermos_type_registry'


# --- verdict (main) ---------------------------------------------------------

def _run_main(proc, gossip, verbose=False):
    """Run the verb's main() with the checks patched. ``gossip=None`` asserts
    the gossip check is never invoked (process down)."""
    verb = ds.DiscoveryDaemonStatusVerb()
    args = Namespace(gossip_timeout=3.0, verbose=verbose)
    with patch.object(ds, 'warn_if_gossip_timeout_overridden', lambda a: None), \
            patch.object(ds, '_self_ipc_ns_inode', return_value=4026531839), \
            patch.object(ds, '_check_daemon_process', return_value=proc), \
            patch.object(ds, '_describe_type_registry', return_value='2 live registration(s) in /x'):
        if gossip is None:
            with patch.object(ds, '_check_gossip') as gossip_mock:
                rc = verb.main(args=args)
                gossip_mock.assert_not_called()
        else:
            with patch.object(ds, '_check_gossip', return_value=gossip):
                rc = verb.main(args=args)
    return rc


def test_verdict_running(capsys):
    rc = _run_main(proc=(True, 'registered in the kmod (domain 0)'),
                   gossip=(True, 'snapshot received on /_hermos_discovery_v2'))
    out = capsys.readouterr().out
    assert rc == 0
    assert 'OK. The discovery agent is running.' in out


def test_verdict_not_running_skips_gossip(capsys):
    rc = _run_main(proc=(False, 'no agent registered in the kmod (domain 0)'), gossip=None)
    out = capsys.readouterr().out
    assert rc == 1
    assert 'NG. The discovery agent is not running.' in out
    # The per-check reason is diagnostic detail, not shown by default.
    assert 'no agent registered' not in out


def test_verdict_running_but_not_publishing(capsys):
    rc = _run_main(proc=(True, 'registered in the kmod (domain 0)'),
                   gossip=(False, 'no snapshot on /_hermos_discovery_v2 within 3.0s'))
    out = capsys.readouterr().out
    assert rc == 1
    assert 'running but not publishing' in out
    assert 'no snapshot' not in out  # reason is --verbose only


def test_verdict_distinguishes_unverifiable_publication(capsys):
    rc = _run_main(proc=(True, 'registered'),
                   gossip=(None, 'local host boot UUID unavailable'), verbose=True)
    out = capsys.readouterr().out
    assert rc == 1
    assert 'publication could not be verified' in out
    assert 'host boot UUID unavailable' in out
    assert 'not publishing' not in out
    assert 'agent not running' not in out


def test_default_output_is_verdict_only(capsys):
    """Default output carries no inode header, type_registry, or per-check reason."""
    _run_main(proc=(True, 'registered in the kmod (domain 0)'),
              gossip=(True, 'snapshot received on /_hermos_discovery_v2'))
    out = capsys.readouterr().out
    assert out.strip() == 'OK. The discovery agent is running.'


def test_verbose_shows_inode_checks_and_type_registry(capsys):
    rc = _run_main(proc=(True, 'registered in the kmod (domain 0)'),
                   gossip=(True, 'snapshot received on /_hermos_discovery_v2'), verbose=True)
    out = capsys.readouterr().out
    assert rc == 0
    assert 'IPC namespace inode: 4026531839' in out
    assert 'type_registry: 2 live registration(s)' in out
    assert 'registered in the kmod' in out  # per-check reason shown in --verbose
    assert 'OK. The discovery agent is running.' in out
