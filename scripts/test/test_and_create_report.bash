#!/bin/bash

# Remove hermos-heaphook if it is installed, for unit tests
HEAPHOOK_FOUND=0
if ls "/usr/lib/" | grep -q "libhermos_heaphook.so"; then
    sudo apt-get -y remove hermos-heaphook
    HEAPHOOK_FOUND=1
fi

rm -rf build install log
colcon build --cmake-args -DCOVERAGE=ON

# Build and install heaphook for tests
cd hermos_heaphook
cargo build --release
cd ..
cp hermos_heaphook/target/release/libhermos_heaphook.so install/hermoslib/lib/

colcon test --packages-skip ros2hermos --event-handlers console_direct+
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '/usr/*' '*/test/*' '*gtest*' '*gmock*' '/opt/*' --output-file coverage_filtered.info
genhtml coverage_filtered.info --output-directory coverage_report_hermoslib
rm -rf coverage.info coverage_filtered.info

# Reinstall hermos-heaphook if it was installed
if [ $HEAPHOOK_FOUND -eq 1 ]; then
    cd src/hermos_heaphook
    cargo deb --install
fi

echo -e "\nPlease open coverage_report_hermoslib/index.html"
