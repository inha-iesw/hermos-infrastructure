"""Registry hints must match a live process, domain, and unambiguous endpoint."""

import os
from pathlib import Path
from unittest.mock import mock_open, patch

from ros2hermos_discovery_agent.type_registry import TypeRegistryReader


def _reader(tmp_path, monkeypatch, starts):
    from ros2hermos_discovery_agent import type_registry

    monkeypatch.setattr(type_registry, '_process_start_ticks', lambda pid: starts.get(pid))
    (tmp_path / '41').mkdir()
    return TypeRegistryReader(41, str(tmp_path))


def _write(reader, pid, *lines):
    Path(reader.ns_dir, f'{pid}.txt').write_text(''.join(line + '\n' for line in lines))


def test_same_topic_and_node_are_resolved_separately_per_domain(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100, 12: 101})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    _write(reader, 12, '/x\tTypeB\tpub\t/node\t2\t101')
    reader.rebuild()
    assert reader.lookup('/x', 'pub', '/node', domain_id=1).type_name == 'TypeA'
    assert reader.lookup('/x', 'pub', '/node', domain_id=2).type_name == 'TypeB'


def test_reused_pid_and_unreadable_pid_do_not_supply_identity(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 999})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    _write(reader, 12, '/x\tTypeB\tpub\t/node\t1\t101')
    reader.rebuild()
    assert reader.lookup('/x', 'pub', '/node', domain_id=1) is None


def test_legacy_registry_does_not_guess_an_explicit_domain(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100})
    _write(reader, 11, '/x\tTypeA\tpub\t/node')
    reader.rebuild()
    assert reader.lookup('/x', 'pub', '/node', domain_id=1) is None


def test_duplicate_node_names_require_exact_pid(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100, 12: 101})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    _write(reader, 12, '/x\tTypeA\tpub\t/node\t1\t101')
    reader.rebuild()
    assert reader.lookup('/x', 'pub', '/node', domain_id=1) is None
    assert reader.lookup('/x', 'pub', '/node', domain_id=1, pid=12).pid == 12


def test_conflicting_live_registrations_remain_unknown(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100',
           '/x\tTypeB\tpub\t/node\t1\t100')
    reader.rebuild()
    assert reader.lookup('/x', 'pub', '/node', domain_id=1, pid=11) is None


def test_process_start_parser_handles_spaces_and_parentheses():
    from ros2hermos_discovery_agent.type_registry import _process_start_ticks

    fields = ['S'] + ['0'] * 18 + ['987654321'] + ['0'] * 3
    stat = '123 (worker ) with (parentheses)) ' + ' '.join(fields)
    with patch('builtins.open', mock_open(read_data=stat)) as source:
        assert _process_start_ticks(123) == 987654321
    source.assert_called_once_with('/proc/123/stat')


def test_legacy_type_hint_does_not_supply_pid_identity(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100})
    _write(reader, 11, '/x\tTypeA\tpub\t/node')
    reader.rebuild()
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1) == 'TypeA'
    assert reader.lookup('/x', 'pub', '/node', domain_id=1) is None


def test_unreadable_proc_preserves_type_but_not_pid_identity(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    reader.rebuild()
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1, pid=11) == 'TypeA'
    assert reader.lookup('/x', 'pub', '/node', domain_id=1, pid=11) is None


def test_writer_without_start_time_supplies_type_but_not_pid_identity(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t0')
    reader.rebuild()
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1, pid=11) == 'TypeA'
    assert reader.lookup('/x', 'pub', '/node', domain_id=1, pid=11) is None


def test_reused_pid_cannot_supply_a_type_hint(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 999})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    reader.rebuild()
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1, pid=11) == ''


def test_exact_domain_type_precedes_legacy_hint(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100, 12: 101})
    _write(reader, 11, '/x\tLegacyType\tpub\t/node')
    _write(reader, 12, '/x\tScopedType\tpub\t/node\t1\t101')
    reader.rebuild()
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1) == 'ScopedType'
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=2) == 'LegacyType'


def test_type_hint_does_not_cross_domain_or_native_pid(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100, 12: 101})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    _write(reader, 12, '/x\tTypeB\tpub\t/node\t2\t101')
    reader.rebuild()
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=3) == ''
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1, pid=12) == ''


def test_conflicting_scoped_types_do_not_fall_back_to_legacy(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100, 12: 101})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100',
           '/x\tTypeB\tpub\t/node\t1\t100')
    _write(reader, 12, '/x\tLegacyType\tpub\t/node')
    reader.rebuild()
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1) == ''


def test_conflicting_legacy_type_hints_remain_unknown(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {11: 100, 12: 101})
    _write(reader, 11, '/x\tTypeA\tpub\t/node')
    _write(reader, 12, '/x\tTypeB\tpub\t/node')
    reader.rebuild()
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1) == ''
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1, pid=12) == 'TypeB'


def test_strict_lookup_rechecks_pid_identity_after_rebuild(tmp_path, monkeypatch):
    starts = {11: 100}
    reader = _reader(tmp_path, monkeypatch, starts)
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    reader.rebuild()
    starts[11] = 999
    assert reader.lookup('/x', 'pub', '/node', domain_id=1, pid=11) is None


def test_type_lookup_rejects_pid_reused_after_rebuild_without_legacy_fallback(
        tmp_path, monkeypatch):
    starts = {11: 100, 12: 101}
    reader = _reader(tmp_path, monkeypatch, starts)
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    _write(reader, 12, '/x\tLegacyType\tpub\t/node')
    reader.rebuild()
    starts[11] = 999
    assert reader.lookup_type('/x', 'pub', '/node', domain_id=1) == ''


def test_rebuild_reads_process_identity_once_per_writer(tmp_path, monkeypatch):
    from ros2hermos_discovery_agent import type_registry

    reader = _reader(tmp_path, monkeypatch, {11: 100})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100',
           '/y\tTypeB\tsub\t/node\t1\t100')
    with patch.object(type_registry, '_process_start_ticks', return_value=100) as starts:
        reader.rebuild()
    starts.assert_called_once_with(11)


def test_cleanup_keeps_writers_hidden_by_proc_permissions(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {})
    _write(reader, 11, '/x\tTypeA\tpub\t/node\t1\t100')
    with patch.object(os.path, 'exists', return_value=False), \
            patch.object(os, 'kill', side_effect=PermissionError):
        reader.cleanup_dead_pids()
    assert Path(reader.ns_dir, '11.txt').exists()


def test_cleanup_ignores_filename_outside_pid_range(tmp_path, monkeypatch):
    reader = _reader(tmp_path, monkeypatch, {})
    pid = 1 << 64
    _write(reader, pid, '/x\tTypeA\tpub\t/node')
    reader.cleanup_dead_pids()
    assert Path(reader.ns_dir, f'{pid}.txt').exists()
