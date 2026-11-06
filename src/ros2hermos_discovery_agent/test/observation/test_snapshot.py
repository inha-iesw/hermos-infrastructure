"""Read-only native statistics preserve identity, availability and counter meaning."""

import ctypes
import errno
from types import SimpleNamespace

import pytest


def test_stats_schema_matches_native_abi_size():
    from ros2hermos_discovery_agent.observation.snapshot import EndpointStatsRet

    assert ctypes.sizeof(EndpointStatsRet) == 336
    offsets = {
        'node_name': 0,
        'pid': 256,
        'endpoint_id': 260,
        'domain_id': 264,
        'qos_depth': 268,
        'is_publisher': 272,
        'is_bridge': 273,
        'is_take_sub': 274,
        'qos_is_transient_local': 275,
        'qos_is_reliable': 276,
        'reserved': 277,
        'published_total': 280,
        'received_total': 288,
        'dropped_history_total': 296,
        'queued_entries': 304,
        'retained_entries': 312,
        'sample_time_ns': 320,
        'supported_fields': 328,
        'reserved2': 332,
    }
    assert {name: getattr(EndpointStatsRet, name).offset
            for name, _ in EndpointStatsRet._fields_} == offsets


@pytest.mark.parametrize('result,status', [
    (-errno.ENOTTY, 'unsupported'), (-errno.EPROTO, 'unsupported'),
    (-errno.EINVAL, 'unavailable'), (-errno.ENOBUFS, 'unavailable'),
    (-errno.EACCES, 'permission_denied'), (-errno.EPERM, 'permission_denied'),
    (-errno.ENOENT, 'unavailable'),
])
def test_stats_error_is_not_reported_as_zero(result, status):
    from ros2hermos_discovery_agent.observation.snapshot import read_endpoint_stats

    library = SimpleNamespace(hermos_query_endpoint_stats_v1=lambda *args: result)
    assert read_endpoint_stats(library, '/camera', 12, False) == (status, [])


def test_missing_stats_symbol_is_unsupported():
    from ros2hermos_discovery_agent.observation.snapshot import read_endpoint_stats

    assert read_endpoint_stats(object(), '/camera', 12, False) == ('unsupported', [])


def test_negotiated_stats_retry_uses_required_capacity():
    from ros2hermos_discovery_agent.observation.snapshot import read_endpoint_stats

    capacities = []

    def query(topic, domain, publisher, output, capacity):
        capacities.append(capacity._obj.value)
        if len(capacities) == 1:
            capacity._obj.value = 73
            return -errno.ENOBUFS
        output[72].received_total = 19
        return 73

    status, rows = read_endpoint_stats(
        SimpleNamespace(hermos_query_endpoint_stats_v1=query), '/camera', 12, False)
    assert capacities == [64, 73]
    assert status == 'available'
    assert len(rows) == 73 and rows[-1].received_total == 19


def test_negotiated_stats_stops_if_endpoints_keep_growing():
    from ros2hermos_discovery_agent.observation.snapshot import read_endpoint_stats

    capacities = []

    def query(topic, domain, publisher, output, capacity):
        capacities.append(capacity._obj.value)
        capacity._obj.value += 1
        return -errno.ENOBUFS

    assert read_endpoint_stats(
        SimpleNamespace(hermos_query_endpoint_stats_v1=query), '/camera', 12, False
    ) == ('unavailable', [])
    assert capacities == [64, 65]


def test_stats_query_passes_domain_and_copies_exact_identity_and_counters():
    from ros2hermos_discovery_agent.observation.snapshot import read_endpoint_stats

    def query(topic, domain, publisher, output, capacity):
        assert (topic, domain, publisher) == (b'/camera', 12, False)
        assert capacity._obj.value == 64
        output[0].node_name = b'/consumer'
        output[0].pid = 71
        output[0].endpoint_id = 44
        output[0].domain_id = 12
        output[0].received_total = 13
        output[0].dropped_history_total = 2
        output[0].queued_entries = 3
        output[0].retained_entries = 1
        output[0].sample_time_ns = 123456
        output[0].supported_fields = 30
        return 1

    status, rows = read_endpoint_stats(
        SimpleNamespace(hermos_query_endpoint_stats_v1=query), '/camera', 12, False)
    assert status == 'available'
    row, = rows
    assert (row.pid, row.endpoint_id, row.domain_id) == (71, 44, 12)
    assert (row.received_total, row.dropped_history_total) == (13, 2)
    assert (row.queued_entries, row.retained_entries, row.sample_time_ns) == (3, 1, 123456)
    assert row.supported_fields == 30


@pytest.mark.parametrize('code', [errno.EACCES, errno.ENOENT, errno.ENODEV])
def test_topic_query_failure_does_not_become_an_empty_snapshot(code):
    from ros2hermos_discovery_agent.agent import read_local_topics

    def query(count, domains):
        count._obj.value = -code
        return None

    library = SimpleNamespace(get_hermos_topics=query)
    with pytest.raises(OSError) as captured:
        read_local_topics(library)
    assert captured.value.errno == code


@pytest.mark.parametrize('symbol', ['hermos_get_endpoint_stats', 'hermos_get_endpoint_stats_v1'])
def test_nonnegotiating_wrapper_symbol_is_not_called_with_a_v1_buffer(symbol):
    from ros2hermos_discovery_agent.observation.snapshot import read_endpoint_stats

    def old_getter(*args):
        pytest.fail('nonnegotiating statistics ABI was invoked')

    library = SimpleNamespace(**{symbol: old_getter})
    assert read_endpoint_stats(library, '/camera', 12, False) == ('unsupported', [])
