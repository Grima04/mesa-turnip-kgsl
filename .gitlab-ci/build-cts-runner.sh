#!/bin/bash

set -ex

git clone https://github.com/anholt/cts_runner.git --depth 1 -b anholt-mesa-ci-2
cd cts_runner
meson build/
ninja -C build -j4 install
cd ..
rm -rf cts_runner
