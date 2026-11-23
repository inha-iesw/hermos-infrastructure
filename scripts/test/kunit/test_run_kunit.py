"""KUnit orchestration tests use a private fake backend and mocked privileges."""

import os
from pathlib import Path
import shutil
import subprocess

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / 'run_kunit.bash'
RESULT_PATH = '/sys/kernel/debug/kunit/hermos_test_suite/results'


def ktap(cases=('ok', 'ok'), *, statistics=True):
    passed = sum(value == 'ok' for value in cases)
    skipped = sum('SKIP' in value for value in cases)
    failed = len(cases) - passed - skipped
    lines = ['KTAP version 1', '1..1', '    KTAP version 1',
             '    # Subtest: hermos_test_suite', f'    1..{len(cases)}']
    for index, status in enumerate(cases, 1):
        result, _, directive = status.partition(' # ')
        lines.append(f'    {result} {index} case_{index}' + (f' # {directive}' if directive else ''))
    if statistics:
        counts = f'pass:{passed} fail:{failed} skip:{skipped} total:{len(cases)}'
        lines.extend([f'# hermos_test_suite: {counts}', f'# Totals: {counts}'])
    lines.append(f'{"not ok" if failed else "ok"} 1 hermos_test_suite')
    return '\n'.join(lines) + '\n'


@pytest.mark.parametrize('results,success', [
    ('', False),
    (ktap(), True),
    (ktap(statistics=False), True),
    (ktap(('ok', 'ok # SKIP unavailable')), True),
    (ktap(('ok', 'not ok # SKIP unavailable')), True),
    (ktap(('not ok', 'ok')), False),
    (ktap(('ok # SKIP unavailable',)), False),
    (ktap(()), False),
    (ktap().replace('    ok 2 case_2\n', ''), False),
    (ktap().replace('ok 2 case_2', 'ok 1 case_2'), False),
    (ktap().replace('1..2', '1..3'), False),
    (ktap().replace('pass:2', 'pass:3'), False),
    (ktap().replace('fail:0', 'fail:invalid'), False),
    (ktap().replace('hermos_test_suite', 'unrelated_suite'), False),
    (ktap().removesuffix('ok 1 hermos_test_suite\n'), False),
    (ktap() + 'ok 1 unrelated_suite\n', False),
    (ktap().replace('ok 1 hermos_test_suite\n', 'not ok 1 hermos_test_suite\n'), False),
    (ktap().replace('KTAP version 1', 'not a KTAP header', 1), False),
    (ktap().replace('1..1', '1..2', 1), False),
    (ktap().replace('ok 1 case_1', 'ok 1 case_1 # TODO broken'), False),
])
def test_results_require_complete_nonempty_successful_suite(tmp_path, results, success):
    _, env = fixture(tmp_path)
    Path(env['TEST_RESULTS']).write_text(results)
    result = subprocess.run(
        ['bash', '-c', 'source "$RUN_SCRIPT"; validate_results "$TEST_RESULTS"', 'test'],
        env=env, text=True,
        capture_output=True)
    assert (result.returncode == 0) is success


def fixture(tmp_path):
    backend = tmp_path / 'backend'
    script = backend / 'scripts/test/run_kunit.bash'
    script.parent.mkdir(parents=True)
    shutil.copyfile(SCRIPT, script)
    (backend / 'hermos_kmod').mkdir()
    commands = tmp_path / 'commands'
    commands.mkdir()
    config = tmp_path / 'config'
    config.write_text('CONFIG_KUNIT=y\nCONFIG_KUNIT_DEBUGFS=y\nCONFIG_GCOV_KERNEL=y\n')
    (tmp_path / 'results').write_text(ktap())
    (tmp_path / 'before').write_text('existing log\n')
    (tmp_path / 'after').write_text('existing log\n# Totals: pass:2 fail:0 skip:0 total:2\n')
    calls = tmp_path / 'calls'
    programs = {
        'sudo': r'''#!/bin/bash
printf '%s\n' "$*" >> "$TEST_CALLS"
case "$1" in
  insmod) [[ "$TEST_INSFAIL" == 0 ]] ;;
  rmmod) exit 0 ;;
  test)
    case "$*" in
      'test -d /sys/kernel/debug/kunit') [[ "$TEST_DEBUGFS" == 1 ]] ;;
      'test ! -e /sys/kernel/debug/kunit/hermos_test_suite/results') [[ "$TEST_STALE" == 0 ]] ;;
      *) exit 98 ;;
    esac ;;
  cat)
    [[ "$2" == '/sys/kernel/debug/kunit/hermos_test_suite/results' ]] || exit 98
    [[ "$TEST_READFAIL" == 0 ]] || exit 1
    cat "$TEST_RESULTS" ;;
  dmesg)
    if [[ ! -f "$TEST_FIRST_DMESG" ]]; then
      touch "$TEST_FIRST_DMESG"
      cat "$TEST_BEFORE"
    else
      cat "$TEST_AFTER"
    fi ;;
  lcov) exit 0 ;;
  *) exit 98 ;;
esac
''',
        'make': '#!/bin/bash\nprintf "make %s\\n" "$*" >> "$TEST_CALLS"\n[[ "$TEST_MAKEFAIL" == 0 ]]\n',
        'lsmod': '#!/bin/bash\nprintf "%s\\n" "$TEST_LOADED"\n',
        'sleep': '#!/bin/bash\nexit 0\n',
        'genhtml': '#!/bin/bash\nexit 0\n',
    }
    for name, contents in programs.items():
        command = commands / name
        command.write_text(contents)
        command.chmod(0o755)
    env = {**os.environ, 'PATH': f'{commands}:/usr/bin:/bin', 'RUN_SCRIPT': str(script),
           'TEST_CONFIG': str(config), 'TEST_CALLS': str(calls), 'TMPDIR': str(tmp_path),
           'TEST_FIRST_DMESG': str(tmp_path / 'first-dmesg'),
           'TEST_BEFORE': str(tmp_path / 'before'), 'TEST_AFTER': str(tmp_path / 'after'),
           'TEST_RESULTS': str(tmp_path / 'results'), 'TEST_STALE': '0',
           'TEST_READFAIL': '0', 'TEST_DEBUGFS': '1',
           'TEST_INSFAIL': '0', 'TEST_MAKEFAIL': '0', 'TEST_LOADED': ''}
    return calls, env


@pytest.mark.parametrize('change,value,success,unloads', [
    ('TEST_INSFAIL', '0', True, 1),
    ('TEST_INSFAIL', '1', False, 0),
    ('TEST_MAKEFAIL', '1', False, 0),
    ('TEST_READFAIL', '1', False, 1),
    ('TEST_STALE', '1', False, 0),
    ('TEST_DEBUGFS', '0', False, 0),
    ('TEST_LOADED', 'hermos_kunit 1000 0', False, 0),
])
def test_only_own_loaded_module_is_unloaded(tmp_path, change, value, success, unloads):
    calls, env = fixture(tmp_path)
    env[change] = value
    result = subprocess.run(
        ['bash', '-c', 'source "$RUN_SCRIPT"; '
         'read_kernel_config() { CONFIG_FILE="$TEST_CONFIG"; }; main', 'test'],
        env=env, text=True, capture_output=True)
    assert (result.returncode == 0) is success, result.stdout + result.stderr
    lines = calls.read_text().splitlines() if calls.exists() else []
    assert lines.count('rmmod hermos_kunit') == unloads
    assert not list(tmp_path.glob('tmp.*')), 'private temporary directory was not cleaned'
    if not success:
        assert not any(line.startswith('lcov ') for line in lines)
    else:
        assert lines.index('cat ' + RESULT_PATH) < lines.index('rmmod hermos_kunit')


def test_previous_successful_totals_do_not_count_for_this_run(tmp_path):
    calls, env = fixture(tmp_path)
    Path(env['TEST_BEFORE']).write_text('# Totals: pass:222 fail:0\n')
    Path(env['TEST_AFTER']).write_text('# Totals: pass:222 fail:0\n')
    Path(env['TEST_RESULTS']).write_text(ktap().removesuffix('ok 1 hermos_test_suite\n'))
    result = subprocess.run(
        ['bash', '-c', 'source "$RUN_SCRIPT"; '
         'read_kernel_config() { CONFIG_FILE="$TEST_CONFIG"; }; main', 'test'],
        env=env, text=True, capture_output=True)
    assert result.returncode != 0
    assert calls.read_text().splitlines().count('rmmod hermos_kunit') == 1


def test_successful_debugfs_result_survives_kernel_log_ring_rotation(tmp_path):
    calls, env = fixture(tmp_path)
    messages = ('[1.000] hermos: endpoint added\n' * 40000 +
                '# Totals: pass:2 fail:0 skip:0 total:2\n')
    assert len(messages) > 1024 * 1024
    Path(env['TEST_AFTER']).write_text(messages[-128 * 1024:])
    result = subprocess.run(
        ['bash', '-c', 'source "$RUN_SCRIPT"; '
         'read_kernel_config() { CONFIG_FILE="$TEST_CONFIG"; }; main', 'test'],
        env=env, text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    lines = calls.read_text().splitlines()
    assert 'dmesg' not in lines
    assert lines.index('cat ' + RESULT_PATH) < lines.index('rmmod hermos_kunit')


def test_debugfs_required_before_build_or_load(tmp_path):
    calls, env = fixture(tmp_path)
    Path(env['TEST_CONFIG']).write_text('CONFIG_KUNIT=y\n# CONFIG_KUNIT_DEBUGFS is not set\n')
    result = subprocess.run(
        ['bash', '-c', 'source "$RUN_SCRIPT"; '
         'read_kernel_config() { CONFIG_FILE="$TEST_CONFIG"; }; main', 'test'],
        env=env, text=True, capture_output=True)
    assert result.returncode != 0
    assert 'CONFIG_KUNIT_DEBUGFS=y' in result.stderr
    assert not calls.exists()
