"""Exercise the real C++ writer and Python reader without kernel/GPU setup."""

import os
from pathlib import Path
import select
import subprocess

from ros2hermos_discovery_agent.type_registry import TypeRegistryReader


def test_writer_default_path_is_readable_and_removed_on_exit(tmp_path, monkeypatch):
    monkeypatch.setenv('HERMOS_TMPFS_DIR', str(tmp_path))
    env = dict(os.environ, HERMOS_TMPFS_DIR=str(tmp_path), ROS_DOMAIN_ID='17')
    process = subprocess.Popen(
        [os.environ['HERMOS_REGISTRY_WRITER_PROBE']], env=env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert select.select([process.stdout], [], [], 10)[0], 'writer did not become ready'
        assert int(process.stdout.readline()) == process.pid
        # Do not pass base_dir: this must check both implementations' default path rules.
        reader = TypeRegistryReader(os.stat('/proc/self/ns/ipc').st_ino)
        reader.rebuild()
        assert reader.registered_pids() == [process.pid]
        assert len(reader.table) == 2
        for role, node in [('pub', '/writer'), ('sub', '/reader')]:
            entry = reader.lookup('/registry_contract', role, node,
                                  domain_id=17, pid=process.pid)
            assert entry is not None
            assert entry.type_name == 'std_msgs/msg/String'
            assert entry.process_start_ticks > 0
            assert reader.lookup_type('/registry_contract', role, node,
                                      domain_id=17) == entry.type_name
            assert reader.lookup('/registry_contract', role, node,
                                 domain_id=18, pid=process.pid) is None
        namespace = Path(reader.ns_dir)
        path = namespace / f'{process.pid}.txt'
        for directory in [namespace, namespace.parent]:
            assert directory.stat().st_mode & 0o777 == 0o700
        assert path.stat().st_mode & 0o777 == 0o600
        process.communicate('q', timeout=10)
        assert process.returncode == 0
        assert not path.exists()
        reader.rebuild()
        assert not reader.table
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=10)
