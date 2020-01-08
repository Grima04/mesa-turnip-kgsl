#!/bin/bash

set -ex

APITRACE_VERSION="9.0"

git clone https://github.com/apitrace/apitrace.git --single-branch --no-checkout /apitrace
pushd /apitrace
git checkout "$APITRACE_VERSION"
cmake -G Ninja -B_build -H. -DCMAKE_BUILD_TYPE=Release -DENABLE_GUI=False
ninja -C _build -j4
mkdir build
cp _build/apitrace build
cp _build/glretrace build
cp _build/eglretrace build
strip build/*
find . -not -path './build' -not -path './build/*' -delete
popd
