"""Scope and freshness contracts for metadata-only CLI observation."""

from types import SimpleNamespace

import pytest

from ros2hermos.observation.scope import (
    SnapshotCache, scoped_snapshots, select_scope, scope_label,
)


def snapshot(*, session='epoch-a', domain=7, host='host', ipc=42, role=1, device='240:0',
             version=2, timestamp=100, lease=30_000_000_000, topics=None):
    return SimpleNamespace(
        schema_version=version, host_uuid=host, ipc_ns_inode=ipc, writer_role=role,
        domain_id=domain, device_id=device, session_id=session, snapshot_time_ns=timestamp,
        lease_duration_ns=lease,
        topics=topics if topics is not None else [
            SimpleNamespace(topic_name='/same', domain_id=domain)],
    )


def test_same_topic_in_different_domains_and_sessions_remains_scoped():
    states = [snapshot(), snapshot(session='epoch-b'), snapshot(domain=9)]
    scoped = scoped_snapshots(states, domain_id=7)
    assert [(scope.domain_id, scope.session_id) for scope, _ in scoped] == [
        (7, 'epoch-a'), (7, 'epoch-b')]
    assert all(state.topics[0].topic_name == '/same' for _, state in scoped)


def test_scope_selector_requires_disambiguation_instead_of_merging_endpoints():
    states = [snapshot(), snapshot(session='epoch-b')]
    with pytest.raises(ValueError, match='multiple HERM0S scopes'):
        select_scope(scoped_snapshots(states, domain_id=7))
    scoped = scoped_snapshots(states, domain_id=7, session_id='epoch-b')
    assert select_scope(scoped)[0].session_id == 'epoch-b'


def test_scope_label_keeps_host_namespace_domain_and_unknown_session():
    scope, _ = scoped_snapshots([snapshot(session='')], domain_id=7)[0]
    assert scope_label(scope) == 'host=host ipc=42 domain=7 device=240:0 session=unknown'


def test_legacy_wire_schema_is_not_accepted_as_a_v2_snapshot():
    with pytest.raises(ValueError, match='schema 1'):
        scoped_snapshots([snapshot(version=1)], domain_id=7)


def test_cache_expires_by_receive_clock_without_comparing_remote_wall_clock():
    cache = SnapshotCache()
    cache.update(snapshot(timestamp=9_000_000_000_000), received_at=10.0)
    assert len(cache.active(now=39.9)) == 1
    assert cache.active(now=40.0) == []


def test_cache_discards_snapshot_when_its_dds_writer_disappears():
    cache = SnapshotCache()
    cache.update(snapshot(), received_at=10.0, publisher_gid=b'writer-a')
    assert len(cache.active(now=10.1, live_gids={b'writer-a'})) == 1
    assert cache.active(now=10.2, live_gids={b'writer-b'}) == []


def test_cache_keeps_restart_epochs_distinct_and_replaces_same_epoch_snapshot():
    cache = SnapshotCache()
    cache.update(snapshot(timestamp=100), received_at=1.0)
    cache.update(snapshot(timestamp=200), received_at=1.1)
    cache.update(snapshot(session='epoch-b'), received_at=1.2)
    assert [(state.session_id, state.snapshot_time_ns) for state in cache.active(now=1.3)] == [
        ('epoch-a', 200), ('epoch-b', 100)]


@pytest.mark.parametrize('state', [snapshot(version=99), snapshot(lease=0), snapshot(lease=1_000_000_000)])
def test_cache_rejects_unknown_schema_and_missing_freshness_contract(state):
    with pytest.raises(ValueError):
        SnapshotCache().update(state, received_at=0)


def test_v2_snapshot_rejects_topic_domain_mismatch():
    state = snapshot(topics=[SimpleNamespace(topic_name='/same', domain_id=9)])
    with pytest.raises(ValueError, match='domain'):
        SnapshotCache().update(state, received_at=0)


def test_two_live_writers_cannot_overwrite_the_same_session_silently():
    cache = SnapshotCache()
    cache.update(snapshot(), received_at=1, publisher_gid=b'first-agent')
    with pytest.raises(ValueError, match='duplicate'):
        cache.update(snapshot(), received_at=1.1, publisher_gid=b'second-agent')
    assert cache.active(now=1.2) == []


def test_refresh_moves_session_to_end_of_local_receipt_order():
    cache = SnapshotCache()
    cache.update(snapshot(session='a'), received_at=1)
    cache.update(snapshot(session='b'), received_at=1.1)
    cache.update(snapshot(session='a'), received_at=1.2)
    assert [state.session_id for state in cache.active(now=1.3)] == ['b', 'a']


def test_bridge_routing_uses_receipt_time_after_display_sort(monkeypatch):
    from ros2hermos import discovery
    from ros2hermos.observation.cli import bridge_roles_for_domain

    cache = SnapshotCache()
    cache.update(snapshot(session='z-old'), received_at=1)
    cache.update(snapshot(session='a-new'), received_at=2)
    states = cache.active(now=3)
    states.sort(key=lambda state: state.session_id)
    monkeypatch.setattr(discovery, 'collect_bridge_roles', lambda states: states)
    node = SimpleNamespace(context=SimpleNamespace(get_domain_id=lambda: 7))
    assert [state.session_id for state in bridge_roles_for_domain(states, node)] == ['a-new']


def test_v2_gossip_has_a_separate_wire_topic():
    from ros2hermos_discovery_agent.observation.protocol import GOSSIP_TOPIC

    assert GOSSIP_TOPIC == '/_hermos_discovery_v2'


def test_bridge_routing_rejects_lists_that_lost_receipt_times():
    from ros2hermos.observation.cli import bridge_roles_for_domain

    cache = SnapshotCache()
    cache.update(snapshot(session='z-old'), received_at=1)
    cache.update(snapshot(session='a-new'), received_at=2)
    states = sorted(cache.active(now=3), key=lambda state: state.session_id)
    node = SimpleNamespace(context=SimpleNamespace(get_domain_id=lambda: 7))
    with pytest.raises(ValueError, match='receipt times'):
        bridge_roles_for_domain(states, node)


def test_routing_prefers_bridge_writer_over_more_recent_observer():
    from ros2hermos_discovery_agent.observation.protocol import latest_per_route

    bridge = snapshot(session='bridge', role=1)
    observer = snapshot(session='observer', role=2)
    assert list(latest_per_route([(bridge, 1), (observer, 2)]).values()) == [bridge]


@pytest.mark.parametrize('bridge_index', [0, 1, 2])
def test_lower_priority_writers_need_no_receipt_times(bridge_index):
    from ros2hermos_discovery_agent.observation.protocol import latest_per_route

    bridge = snapshot(session='bridge', role=1)
    states = [(snapshot(session='observer-a', role=2), None),
              (snapshot(session='observer-b', role=2), None)]
    states.insert(bridge_index, (bridge, None))
    assert list(latest_per_route(states).values()) == [bridge]


def test_default_cli_chooses_bridge_but_explicit_session_selects_observer():
    from ros2hermos.observation.cli import scopes_for_args

    cache = SnapshotCache()
    cache.update(snapshot(session='bridge', role=1), received_at=1)
    cache.update(snapshot(session='observer', role=2), received_at=2)
    node = SimpleNamespace(context=SimpleNamespace(get_domain_id=lambda: 7))
    scopes = scopes_for_args(cache.active(now=3), node, SimpleNamespace())
    assert [scope.session_id for scope, _ in scopes] == ['bridge']
    selected = scopes_for_args(
        cache.active(now=3), node, SimpleNamespace(session_id='observer'))
    assert [scope.session_id for scope, _ in selected] == ['observer']


def test_preferred_writer_removal_does_not_revive_observers_stale_topic():
    from ros2hermos.observation.cli import scopes_for_args

    cache = SnapshotCache()
    cache.update(snapshot(session='observer', role=2), received_at=2)
    cache.update(snapshot(session='bridge', role=1, topics=[]), received_at=1)
    node = SimpleNamespace(context=SimpleNamespace(get_domain_id=lambda: 7))
    assert scopes_for_args(
        cache.active(now=3), node, SimpleNamespace(), topic_name='/same') == []


def test_observer_becomes_preferred_after_bridge_lease_expires():
    from ros2hermos.observation.scope import preferred_snapshots

    cache = SnapshotCache()
    cache.update(snapshot(session='bridge', role=1), received_at=1)
    cache.update(snapshot(session='observer', role=2), received_at=5)
    chosen = preferred_snapshots(cache.active(now=31), domain_id=7)
    assert [state.session_id for state in chosen] == ['observer']


@pytest.mark.parametrize('role', [0, 3, 4])
def test_unknown_writer_role_is_not_accepted(role):
    with pytest.raises(ValueError, match='writer role'):
        SnapshotCache().update(snapshot(role=role), received_at=0)


def test_different_devices_keep_independent_inventories_even_in_the_same_session():
    from ros2hermos.observation.cli import scopes_for_args

    cache = SnapshotCache()
    cache.update(snapshot(role=1, device='240:0', topics=[]), received_at=1)
    cache.update(snapshot(role=2, device='241:0'), received_at=2)
    node = SimpleNamespace(context=SimpleNamespace(get_domain_id=lambda: 7))
    states = cache.active(now=3)
    assert len(states) == 2
    assert len(scopes_for_args(states, node, SimpleNamespace())) == 2
    chosen = scopes_for_args(states, node, SimpleNamespace(device_id='241:0'), topic_name='/same')
    assert len(chosen) == 1
    assert chosen[0][0].device_id == '241:0'
    assert 'device=241:0' in scope_label(chosen[0][0])


@pytest.mark.parametrize('device', ['', '/dev/hermos', '0240:0', '-1:0'])
def test_missing_or_noncanonical_device_identity_is_rejected(device):
    with pytest.raises(ValueError, match='device'):
        SnapshotCache().update(snapshot(device=device), received_at=0)


def test_device_aliases_share_identity_and_different_devices_do_not(tmp_path, monkeypatch):
    from ros2hermos_discovery_agent.observation.protocol import local_device_id

    alias = tmp_path / 'device-alias'
    alias.symlink_to('/dev/null')
    monkeypatch.setenv('HERMOS_DEVICE_PATH', str(alias))
    assert local_device_id() == '1:3'
    monkeypatch.setenv('HERMOS_DEVICE_PATH', '/dev/null')
    assert local_device_id() == '1:3'
    monkeypatch.setenv('HERMOS_DEVICE_PATH', '/dev/zero')
    assert local_device_id() == '1:5'


def test_unavailable_or_regular_file_cannot_supply_a_device_identity(tmp_path, monkeypatch):
    from ros2hermos_discovery_agent.observation.protocol import local_device_id

    path = tmp_path / 'not-a-device'
    monkeypatch.setenv('HERMOS_DEVICE_PATH', str(path))
    with pytest.raises(FileNotFoundError):
        local_device_id()
    path.touch()
    with pytest.raises(OSError, match='not a character device'):
        local_device_id()
