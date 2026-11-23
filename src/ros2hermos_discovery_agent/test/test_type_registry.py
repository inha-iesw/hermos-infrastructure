"""Unit tests for the discovery agent's TypeRegistryReader.

The reader scans a per-namespace tmpfs directory of `<pid>.txt` files
written by `hermoslib::internal::TypeRegistryWriter`. These tests use
a temporary directory as the base — no kmod, no rclpy.
"""

import os
import tempfile

import pytest
from pathlib import Path
from types import SimpleNamespace

from ros2hermos_discovery_agent.type_registry import (
    RegistryEntry,
    TypeRegistryReader,
)


def _write(path: str, content: str) -> None:
    with open(path, 'w', encoding='utf-8') as fp:
        fp.write(content)
    os.chmod(path, 0o600)


def _make_reader(tmpdir: str, ns_inode: int = 1234) -> TypeRegistryReader:
    os.makedirs(os.path.join(tmpdir, str(ns_inode)), mode=0o700, exist_ok=True)
    return TypeRegistryReader(ipc_ns_inode=ns_inode, base_dir=tmpdir)


def test_rebuild_parses_well_formed_lines():
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        _write(
            os.path.join(reader.ns_dir, '1111.txt'),
            '/chatter\tstd_msgs/msg/Int32\tpub\t/talker_node\n'
            '/echo\tstd_msgs/msg/String\tsub\t/listener_node\n')
        reader.rebuild()

        assert reader.table[('/chatter', 'pub', '/talker_node', None, 1111)] == RegistryEntry(
            pid=1111, type_name='std_msgs/msg/Int32')
        assert reader.table[('/echo', 'sub', '/listener_node', None, 1111)] == RegistryEntry(
            pid=1111, type_name='std_msgs/msg/String')


def test_rebuild_replaces_previous_table():
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        path = os.path.join(reader.ns_dir, '1111.txt')
        _write(path, '/old\tstd_msgs/msg/Int32\tpub\t/n\n')
        reader.rebuild()
        assert reader.lookup_type('/old', 'pub', '/n', domain_id=0) == 'std_msgs/msg/Int32'

        os.unlink(path)
        reader.rebuild()
        assert reader.lookup_type('/old', 'pub', '/n', domain_id=0) == ''


def test_rebuild_handles_missing_ns_dir():
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = TypeRegistryReader(ipc_ns_inode=99999, base_dir=tmpdir)
        reader.rebuild()  # should not raise
        assert reader.table == {}


def test_rebuild_skips_unterminated_tail():
    """A writer dying mid-write leaves a tail without `\\n`. Skip it."""
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        _write(
            os.path.join(reader.ns_dir, '1.txt'),
            '/good\tstd_msgs/msg/Int32\tpub\t/node_a\n'
            '/partial\tstd_msgs/msg/Float64\tpub')
        reader.rebuild()

        assert reader.lookup_type('/good', 'pub', '/node_a', domain_id=0) == 'std_msgs/msg/Int32'
        # The partial line must be dropped.
        assert reader.lookup_type('/partial', 'pub', '', domain_id=0) == ''


def test_rebuild_skips_lines_with_too_few_fields(caplog):
    """Lines with fewer than 4 tab-separated fields are dropped + warned."""
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        _write(
            os.path.join(reader.ns_dir, '1.txt'),
            '/good\tstd_msgs/msg/Int32\tpub\t/node_a\n'
            '/bad\tonly_two_fields\n')
        reader.rebuild()
        assert reader.lookup_type('/good', 'pub', '/node_a', domain_id=0) == 'std_msgs/msg/Int32'
        # No assertion on warning text since logger is optional.


def test_rebuild_accepts_extra_fields_forward_compat():
    """Lines with 5+ fields keep their first 4 fields and ignore extras."""
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        _write(
            os.path.join(reader.ns_dir, '7.txt'),
            '/topic\tstd_msgs/msg/Int32\tpub\t/node_a\tfuture_field_v2\tand_one_more\n')
        reader.rebuild()
        entry = reader.table[('/topic', 'pub', '/node_a', None, 7)]
        assert entry == RegistryEntry(pid=7, type_name='std_msgs/msg/Int32')


def test_rebuild_skips_lines_with_unknown_role():
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        _write(
            os.path.join(reader.ns_dir, '1.txt'),
            '/topic\tstd_msgs/msg/Int32\tunknown_role\t/node_a\n')
        reader.rebuild()
        assert reader.lookup_type('/topic', 'unknown_role', '/node_a', domain_id=0) == ''
        assert reader.lookup_type('/topic', 'pub', '/node_a', domain_id=0) == ''


def test_rebuild_records_each_endpoint_independently_for_same_topic():
    """Two endpoints on the same topic with conflicting `type_name` are both stored.

    Hermos enforces the one-message-type-per-topic invariant at the kmod
    layer (see https://autowarefoundation.github.io/agnocast_doc/#one-message-type-per-topic);
    the registry deliberately does not deduplicate — it reports exactly what
    each writer announced so the discovery agent can surface a mismatch
    rather than silently masking it.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        # Two processes registering the same topic with conflicting types.
        _write(
            os.path.join(reader.ns_dir, '1.txt'),
            '/conflict_topic\tstd_msgs/msg/Int32\tpub\t/node_a\n')
        _write(
            os.path.join(reader.ns_dir, '2.txt'),
            '/conflict_topic\tstd_msgs/msg/String\tpub\t/node_b\n')
        reader.rebuild()

        # Each endpoint keeps its own announced type — keyed by node_name,
        # the table is per-endpoint, not per-topic.
        assert reader.table[('/conflict_topic', 'pub', '/node_a', None, 1)] == RegistryEntry(
            pid=1, type_name='std_msgs/msg/Int32')
        assert reader.table[('/conflict_topic', 'pub', '/node_b', None, 2)] == RegistryEntry(
            pid=2, type_name='std_msgs/msg/String')


def test_cleanup_dead_pids_removes_files_for_dead_pids():
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        # PID 1 is always alive (init). PID 999999999 is unlikely to be alive.
        alive_path = os.path.join(reader.ns_dir, '1.txt')
        dead_path = os.path.join(reader.ns_dir, '999999999.txt')
        _write(alive_path, '/t\tT\tpub\t/n\n')
        _write(dead_path, '/t\tT\tpub\t/n\n')

        reader.cleanup_dead_pids()

        assert os.path.exists(alive_path)
        assert not os.path.exists(dead_path)


def test_cleanup_dead_pids_handles_missing_ns_dir():
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = TypeRegistryReader(ipc_ns_inode=99999, base_dir=tmpdir)
        reader.cleanup_dead_pids()  # should not raise


def test_rebuild_skips_non_numeric_filenames():
    with tempfile.TemporaryDirectory() as tmpdir:
        reader = _make_reader(tmpdir)
        _write(os.path.join(reader.ns_dir, 'README.txt'),
               '/topic\tstd_msgs/msg/Int32\tpub\t/node_a\n')
        reader.rebuild()
        # File ignored because its name doesn't parse as int.
        assert reader.table == {}


def test_default_base_dir_honors_hermos_tmpfs_dir(monkeypatch):
    """`HERMOS_TMPFS_DIR` overrides the `/dev/shm` default for the registry root."""
    from ros2hermos_discovery_agent import type_registry

    monkeypatch.setenv('HERMOS_TMPFS_DIR', '/run/custom')
    assert type_registry._default_base_dir() == f'/run/custom/hermos_type_registry_{os.geteuid()}'

    monkeypatch.delenv('HERMOS_TMPFS_DIR', raising=False)
    assert type_registry._default_base_dir() == f'/dev/shm/hermos_type_registry_{os.geteuid()}'

    monkeypatch.setenv('HERMOS_TMPFS_DIR', '')
    # Empty string is treated as unset (falsy) and the default applies.
    assert type_registry._default_base_dir() == f'/dev/shm/hermos_type_registry_{os.geteuid()}'


@pytest.mark.parametrize('kind', ['base_symlink', 'namespace_symlink', 'file_symlink',
                                 'base_public', 'namespace_public', 'file_public', 'hardlink'])
def test_unsafe_registry_objects_are_neither_read_nor_removed(tmp_path, kind):
    base = tmp_path / 'base'
    base.mkdir(mode=0o700)
    reader = _make_reader(str(base))
    path = Path(reader.ns_dir, '999999999.txt')
    _write(str(path), '/secret\tPrivateType\tpub\t/node\n')
    if kind == 'base_symlink':
        alias = tmp_path / 'alias'
        alias.symlink_to(base, target_is_directory=True)
        reader = TypeRegistryReader(1234, str(alias))
    elif kind == 'namespace_symlink':
        target = base / 'real'
        Path(reader.ns_dir).rename(target)
        Path(reader.ns_dir).symlink_to(target, target_is_directory=True)
        path = target / path.name
    elif kind == 'file_symlink':
        target = base / 'real.txt'
        path.rename(target)
        path.symlink_to(target)
        path = target
    elif kind == 'hardlink':
        os.link(path, base / 'alias.txt')
    else:
        {'base_public': base, 'namespace_public': Path(reader.ns_dir),
         'file_public': path}[kind].chmod(0o755 if kind != 'file_public' else 0o644)
    reader.rebuild()
    assert reader.table == {}
    reader.cleanup_dead_pids()
    assert path.read_text() == '/secret\tPrivateType\tpub\t/node\n'


@pytest.mark.parametrize('target', ['directory', 'file'])
def test_foreign_owner_metadata_is_rejected_before_read_or_cleanup(tmp_path, monkeypatch, target):
    import stat
    from ros2hermos_discovery_agent import type_registry

    reader = _make_reader(str(tmp_path))
    path = Path(reader.ns_dir, '999999999.txt')
    _write(str(path), '/secret\tPrivateType\tpub\t/node\n')
    real_fstat = os.fstat

    def foreign_owner(fd):
        metadata = real_fstat(fd)
        selected = stat.S_ISDIR(metadata.st_mode) if target == 'directory' else stat.S_ISREG(metadata.st_mode)
        return SimpleNamespace(st_mode=metadata.st_mode,
                               st_uid=os.geteuid() + 1 if selected else metadata.st_uid,
                               st_nlink=metadata.st_nlink, st_dev=metadata.st_dev,
                               st_ino=metadata.st_ino)

    monkeypatch.setattr(type_registry.os, 'fstat', foreign_owner)
    reader.rebuild()
    assert reader.table == {}
    reader.cleanup_dead_pids()
    assert path.exists()
