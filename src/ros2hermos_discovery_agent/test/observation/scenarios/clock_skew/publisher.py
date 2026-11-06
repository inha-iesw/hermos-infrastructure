"""Publish metadata using a process-local clock shift supplied by the test."""

import sys
import time

import rclpy

from ros2hermos_discovery_agent.observation.protocol import (
    GOSSIP_TOPIC, make_snapshot, writer_qos,
)
from ros2hermos_discovery_msgs.msg import HermosDaemonState


def main():
    domain, session = int(sys.argv[1]), sys.argv[2]
    rclpy.init(args=[], domain_id=domain)
    node = rclpy.create_node('metadata_shifted_writer_' + session)
    publisher = node.create_publisher(HermosDaemonState, GOSSIP_TOPIC, writer_qos())
    try:
        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline:
            publisher.publish(make_snapshot(
                host_uuid='clock-skew-test-host', hostname='shifted-publisher',
                ipc_ns_inode=7, domain_id=domain, device_id='240:0', session_id=session, topics=[],
                writer_role=HermosDaemonState.WRITER_BRIDGE))
            rclpy.spin_once(node, timeout_sec=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
