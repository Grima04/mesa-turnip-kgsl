#!/bin/bash

set -ex

git clone https://gitlab.freedesktop.org/mesa/parallel-deqp-runner.git --depth 1
cd parallel-deqp-runner
meson build/
ninja -C build -j4 install
cd ..
rm -rf parallel-deqp-runner
