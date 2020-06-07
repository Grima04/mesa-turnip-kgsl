#!/bin/bash

set -e
set -o xtrace

# etnaviv will eventually need armhf too.
CROSS_ARCHITECTURES="arm64"

for arch in $CROSS_ARCHITECTURES; do
    dpkg --add-architecture $arch
done

############### Install packages for building
apt-get install -y ca-certificates
sed -i -e 's/http:\/\/deb/https:\/\/deb/g' /etc/apt/sources.list
echo 'deb https://deb.debian.org/debian buster-backports main' >/etc/apt/sources.list.d/backports.list
apt-get update

apt-get install -y --no-remove \
        abootimg \
        android-sdk-ext4-utils \
        bc \
        bison \
        bzip2 \
        ccache \
        cmake \
        cpio \
        g++ \
        debootstrap \
        fastboot \
        flex \
        git \
        netcat \
        python3-distutils \
        python3-minimal \
        python3-serial \
        python3.7 \
        pkg-config \
        procps \
        u-boot-tools \
        unzip

apt install -t buster-backports -y --no-remove \
    meson

# Cross-build test deps
for arch in $CROSS_ARCHITECTURES; do
    apt-get install -y --no-remove \
        crossbuild-essential-${arch} \
        libdrm-dev:${arch} \
        libegl1-mesa-dev:${arch} \
        libelf-dev:${arch} \
        libexpat1-dev:${arch} \
        libffi-dev:${arch} \
        libgbm-dev:${arch} \
        libgles2-mesa-dev:${arch} \
        libpng-dev:${arch} \
        libstdc++6:${arch} \
        libtinfo-dev:${arch} \
        libegl1-mesa-dev:${arch} \
        libvulkan-dev:${arch}

    mkdir /var/cache/apt/archives/${arch}
done

############### Create cross-files

for arch in $CROSS_ARCHITECTURES; do
  . .gitlab-ci/create-cross-file.sh $arch
done

. .gitlab-ci/container/container_post_build.sh
