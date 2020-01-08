#!/bin/bash

set -ex

RENDERDOC_VERSION=6653316a62f6168b3e45040358cb77612dcffcb8

git clone https://github.com/baldurk/renderdoc.git --single-branch --no-checkout /renderdoc
pushd /renderdoc
git checkout "$RENDERDOC_VERSION"
cmake -G Ninja -B_build -H. -DENABLE_QRENDERDOC=false -DCMAKE_BUILD_TYPE=Release
ninja -C _build -j4
mkdir -p build/lib
cp _build/lib/renderdoc.so build/lib
cp _build/lib/librenderdoc.so build/lib
strip build/lib/*
find . -not -path './build' -not -path './build/*' -delete
popd
