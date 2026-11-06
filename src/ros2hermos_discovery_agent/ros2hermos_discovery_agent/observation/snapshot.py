"""Read native endpoint counters without taking a payload reference."""

import ctypes
import errno

COUNTERS = (
    (1, 'published_total'), (2, 'received_total'), (4, 'dropped_history_total'),
    (8, 'queued_entries'), (16, 'retained_entries'),
)
INITIAL_CAPACITY = 64


class EndpointStatsRet(ctypes.Structure):
    """Mirror ABI v1 from hermos_kmod/hermos.h (336 bytes)."""

    _fields_ = [
        ('node_name', ctypes.c_char * 256),
        ('pid', ctypes.c_int32), ('endpoint_id', ctypes.c_int32),
        ('domain_id', ctypes.c_uint32), ('qos_depth', ctypes.c_uint32),
        ('is_publisher', ctypes.c_uint8), ('is_bridge', ctypes.c_uint8),
        ('is_take_sub', ctypes.c_uint8), ('qos_is_transient_local', ctypes.c_uint8),
        ('qos_is_reliable', ctypes.c_uint8), ('reserved', ctypes.c_uint8 * 3),
        ('published_total', ctypes.c_uint64), ('received_total', ctypes.c_uint64),
        ('dropped_history_total', ctypes.c_uint64), ('queued_entries', ctypes.c_uint64),
        ('retained_entries', ctypes.c_uint64), ('sample_time_ns', ctypes.c_uint64),
        ('supported_fields', ctypes.c_uint32), ('reserved2', ctypes.c_uint32),
    ]


def configure_endpoint_stats(library):
    """Bind the optional read-only ioctl without making older wrappers unusable."""
    getter = getattr(library, 'hermos_query_endpoint_stats_v1', None)
    if getter is not None:
        getter.argtypes = [
            ctypes.c_char_p, ctypes.c_uint32, ctypes.c_bool,
            ctypes.POINTER(EndpointStatsRet), ctypes.POINTER(ctypes.c_uint32),
        ]
        getter.restype = ctypes.c_int


def read_endpoint_stats(library, topic_name, domain_id, is_publisher):
    """Return availability and native rows; unsupported fields remain unknown.

    Counters describe kernel publications, reference acquisitions, and unread
    history loss. They do not count callbacks, application drops, or CUDA
    completions. A small buffer grows once to the kernel's reported capacity.
    """
    getter = getattr(library, 'hermos_query_endpoint_stats_v1', None)
    if getter is None:
        return 'unsupported', []
    capacity = ctypes.c_uint32(INITIAL_CAPACITY)
    for _ in range(2):
        rows = (EndpointStatsRet * capacity.value)()
        count = getter(topic_name.encode('utf-8'), domain_id, is_publisher,
                       rows, ctypes.byref(capacity))
        if count != -errno.ENOBUFS or capacity.value <= len(rows):
            break
    if count in (-errno.ENOTTY, -errno.EPROTO):
        return 'unsupported', []
    if count in (-errno.EACCES, -errno.EPERM):
        return 'permission_denied', []
    if count < 0 or count > len(rows):
        return 'unavailable', []
    return 'available', list(rows[:count])
