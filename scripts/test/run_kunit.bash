#!/bin/bash

### Validation
if [ -f "/boot/config-$(uname -r)" ]; then
    CONFIG_FILE="/boot/config-$(uname -r)"
elif [ -f "/proc/config.gz" ]; then
    zcat /proc/config.gz >/tmp/config-$(uname -r)
    CONFIG_FILE="/tmp/config-$(uname -r)"
else
    echo "Kernel config file not found!"
    exit 1
fi

if ! grep -q "CONFIG_KUNIT=y" $CONFIG_FILE; then
    echo "Skipping KUnit tests as CONFIG_KUNIT is not enabled."
    exit 0
fi

### Check if hermos.ko is not already loaded.
### Since hermos_kunit.ko contains symbols from hermos.ko,
### symbol conflicts will occur if it is already loaded.
if lsmod | grep -q "hermos"; then
    echo "Kernel module 'hermos' is already loaded. Unload it before kunit test."
    exit 1
fi

HERMOS_DIR=$(realpath "$(dirname $(readlink -f $0))/../..")

HERMOS_KMOD_PATH=$HERMOS_DIR/hermos_kmod

if [ -z "$HERMOS_KMOD_PATH" ]; then
    echo "Usage: create_coverage_report <hermos_kmod_path>"
    exit 1
fi

cd $HERMOS_KMOD_PATH
make clean
make test
sudo insmod $HERMOS_KMOD_PATH/hermos_kunit.ko
sleep 3 # HACK
TOTALS_LINE=$(sudo dmesg | tail -n 10 | grep -E "# Totals: pass:[0-9]+ fail:[0-9]+")
FAIL_COUNT=$(echo "$TOTALS_LINE" | awk -F'fail:' '{print $2}' | awk '{print $1}')
sudo rmmod hermos_kunit

if [ $FAIL_COUNT -ne 0 ]; then
    echo "KUnit tests failed. Please check dmesg for more information."
    exit 1
fi

sudo lcov --capture --directory /sys/kernel/debug/gcov/$HERMOS_KMOD_PATH --output-file $HERMOS_DIR/coverage.info
sudo lcov --remove $HERMOS_DIR/coverage.info "*linux*" "*kunit*" --output-file $HERMOS_DIR/coverage_filtered.info
genhtml $HERMOS_DIR/coverage_filtered.info --output-directory $HERMOS_DIR/hermos_kmod_coverage_report
rm -f $HERMOS_DIR/coverage.info $HERMOS_DIR/coverage_filtered.info $HERMOS_KMOD_PATH/*.gcno
echo "Please open hermos_kmod_coverage_report/index.html in your web browser."
