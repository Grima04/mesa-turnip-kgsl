#!/bin/bash

set -ex

mkdir -p /epoxy
pushd /epoxy
wget -qO- https://github.com/anholt/libepoxy/releases/download/1.5.4/libepoxy-1.5.4.tar.xz | tar -xJ --strip-components=1
meson build/ $EXTRA_MESON_ARGS
ninja -C build install
popd
rm -rf /epoxy

VIRGLRENDERER_VERSION=33cfd72346878a71f27f2f11ac41f9e347a34369
git clone https://gitlab.freedesktop.org/virgl/virglrenderer.git --single-branch --no-checkout /virglrenderer
pushd /virglrenderer
git checkout "$VIRGLRENDERER_VERSION"
meson build/ $EXTRA_MESON_ARGS
ninja -C build install
popd
rm -rf /virglrenderer
