#!/bin/bash

set -e
set -o xtrace

export DEBIAN_FRONTEND=noninteractive

# Ephemeral packages (installed for this script and removed again at the end)
STABLE_EPHEMERAL=" \
      autoconf \
      automake \
      autotools-dev \
      bzip2 \
      cmake \
      libgbm-dev \
      libtool \
      unzip \
      "

# We need multiarch for Wine
dpkg --add-architecture i386
apt-get update

apt-get install -y --no-remove \
      $STABLE_EPHEMERAL \
      clang \
      libasan6 \
      libarchive-dev \
      libclang-cpp11-dev \
      liblua5.3-dev \
      libxcb-dri2-0-dev \
      libxcb-dri3-dev \
      libxcb-glx0-dev \
      libxcb-present-dev \
      libxcb-randr0-dev \
      libxcb-shm0-dev \
      libxcb-sync-dev \
      libxcb-xfixes0-dev \
      libxcb1-dev \
      libxml2-dev \
      llvm-11-dev \
      llvm-9-dev \
      ocl-icd-opencl-dev \
      procps \
      strace \
      time \
      wine \
      wine32


. .gitlab-ci/container/container_pre_build.sh


# Debian's pkg-config wrapers for mingw are broken, and there's no sign that
# they're going to be fixed, so we'll just have to fix it ourselves
# https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=930492
cat >/usr/local/bin/x86_64-w64-mingw32-pkg-config <<EOF
#!/bin/sh

PKG_CONFIG_LIBDIR=/usr/x86_64-w64-mingw32/lib/pkgconfig pkg-config \$@
EOF
chmod +x /usr/local/bin/x86_64-w64-mingw32-pkg-config


# dependencies where we want a specific version
export              XORG_RELEASES=https://xorg.freedesktop.org/releases/individual
export           WAYLAND_RELEASES=https://wayland.freedesktop.org/releases

export         XORGMACROS_VERSION=util-macros-1.19.0
export         LIBWAYLAND_VERSION=wayland-1.18.0

wget $XORG_RELEASES/util/$XORGMACROS_VERSION.tar.bz2
tar -xvf $XORGMACROS_VERSION.tar.bz2 && rm $XORGMACROS_VERSION.tar.bz2
cd $XORGMACROS_VERSION; ./configure; make install; cd ..
rm -rf $XORGMACROS_VERSION

. .gitlab-ci/container/build-libdrm.sh

wget $WAYLAND_RELEASES/$LIBWAYLAND_VERSION.tar.xz
tar -xvf $LIBWAYLAND_VERSION.tar.xz && rm $LIBWAYLAND_VERSION.tar.xz
cd $LIBWAYLAND_VERSION; ./configure --enable-libraries --without-host-scanner --disable-documentation --disable-dtd-validation; make install; cd ..
rm -rf $LIBWAYLAND_VERSION


# The version of libglvnd-dev in debian is too old
# Check this page to see when this local compilation can be dropped in favour of the package:
# https://packages.debian.org/libglvnd-dev
GLVND_VERSION=1.3.2
wget https://gitlab.freedesktop.org/glvnd/libglvnd/-/archive/v$GLVND_VERSION/libglvnd-v$GLVND_VERSION.tar.gz
tar -xvf libglvnd-v$GLVND_VERSION.tar.gz && rm libglvnd-v$GLVND_VERSION.tar.gz
pushd libglvnd-v$GLVND_VERSION; ./autogen.sh; ./configure; make install; popd
rm -rf libglvnd-v$GLVND_VERSION

. .gitlab-ci/container/build-spirv-tools.sh

git clone https://github.com/KhronosGroup/SPIRV-LLVM-Translator -b llvm_release_110 --single-branch --shallow-since=2020-11-12
pushd SPIRV-LLVM-Translator
# Last commit before bumping required LLVM version to 11.1.0
git checkout 93032d36d2fe17befb7994714c07c67ea68efbea
cmake -S . -B . -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-fPIC -DCMAKE_CXX_FLAGS=-fPIC
ninja
ninja install
popd

pushd /usr/local
git clone https://gitlab.freedesktop.org/mesa/shader-db.git --depth 1
rm -rf shader-db/.git
cd shader-db
make
popd

git clone https://github.com/microsoft/DirectX-Headers -b v1.0.1 --depth 1
pushd DirectX-Headers
mkdir build
cd build
meson .. --backend=ninja --buildtype=release -Dbuild-test=false
ninja
ninja install
popd
rm -rf DirectX-Headers

############### Uninstall the build software

apt-get purge -y \
      $STABLE_EPHEMERAL

. .gitlab-ci/container/container_post_build.sh
