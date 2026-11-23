"""Installer trust checks run before any privileged change; sudo is always mocked."""

import os
from pathlib import Path
import subprocess

import pytest


SCRIPT = Path(__file__).resolve().parents[2] / 'setup_thread_configurator.bash'


def fixture(tmp_path, *, fake_trusted_stat=False):
    distro = os.environ.get('ROS_DISTRO', 'humble')
    if not Path(f'/opt/ros/{distro}/lib').is_dir():
        pytest.skip(f'ROS library directory is not installed for {distro}')
    commands = tmp_path / 'bin'
    commands.mkdir()
    prefix = tmp_path / 'install'
    executable = prefix / 'lib/hermos_cie_thread_configurator/thread_configurator_node'
    executable.parent.mkdir(parents=True)
    executable.write_text('test binary')
    log = tmp_path / 'sudo-calls'
    scripts = {
        'ros2': '#!/bin/bash\nprintf "%s\\n" "$TEST_PREFIX"\n',
        'dpkg-architecture': '#!/bin/bash\necho x86_64-linux-gnu\n',
        'getcap': '#!/bin/bash\nexit 0\n',
        'setcap': '#!/bin/bash\nexit 0\n',
        'ldconfig': '#!/bin/bash\nexit 0\n',
        'sudo': '#!/bin/bash\nprintf "%s\\n" "$*" >> "$TEST_SUDO_LOG"\n'
                'if [[ "$1" == tee ]]; then cat > "$TEST_CONF_CONTENT"; fi\n',
    }
    if fake_trusted_stat:
        scripts['stat'] = (
            '#!/bin/bash\n'
            'if [[ "${@: -1}" == "$TEST_UNTRUSTED_PATH" ]]; then\n'
            '  echo "$TEST_UNTRUSTED_STAT"\nelse\n  echo "0 755"\nfi\n')
    for name, contents in scripts.items():
        command = commands / name
        command.write_text(contents)
        command.chmod(0o755)
    env = {**os.environ, 'PATH': f'{commands}:/usr/bin:/bin', 'ROS_DISTRO': distro,
           'TEST_PREFIX': str(prefix), 'TEST_SUDO_LOG': str(log), 'TEST_UNTRUSTED_PATH': '',
           'TEST_UNTRUSTED_STAT': '1000 775', 'TEST_CONF_CONTENT': str(tmp_path / 'loader-content')}
    return prefix, log, env


def run(env):
    return subprocess.run(['bash', str(SCRIPT)], env=env, text=True, capture_output=True)


def test_user_writable_install_is_rejected_before_sudo(tmp_path):
    _, log, env = fixture(tmp_path)
    result = run(env)
    assert result.returncode != 0
    assert 'untrusted' in result.stdout.lower()
    assert not log.exists()


def test_library_symlink_target_must_also_be_trusted(tmp_path):
    prefix, log, env = fixture(tmp_path, fake_trusted_stat=True)
    target = tmp_path / 'untrusted-library.so'
    target.write_text('library')
    (prefix / 'lib/libtest.so').symlink_to(target)
    env['TEST_UNTRUSTED_PATH'] = str(target)
    result = run(env)
    assert result.returncode != 0
    assert 'untrusted' in result.stdout.lower()
    assert not log.exists()


def test_validated_install_reaches_only_mocked_privileged_commands(tmp_path):
    _, log, env = fixture(tmp_path, fake_trusted_stat=True)
    result = run(env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'setcap cap_sys_nice=eip' in log.read_text()
    assert 'tee /etc/ld.so.conf.d/hermos-cie.conf' in log.read_text()


def test_loader_configuration_uses_canonical_library_paths(tmp_path):
    prefix, _, env = fixture(tmp_path, fake_trusted_stat=True)
    alias = tmp_path / 'alias'
    alias.symlink_to(prefix, target_is_directory=True)
    env['TEST_PREFIX'] = str(alias)
    result = run(env)
    assert result.returncode == 0, result.stdout + result.stderr
    contents = Path(env['TEST_CONF_CONTENT']).read_text()
    assert str(alias / 'lib') not in contents
    assert str(prefix / 'lib') in contents


@pytest.mark.parametrize(('relative_path', 'mode'), [('', '775'), ('lib/hermos_cie_thread_configurator', '1777')])
def test_root_owned_writable_component_is_rejected_before_sudo(tmp_path, relative_path, mode):
    prefix, log, env = fixture(tmp_path, fake_trusted_stat=True)
    env['TEST_UNTRUSTED_PATH'] = str(prefix / relative_path)
    env['TEST_UNTRUSTED_STAT'] = f'0 {mode}'
    result = run(env)
    assert result.returncode != 0
    assert f'untrusted installation path: {prefix / relative_path}' in result.stdout
    assert not log.exists()


def test_missing_ros_library_directory_reports_path_before_sudo(tmp_path):
    _, log, env = fixture(tmp_path, fake_trusted_stat=True)
    env['ROS_DISTRO'] = 'hermos-test-missing-distro'
    result = run(env)
    assert result.returncode != 0
    assert 'missing library path: /opt/ros/hermos-test-missing-distro/lib' in result.stdout
    assert not log.exists()
