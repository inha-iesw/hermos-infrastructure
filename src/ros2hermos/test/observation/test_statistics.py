"""Native statistics keep their kernel meanings and unavailable values."""

from types import SimpleNamespace

from ros2hermos.observation.stats import statistics_lines


def endpoint(**values):
    fields = dict(
        node_name='/listener', pid=123, endpoint_id=4,
        statistics_status='available', supported_fields=30,
        published_total=0, received_total=5, dropped_history_total=2,
        queued_entries=3, retained_entries=1, sample_time_ns=1000,
    )
    fields.update(values)
    return SimpleNamespace(**fields)


def test_receiver_counts_are_labeled_as_kernel_acquisitions_and_history_loss():
    text = '\n'.join(statistics_lines(endpoint(), is_publisher=False))
    assert 'received_total (kernel reference acquisitions): 5' in text
    assert 'dropped_history_total (unread history loss): 2' in text
    assert 'queued_entries (unread resident entries before QoS filtering): 3' in text
    assert 'retained_entries (held kernel references): 1' in text
    assert 'sample_time_ns (source CLOCK_MONOTONIC): 1000' in text
    assert 'GPU' not in text


def test_real_zero_counter_is_distinct_from_unsupported_counter():
    text = '\n'.join(statistics_lines(endpoint(supported_fields=17), is_publisher=True))
    assert 'published_total: 0' in text
    assert 'received_total (kernel reference acquisitions): unsupported' in text
    assert 'retained_entries (publisher resident entries): 1' in text


def test_permission_denied_does_not_print_default_zero_as_measurement():
    text = '\n'.join(statistics_lines(
        endpoint(statistics_status='permission_denied', supported_fields=0),
        is_publisher=False))
    assert 'statistics: permission_denied' in text
    assert 'received_total' not in text
    assert 'sample_time_ns' not in text


def test_legacy_endpoint_reports_unsupported_statistics_and_unknown_pid():
    legacy = SimpleNamespace(node_name='/listener', pid=0)
    text = '\n'.join(statistics_lines(legacy, is_publisher=False))
    assert 'pid=unknown' in text
    assert 'statistics: unsupported' in text
