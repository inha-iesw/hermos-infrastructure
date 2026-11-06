"""Exercise the installed rclpy callback contract with a real DDS snapshot."""

import os
import subprocess
import sys
import textwrap


def test_transient_local_metadata_late_join_uses_actual_rclpy_callback():
    # A subprocess owns its default ROS context and cannot disturb other tests.
    script = textwrap.dedent('''
        import uuid
        import rclpy
        from ros2hermos.discovery import collect_announcements, GOSSIP_TOPIC
        from ros2hermos_discovery_agent.observation.protocol import writer_qos
        from ros2hermos_discovery_msgs.msg import HermosDaemonState

        domain = 201
        session = uuid.uuid4().hex
        rclpy.init(args=[], domain_id=domain)
        publisher_node = rclpy.create_node('hermos_cli_publisher_' + session)
        query_node = rclpy.create_node('hermos_cli_query_' + session)
        publisher = publisher_node.create_publisher(HermosDaemonState, GOSSIP_TOPIC, writer_qos())
        try:
            publisher.publish(HermosDaemonState(device_id='240:0', writer_role=1,
                schema_version=2, host_uuid='host-test', ipc_ns_inode=42,
                domain_id=domain, session_id=session, lease_duration_ns=30_000_000_000))
            received = collect_announcements(query_node, timeout_sec=3.0)
            assert any(message.session_id == session for message in received), received
        finally:
            query_node.destroy_node()
            publisher_node.destroy_node()
            rclpy.try_shutdown()
    ''')
    result = subprocess.run(
        [sys.executable, '-c', script], capture_output=True, text=True,
        env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1', 'ROS_LOCALHOST_ONLY': '1'},
        timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
