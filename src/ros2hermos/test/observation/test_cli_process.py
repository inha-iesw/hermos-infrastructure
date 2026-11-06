"""Installed CLI regression with DDS graph data and no usable native device."""

import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid
from xmlrpc.client import ServerProxy

import rclpy
from rclpy.context import Context
from std_msgs.msg import String


def test_installed_cli_preserves_dds_results_and_reports_native_failures(tmp_path, monkeypatch):
    ros2 = shutil.which('ros2')
    assert ros2 is not None
    # Own a daemon subprocess in an unused local domain, and shut down only that
    # PID. Existing user daemons and their discovery caches are never touched.
    for domain in range(70, 100):
        with socket.socket() as probe:
            try:
                probe.bind(('127.0.0.1', 11511 + domain))
                break
            except OSError:
                continue
    else:
        raise AssertionError('no unused local ROS domain for CLI regression')
    environment = {
        **os.environ, 'ROS_DOMAIN_ID': str(domain), 'ROS_LOCALHOST_ONLY': '1',
        'PYTHONDONTWRITEBYTECODE': '1',
    }
    monkeypatch.setenv('ROS_LOCALHOST_ONLY', '1')
    daemon_log = tmp_path / 'daemon.log'
    context = Context()
    node = None
    with daemon_log.open('w') as output:
        daemon = subprocess.Popen(
            [sys.executable, '-u', '-c', 'from ros2cli.daemon import main; main()',
             '--rmw-implementation', rclpy.get_rmw_implementation_identifier(),
             '--ros-domain-id', str(domain), '--timeout', '60'],
            env=environment, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while 'Serving XML-RPC' not in daemon_log.read_text():
                assert daemon.poll() is None, daemon_log.read_text()
                assert time.monotonic() < deadline, daemon_log.read_text()
                time.sleep(0.05)
            rclpy.init(args=[], context=context, domain_id=domain)
            suffix = uuid.uuid4().hex
            node_name = 'e3_cli_dds_' + suffix
            topic_name = '/e3_cli_dds_' + suffix
            node = rclpy.create_node(node_name, context=context)
            publisher = node.create_publisher(String, topic_name, 1)
            publisher.publish(String(data='DDS-only CLI regression'))
            with ServerProxy(f'http://127.0.0.1:{11511 + domain}/ros2cli/') as proxy:
                deadline = time.monotonic() + 10
                while not any(name == topic_name for name, _ in proxy.get_topic_names_and_types()):
                    assert time.monotonic() < deadline, daemon_log.read_text()[-4000:]
                    time.sleep(0.05)

            denied = tmp_path / 'denied-device'
            denied.touch(mode=0)
            failures = [(tmp_path / 'missing-device', 'unavailable')]
            if os.geteuid() != 0:
                failures.append((denied, 'permission_denied'))
            for device, status in failures:
                command_env = {**environment, 'HERMOS_DEVICE_PATH': str(device)}
                checks = [
                    (['topic', 'list_hermos'], 0, topic_name),
                    (['topic', 'info_hermos', topic_name], 0, 'ROS 2 Publisher count: 1'),
                    (['node', 'list_hermos'], 0, '/' + node_name),
                    (['node', 'info_hermos', '/' + node_name], 0, topic_name),
                    (['topic', 'list_hermos', '--local'], 1, status),
                    (['topic', 'stats_hermos', topic_name], 1, status),
                    *[(['topic', verb, '/missing_' + suffix], 1, 'Could not resolve message type')
                      for verb in ('echo_hermos', 'hz_hermos', 'delay_hermos')],
                ]
                for arguments, expected_code, expected_text in checks:
                    result = subprocess.run(
                        [ros2, *arguments, '--gossip-timeout', '0'],
                        env=command_env, capture_output=True, text=True, timeout=15)
                    detail = f'{arguments}: {result.stdout}\n{result.stderr}'
                    assert result.returncode == expected_code, detail
                    assert expected_text in result.stdout + result.stderr, detail
                    assert 'Traceback' not in result.stdout + result.stderr, detail
                    if expected_code == 0:
                        assert f'Local HERM0S metadata {status}' in result.stderr, detail
        finally:
            if node is not None:
                node.destroy_node()
            rclpy.try_shutdown(context=context)
            if daemon.poll() is None:
                daemon.send_signal(signal.SIGINT)
            try:
                daemon.wait(timeout=10)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=5)
                raise AssertionError('owned CLI test daemon did not stop normally')
