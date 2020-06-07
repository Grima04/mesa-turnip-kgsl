#!/bin/bash

set -e
set -o xtrace

dpkg --add-architecture $arch
apt-get update

# Cross-build test deps
apt-get install -y --no-remove \
        crossbuild-essential-$arch \
        libdrm-dev:$arch \
        libegl1-mesa-dev:$arch \
        libelf-dev:$arch \
        libexpat1-dev:$arch \
        libffi-dev:$arch \
        libgbm-dev:$arch \
        libgles2-mesa-dev:$arch \
        libpng-dev:$arch \
        libstdc++6:$arch \
        libtinfo-dev:$arch \
        libegl1-mesa-dev:$arch \
        libvulkan-dev:$arch

mkdir /var/cache/apt/archives/$arch

############### Create cross-files

. .gitlab-ci/create-cross-file.sh $arch

. .gitlab-ci/container/container_pre_build.sh

############### Create rootfs

DEBIAN_ARCH=$arch . .gitlab-ci/container/lava_arm.sh

ccache --show-stats

. .gitlab-ci/container/container_post_build.sh
