#!/bin/bash

set -e
set -o xtrace

export DEBIAN_FRONTEND=noninteractive

CROSS_ARCHITECTURES="i386 ppc64el s390x"
for arch in $CROSS_ARCHITECTURES; do
    dpkg --add-architecture $arch
done

apt-get install -y \
        ca-certificates \
        gnupg \
        unzip \
        wget

# Upstream LLVM package repository
apt-key add .gitlab-ci/container/llvm-snapshot.gpg.key
echo "deb https://apt.llvm.org/buster/ llvm-toolchain-buster-9 main" >/etc/apt/sources.list.d/llvm9.list

sed -i -e 's/http:\/\/deb/https:\/\/deb/g' /etc/apt/sources.list
echo 'deb https://deb.debian.org/debian buster-backports main' >/etc/apt/sources.list.d/backports.list

apt-get update

apt-get install -y --no-remove \
        $STABLE_EPHEMERAL \
        bison \
        ccache \
        clang-9 \
        flex \
        g++ \
        g++-mingw-w64-x86-64 \
        gcc \
        gettext \
        libclang-9-dev \
        libclc-dev \
        libdrm-dev:s390x \
        libelf-dev \
        libepoxy-dev \
        libexpat1-dev \
        libgtk-3-dev \
        libomxil-bellagio-dev \
        libpciaccess-dev \
        libpciaccess-dev:i386 \
        libunwind-dev \
        libva-dev \
        libvdpau-dev \
        libvulkan-dev \
        libvulkan-dev:ppc64el \
        libx11-dev \
        libx11-xcb-dev \
        libxdamage-dev \
        libxext-dev \
        libxml2-utils \
        libxrandr-dev \
        libxrender-dev \
        libxshmfence-dev \
        libxvmc-dev \
        libxxf86vm-dev \
        libz-mingw-w64-dev \
        llvm-9-dev \
        pkg-config \
        python-mako \
        python3-mako \
        python3-pil \
        python3-requests \
        qemu-user \
        scons \
        wine-development \
        wine32-development \
        wine64-development \
        x11proto-dri2-dev \
        x11proto-gl-dev \
        x11proto-randr-dev \
        xz-utils \
        zlib1g-dev

apt-get install -y --no-remove -t buster-backports \
        libclang-8-dev \
        libllvm8 \
        meson

# Cross-build Mesa deps
for arch in $CROSS_ARCHITECTURES; do
    apt-get install -y --no-remove \
            crossbuild-essential-${arch} \
            libelf-dev:${arch} \
            libexpat1-dev:${arch} \
            libffi-dev:${arch} \
            libstdc++6:${arch} \
            libtinfo-dev:${arch}

    apt-get install -y --no-remove -t buster-backports \
            libllvm8:${arch}

    mkdir /var/cache/apt/archives/${arch}
    # Download llvm-* packages, but don't install them yet, since they can
    # only be installed for one architecture at a time
    apt-get install -o Dir::Cache::archives=/var/cache/apt/archives/$arch --download-only \
            -y --no-remove -t buster-backports \
            llvm-8-dev:${arch}
done

apt-get install -y --no-remove -t buster-backports \
        llvm-8-dev


# Generate cross build files for Meson
for arch in $CROSS_ARCHITECTURES; do
    . .gitlab-ci/create-cross-file.sh $arch
done


# for the vulkan overlay layer
wget https://github.com/KhronosGroup/glslang/releases/download/master-tot/glslang-master-linux-Release.zip
unzip glslang-master-linux-Release.zip bin/glslangValidator
install -m755 bin/glslangValidator /usr/local/bin/
rm bin/glslangValidator glslang-master-linux-Release.zip


############### Uninstall ephemeral packages

apt-get purge -y \
        gnupg \
        unzip \
        wget

. .gitlab-ci/container/container_post_build.sh
