"""Keep the userspace ioctl declarations identical to their kmod source of truth.

The expected inventories are explicit so deleting a mirrored definition cannot
silently shrink the comparison. This checks source layout/command drift, not
cross-architecture packing or the ioctl's runtime semantics.
"""

from pathlib import Path
import re

import pytest


ROOT = Path(__file__).resolve().parents[3]
KERNEL = ROOT / 'hermos_kmod/hermos.h'
MIRRORS = {
    'hermoslib': (
        'src/hermoslib/include/hermos/hermos_ioctl.hpp',
        '''publisher_shm_info name_info ioctl_get_version_args ioctl_add_process_args
        ioctl_add_subscriber_args ioctl_add_publisher_args ioctl_update_entry_args
        ioctl_receive_msg_args ioctl_publish_msg_args ioctl_take_msg_args
        ioctl_get_subscriber_num_args ioctl_get_publisher_num_args exit_subscription_mq_info
        ioctl_get_exit_process_args topic_info_ret ioctl_topic_info_args
        ioctl_get_subscriber_qos_args ioctl_get_publisher_qos_args ioctl_remove_subscriber_args
        ioctl_remove_publisher_args ioctl_add_bridge_args ioctl_remove_bridge_args
        ioctl_check_and_request_bridge_shutdown_args ioctl_add_unlink_daemon_args
        ioctl_unlink_daemon_should_exit_args ioctl_set_ros2_subscriber_num_args
        ioctl_set_ros2_publisher_num_args''',
        '''GET_VERSION ADD_PROCESS ADD_SUBSCRIBER ADD_PUBLISHER RELEASE_SUB_REF PUBLISH_MSG
        RECEIVE_MSG TAKE_MSG GET_SUBSCRIBER_NUM GET_EXIT_PROCESS GET_SUBSCRIBER_QOS
        GET_PUBLISHER_QOS ADD_BRIDGE REMOVE_BRIDGE GET_PUBLISHER_NUM REMOVE_SUBSCRIBER
        REMOVE_PUBLISHER CHECK_AND_REQUEST_BRIDGE_SHUTDOWN GET_TOPIC_SUBSCRIBER_INFO
        SET_ROS2_SUBSCRIBER_NUM SET_ROS2_PUBLISHER_NUM NOTIFY_BRIDGE_SHUTDOWN ADD_UNLINK_DAEMON
        UNLINK_DAEMON_SHOULD_EXIT OPEN_SUBSCRIBER_NOTIFY_FD''',
        '''MAX_RECEIVE_NUM MAX_RELEASE_NUM MAX_SUBSCRIPTION_NUM_PER_PROCESS''',
    ),
    'wrapper': (
        'src/hermos_ioctl_wrapper/src/hermos_ioctl.hpp',
        '''name_info ioctl_topic_list_args ioctl_node_info_args topic_info_ret
        ioctl_topic_info_args ioctl_get_version_args ioctl_add_domain_bridge_args
        ioctl_discovery_agent_should_exit_args ioctl_add_discovery_agent_args
        ioctl_discovery_agent_exists_args endpoint_stats_ret ioctl_endpoint_stats_args''',
        '''GET_VERSION DISCOVERY_AGENT_SHOULD_EXIT ADD_DISCOVERY_AGENT DISCOVERY_AGENT_EXISTS
        GET_TOPIC_LIST GET_TOPIC_SUBSCRIBER_INFO GET_TOPIC_PUBLISHER_INFO
        GET_NODE_SUBSCRIBER_TOPICS GET_NODE_PUBLISHER_TOPICS ADD_DOMAIN_BRIDGE
        GET_ENDPOINT_STATS''',
        '''MAX_TOPIC_NUM HERMOS_ENDPOINT_STATS_ABI_VERSION HERMOS_STATS_PUBLISHED
        HERMOS_STATS_RECEIVED HERMOS_STATS_HISTORY_DROP HERMOS_STATS_QUEUED HERMOS_STATS_RETAINED''',
    ),
}
COMMON_CONSTANTS = '''MAX_PUBLISHER_NUM MAX_TOPIC_LOCAL_ID MAX_SUBSCRIBER_NUM
NODE_NAME_BUFFER_SIZE TOPIC_NAME_BUFFER_SIZE VERSION_BUFFER_LEN'''


def _without_comments(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.DOTALL).replace('\\\n', '')


def _normalize(text):
    return re.sub(r'\s+|\b__user\b', '', text)


def _records(text):
    # The UAPI uses column-zero closing braces; anonymous members are indented.
    matches = list(re.finditer(
        r'^(?:struct|union)\s+(\w+)\s*\{.*?^};', text, flags=re.MULTILINE | re.DOTALL))
    assert len(matches) == len({match[1] for match in matches}), 'duplicate UAPI record name'
    return {match[1]: _normalize(match[0]) for match in matches}


def _macros(text):
    return {name: _normalize(body) for name, body in
            re.findall(r'^#define\s+(\w+)\s+([^\n]+)', text, flags=re.MULTILINE)}


def _assert_mirror_matches(kernel_text, mirror_text, spec):
    _, record_names, command_names, extra_constants = spec
    kernel_text, mirror_text = map(_without_comments, (kernel_text, mirror_text))
    kernel_records, mirror_records = map(_records, (kernel_text, mirror_text))
    assert set(mirror_records) == set(record_names.split())
    for name, body in mirror_records.items():
        assert body == kernel_records[name], f'layout mismatch: {name}'

    kernel_macros, mirror_macros = map(_macros, (kernel_text, mirror_text))
    commands = {f'HERMOS_{name}_CMD' for name in command_names.split()}
    assert {name for name in mirror_macros if name.endswith('_CMD')} == commands
    for name in commands | set((COMMON_CONSTANTS + ' ' + extra_constants).split()):
        assert mirror_macros[name] == kernel_macros[name], f'macro mismatch: {name}'


@pytest.mark.parametrize('mirror', MIRRORS)
def test_userspace_ioctl_mirror_matches_kernel(mirror):
    spec = MIRRORS[mirror]
    _assert_mirror_matches(KERNEL.read_text(), (ROOT / spec[0]).read_text(), spec)


def test_topic_local_id_alias_is_signed_32_bit_in_both_headers():
    kernel = _without_comments(KERNEL.read_text())
    library = _without_comments((ROOT / MIRRORS['hermoslib'][0]).read_text())
    assert re.search(r'typedef\s+int32_t\s+topic_local_id_t\s*;', kernel)
    assert re.search(r'using\s+topic_local_id_t\s*=\s*int32_t\s*;', library)
