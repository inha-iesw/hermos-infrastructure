#!/bin/bash

colcon build --symlink-install --cmake-args -DCMAKE_BUILD_TYPE=Release

cd hermos_kmod
make

cd ../hermos_heaphook
cargo vendor
cargo build --release
cd ..
cp hermos_heaphook/target/release/libhermos_heaphook.so install/hermoslib/lib
