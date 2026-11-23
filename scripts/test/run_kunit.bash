#!/bin/bash
set -euo pipefail

read_kernel_config() {
    if [ -f "/boot/config-$(uname -r)" ]; then
        CONFIG_FILE="/boot/config-$(uname -r)"
    elif [ -f /proc/config.gz ]; then
        CONFIG_FILE="$KUNIT_TEMP_DIR/config"
        zcat /proc/config.gz > "$CONFIG_FILE"
    else
        echo "Kernel config file not found!" >&2
        return 1
    fi
}

validate_results() {
    # This suite has flat, non-parameterized cases. Check its complete debugfs
    # KTAP document; optional Totals diagnostics alone cannot prove completion.
    python3 -I - "$1" <<'PY'
from pathlib import Path
import re
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


try:
    lines = Path(sys.argv[1]).read_text().splitlines()
    require(len(lines) >= 7 and lines[:2] == ['KTAP version 1', '1..1'], 'missing KTAP header')
    header = re.fullmatch(r'( +)KTAP version 1', lines[2])
    require(header is not None, 'missing suite header')
    indent = header[1]
    require(lines[3] == indent + '# Subtest: hermos_test_suite', 'unexpected suite')
    plan = re.fullmatch(re.escape(indent) + r'1\.\.([1-9][0-9]*)', lines[4])
    require(plan is not None, 'missing or empty case plan')
    require(lines[-1] == 'ok 1 hermos_test_suite', 'suite did not complete successfully')
    passed = skipped = seen = 0
    statistics = {}
    for line in lines[5:-1]:
        if line.startswith(('# Totals:', '# hermos_test_suite:')):
            stats = re.fullmatch(r'# (Totals|hermos_test_suite): pass:(\d+) fail:(\d+) skip:(\d+) total:(\d+)', line)
            require(stats is not None and stats[1] not in statistics, 'malformed or duplicate statistics')
            statistics[stats[1]] = tuple(map(int, stats.groups()[1:]))
            continue
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        result = re.fullmatch(re.escape(indent) + r'(ok|not ok) ([1-9][0-9]*) ([^#]+?)(?: # (.*))?', line)
        require(result is not None, 'unexpected or nested KTAP result: ' + line)
        seen += 1
        require(int(result[2]) == seen, 'missing or duplicate case result')
        directive = result[4]
        if directive is not None:
            require(re.fullmatch(r'SKIP(?: .*)?', directive) is not None, 'unsupported case directive')
            skipped += 1
        else:
            require(result[1] == 'ok', 'failed case: ' + result[3])
            passed += 1
    require(seen == int(plan[1]) and passed > 0, 'incomplete or entirely skipped suite')
    require(all(value == (passed, 0, skipped, seen) for value in statistics.values()), 'inconsistent statistics')
except (OSError, ValueError) as error:
    print('KUnit results rejected: ' + str(error), file=sys.stderr)
    sys.exit(1)
PY
}

cleanup() {
    local status=$?
    if [[ "$KUNIT_MODULE_LOADED" == 1 ]]; then
        if ! sudo rmmod hermos_kunit; then
            status=1
        fi
    fi
    rm -rf -- "$KUNIT_TEMP_DIR"
    exit "$status"
}

main() {
    KUNIT_MODULE_LOADED=0
    KUNIT_TEMP_DIR=$(mktemp -d)
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    read_kernel_config
    if ! grep -q '^CONFIG_KUNIT=y$' "$CONFIG_FILE"; then
        echo "Skipping KUnit tests as CONFIG_KUNIT is not enabled."
        return 0
    fi
    if ! grep -q '^CONFIG_KUNIT_DEBUGFS=y$' "$CONFIG_FILE"; then
        echo "KUnit results require CONFIG_KUNIT_DEBUGFS=y and mounted debugfs." >&2
        return 1
    fi

    # The test module contains the backend symbols, so no Hermos module may
    # already be present. Never unload a module this script did not insert.
    if lsmod | awk '$1 ~ /^hermos(_|$)/ { found=1 } END { exit !found }'; then
        echo "A Hermos kernel module is already loaded. Unload it before KUnit tests." >&2
        return 1
    fi

    local results=/sys/kernel/debug/kunit/hermos_test_suite/results
    if ! sudo test -d /sys/kernel/debug/kunit; then
        echo "KUnit debugfs is unavailable; mount debugfs before running this script." >&2
        return 1
    fi
    if ! sudo test ! -e "$results"; then
        echo "KUnit suite results already exist; cannot attribute a new run." >&2
        return 1
    fi

    HERMOS_DIR=$(realpath "$(dirname "${BASH_SOURCE[0]}")/../..")
    HERMOS_KMOD_PATH="$HERMOS_DIR/hermos_kmod"
    cd "$HERMOS_KMOD_PATH"
    make clean
    make test

    sudo insmod "$HERMOS_KMOD_PATH/hermos_kunit.ko"
    KUNIT_MODULE_LOADED=1
    # Module tests finish during insmod. Their private results survive printk
    # ring rotation, but debugfs removes them when the module is unloaded.
    sudo cat "$results" > "$KUNIT_TEMP_DIR/results"
    validate_results "$KUNIT_TEMP_DIR/results"
    sudo rmmod hermos_kunit
    KUNIT_MODULE_LOADED=0

    sudo lcov --capture --directory "/sys/kernel/debug/gcov/$HERMOS_KMOD_PATH" --output-file "$HERMOS_DIR/coverage.info"
    sudo lcov --remove "$HERMOS_DIR/coverage.info" '*linux*' '*kunit*' --output-file "$HERMOS_DIR/coverage_filtered.info"
    genhtml "$HERMOS_DIR/coverage_filtered.info" --output-directory "$HERMOS_DIR/hermos_kmod_coverage_report"
    rm -f "$HERMOS_DIR/coverage.info" "$HERMOS_DIR/coverage_filtered.info" "$HERMOS_KMOD_PATH"/*.gcno
    echo "Please open hermos_kmod_coverage_report/index.html in your web browser."
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main
fi
