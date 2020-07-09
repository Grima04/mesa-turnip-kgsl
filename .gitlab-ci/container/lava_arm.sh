#!/bin/bash

set -e
set -o xtrace

check_minio()
{
    MINIO_PATH="minio-packet.freedesktop.org/mesa-lava/$1/${DISTRIBUTION_TAG}/${DEBIAN_ARCH}"
    if wget -q --method=HEAD "https://${MINIO_PATH}/done"; then
        exit
    fi
}

# If remote files are up-to-date, skip rebuilding them
check_minio "mesa/mesa"
check_minio "${CI_PROJECT_PATH}"

. .gitlab-ci/container/container_pre_build.sh

if [[ "$DEBIAN_ARCH" = "arm64" ]]; then
    GCC_ARCH="aarch64-linux-gnu"
    KERNEL_ARCH="arm64"
    DEFCONFIG="arch/arm64/configs/defconfig"
    DEVICE_TREES="arch/arm64/boot/dts/rockchip/rk3399-gru-kevin.dtb arch/arm64/boot/dts/amlogic/meson-gxl-s905x-libretech-cc.dtb arch/arm64/boot/dts/allwinner/sun50i-h6-pine-h64.dtb arch/arm64/boot/dts/amlogic/meson-gxm-khadas-vim2.dtb arch/arm64/boot/dts/qcom/apq8016-sbc.dtb"
    KERNEL_IMAGE_NAME="Image"
else
    GCC_ARCH="arm-linux-gnueabihf"
    KERNEL_ARCH="arm"
    DEFCONFIG="arch/arm/configs/multi_v7_defconfig"
    DEVICE_TREES="arch/arm/boot/dts/rk3288-veyron-jaq.dtb arch/arm/boot/dts/sun8i-h3-libretech-all-h3-cc.dtb"
    KERNEL_IMAGE_NAME="zImage"
    . .gitlab-ci/create-cross-file.sh armhf
fi

# Determine if we're in a cross build.
if [[ -e /cross_file-$DEBIAN_ARCH.txt ]]; then
    EXTRA_MESON_ARGS="--cross-file /cross_file-$DEBIAN_ARCH.txt"
    EXTRA_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=/toolchain-$DEBIAN_ARCH.cmake"

    export ARCH=${KERNEL_ARCH}
    export CROSS_COMPILE="${GCC_ARCH}-"
fi

apt-get update
apt-get install -y automake \
                   git \
                   bc \
                   cmake \
                   cpio \
                   wget \
                   debootstrap \
                   libboost-dev \
                   libegl1-mesa-dev \
                   libgbm-dev \
                   libgles2-mesa-dev \
                   libpcre3-dev \
                   libpng-dev \
                   libpython3-dev \
                   libssl-dev \
                   libvulkan-dev \
                   libxcb-keysyms1-dev \
                   python3-dev \
                   python3-distutils \
                   python3-serial \
                   qt5-default \
                   qt5-qmake \
                   qtbase5-dev


if [[ "$DEBIAN_ARCH" = "armhf" ]]; then
	apt-get install -y libboost-dev:armhf \
		libegl1-mesa-dev:armhf \
		libelf-dev:armhf \
		libgbm-dev:armhf \
		libgles2-mesa-dev:armhf \
		libpcre3-dev:armhf \
		libpng-dev:armhf \
		libpython3-dev:armhf \
		libvulkan-dev:armhf \
		libxcb-keysyms1-dev:armhf \
               qtbase5-dev:armhf
fi

############### Build dEQP runner
. .gitlab-ci/build-cts-runner.sh
mkdir -p /lava-files/rootfs-${DEBIAN_ARCH}/usr/bin
mv /usr/local/bin/deqp-runner /lava-files/rootfs-${DEBIAN_ARCH}/usr/bin/.


############### Build dEQP
STRIP_CMD="${GCC_ARCH}-strip"
if [ -n "$INCLUDE_VK_CTS" ]; then
   DEQP_TARGET=surfaceless . .gitlab-ci/build-deqp-vk.sh
else
   . .gitlab-ci/build-deqp-gl.sh
fi

mv /deqp /lava-files/rootfs-${DEBIAN_ARCH}/.


############### Build apitrace
. .gitlab-ci/build-apitrace.sh
mkdir -p /lava-files/rootfs-${DEBIAN_ARCH}/apitrace
mv /apitrace/build /lava-files/rootfs-${DEBIAN_ARCH}/apitrace
rm -rf /apitrace

mkdir -p /lava-files/rootfs-${DEBIAN_ARCH}/waffle
mv /waffle/build /lava-files/rootfs-${DEBIAN_ARCH}/waffle
rm -rf /waffle


############### Build renderdoc
EXTRA_CMAKE_ARGS+=" -DENABLE_XCB=false"
. .gitlab-ci/build-renderdoc.sh
mkdir -p /lava-files/rootfs-${DEBIAN_ARCH}/renderdoc
mv /renderdoc/build /lava-files/rootfs-${DEBIAN_ARCH}/renderdoc
rm -rf /renderdoc


############### Cross-build kernel
KERNEL_URL="https://gitlab.freedesktop.org/tomeu/linux/-/archive/v5.5-panfrost-fixes/linux-v5.5-panfrost-fixes.tar.gz"
mkdir -p kernel
wget -qO- ${KERNEL_URL} | tar -xz --strip-components=1 -C kernel
pushd kernel
./scripts/kconfig/merge_config.sh ${DEFCONFIG} ../.gitlab-ci/${KERNEL_ARCH}.config
make ${KERNEL_IMAGE_NAME} dtbs
for image in ${KERNEL_IMAGE_NAME}; do
    cp arch/${KERNEL_ARCH}/boot/${image} /lava-files/.
done
cp ${DEVICE_TREES} /lava-files/.

popd
rm -rf kernel

############### Create rootfs
set +e
debootstrap \
    --variant=minbase \
    --arch=${DEBIAN_ARCH} \
     --components main,contrib,non-free \
    buster \
    /lava-files/rootfs-${DEBIAN_ARCH}/ \
    http://deb.debian.org/debian

cat /lava-files/rootfs-${DEBIAN_ARCH}/debootstrap/debootstrap.log
set -e

cp .gitlab-ci/create-rootfs.sh /lava-files/rootfs-${DEBIAN_ARCH}/.
chroot /lava-files/rootfs-${DEBIAN_ARCH} sh /create-rootfs.sh
rm /lava-files/rootfs-${DEBIAN_ARCH}/create-rootfs.sh
pushd /lava-files/rootfs-${DEBIAN_ARCH}
  find -H  |  cpio -H newc -o | gzip -c - > /lava-files/lava-rootfs.cpio.gz
popd
rm -rf /lava-files/rootfs-${DEBIAN_ARCH}

ls -lh /lava-files/

. .gitlab-ci/container/container_post_build.sh

############### Upload the files!
ci-fairy minio login $CI_JOB_JWT
for f in $(ls /lava-files/); do
    ci-fairy minio cp /lava-files/$f \
        minio://${MINIO_PATH}/$f
done

touch /lava-files/done
ci-fairy minio cp /lava-files/done minio://${MINIO_PATH}/done

