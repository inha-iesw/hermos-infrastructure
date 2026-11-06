"""Fast DDS must deliver remote metadata despite an unsynchronized wall clock."""

import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import uuid
from unittest.mock import Mock

from rclpy.context import Context
from rclpy.executors import SingleThreadedExecutor

from ros2hermos_discovery_agent import agent


SCENARIO = Path(__file__).with_name('scenarios') / 'clock_skew'


def test_default_agent_receives_metadata_from_writer_sixty_seconds_behind(monkeypatch, tmp_path):
    shim = tmp_path / 'clock_shift.so'
    subprocess.run(
        ['cc', '-shared', '-fPIC', '-O2', '-o', str(shim),
         str(SCENARIO / 'shift.c'), '-ldl'], check=True, capture_output=True, text=True)
    monkeypatch.setenv('ROS_LOCALHOST_ONLY', '1')
    monkeypatch.setenv('RMW_IMPLEMENTATION', 'rmw_fastrtps_cpp')
    monkeypatch.setattr(agent, 'load_query_library', lambda **kwargs: object())
    monkeypatch.setattr(agent, 'read_local_topics', lambda *args: [])
    domain = 173
    session = uuid.uuid4().hex
    context = Context()
    context.init(args=[], domain_id=domain)
    observer = executor = publisher = None
    output = ''
    try:
        # Exercise the actual default-mode agent subscription and callback;
        # no synthetic subscription substitutes for the production QoS path.
        observer = agent.DiscoveryAgent(
            registry=Mock(), context=context, exit_when_idle=False,
            observation_only=False, session_id='receiver-' + session)
        executor = SingleThreadedExecutor(context=context)
        executor.add_node(observer)
        publisher = subprocess.Popen(
            [sys.executable, str(SCENARIO / 'publisher.py'), str(domain), session],
            env={**os.environ, 'LD_PRELOAD': str(shim), 'PYTHONDONTWRITEBYTECODE': '1'},
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 8.0
        received = None
        while time.monotonic() < deadline:
            executor.spin_once(timeout_sec=0.05)
            received = next((message for message, _ in observer.remote_states.values()
                             if message.session_id == session), None)
            if received is not None or publisher.poll() is not None:
                break
        if publisher.poll() is not None:
            output, _ = publisher.communicate(timeout=2)
        assert received is not None, (
            f'default agent discarded shifted-clock metadata; writer output: {output}')
        # This also proves the shim was active, rather than accidentally testing
        # two synchronized writers. Only the child has LD_PRELOAD set.
        skew_seconds = (time.time_ns() - received.snapshot_time_ns) / 1e9
        assert 55 < skew_seconds < 75, skew_seconds
    finally:
        if publisher is not None and publisher.poll() is None:
            publisher.send_signal(signal.SIGINT)
            try:
                publisher.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                publisher.kill()
                publisher.communicate(timeout=5)
        if executor is not None:
            executor.shutdown()
        if observer is not None:
            observer.destroy_node()
        context.shutdown()
