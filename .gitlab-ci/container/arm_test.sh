#!/bin/bash

set -e
set -o xtrace

# etnaviv will eventually need armhf too.
CROSS_ARCHITECTURES="arm64"

. .gitlab-ci/container/container_pre_build.sh

############### Create rootfs

for arch in $CROSS_ARCHITECTURES; do
  DEBIAN_ARCH=$arch . .gitlab-ci/container/lava_arm.sh
done

############### Uninstall the build software

ccache --show-stats

apt-get purge -y \
        bc \
        bison \
        bzip2 \
        ccache \
        cmake \
        g++ \
        flex \
        git \
        meson \
        pkg-config \
        python3-distutils \
        procps \
        u-boot-tools

for arch in $CROSS_ARCHITECTURES; do
    apt-get purge -y ".*:${arch}"
done

apt-get autoremove -y --purge
