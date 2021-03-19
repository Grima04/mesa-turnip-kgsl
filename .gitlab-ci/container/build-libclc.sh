#!/bin/bash

set -ex

export LLVM_CONFIG="llvm-config-11"

$LLVM_CONFIG --version

git clone https://github.com/KhronosGroup/SPIRV-LLVM-Translator -b llvm_release_110 --single-branch --shallow-since=2020-11-12 /SPIRV-LLVM-Translator
pushd /SPIRV-LLVM-Translator
# Last commit before bumping required LLVM version to 11.1.0
git checkout 93032d36d2fe17befb7994714c07c67ea68efbea
cmake -S . -B . -G Ninja -DLLVM_BUILD_TOOLS=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-fPIC -DCMAKE_CXX_FLAGS=-fPIC -DCMAKE_INSTALL_PREFIX=`$LLVM_CONFIG --prefix`
ninja
ninja install
popd


git config --global user.email "mesa@example.com"
git config --global user.name "Mesa CI"
git clone \
    https://github.com/llvm/llvm-project \
    --depth 1 \
    /llvm-project

mkdir /libclc
pushd /libclc
cmake -S /llvm-project/libclc -B . -G Ninja -DLLVM_CONFIG=$LLVM_CONFIG -DLIBCLC_TARGETS_TO_BUILD="spirv-mesa3d-;spirv64-mesa3d-" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
ninja
ninja install
popd

# workaroud cmake vs debian packaging.
mkdir -p /usr/lib/clc
ln -s /usr/share/clc/spirv64-mesa3d-.spv /usr/lib/clc/
ln -s /usr/share/clc/spirv-mesa3d-.spv /usr/lib/clc/

du -sh *
rm -rf /libclc /llvm-project /SPIRV-LLVM-Translator
