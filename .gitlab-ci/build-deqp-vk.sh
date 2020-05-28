#!/bin/bash

set -ex

git config --global user.email "mesa@example.com"
git config --global user.name "Mesa CI"
git clone \
    https://github.com/KhronosGroup/VK-GL-CTS.git \
    -b vulkan-cts-1.2.2.1 \
    /VK-GL-CTS
pushd /VK-GL-CTS

# Cherry pick a fix that's not in 1.2.2.1 yet.  Re-add --depth 1 to the clone
# when an uprev removes this.
git cherry-pick -x ea6f1ffae14de94bbd9c354ad5a6c3f452f65ac4

# --insecure is due to SSL cert failures hitting sourceforge for zlib and
# libpng (sigh).  The archives get their checksums checked anyway, and git
# always goes through ssh or https.
python3 external/fetch_sources.py --insecure

mkdir -p /deqp

popd

pushd /deqp
cmake -G Ninja \
      -DDEQP_TARGET=x11_glx \
      -DCMAKE_BUILD_TYPE=Release \
      /VK-GL-CTS
ninja

# Copy out the mustpass list we want.
mkdir /deqp/mustpass
cp /VK-GL-CTS/external/vulkancts/mustpass/master/vk-default.txt \
   /deqp/mustpass/vk-master.txt

rm -rf /deqp/modules/internal
rm -rf /deqp/executor
rm -rf /deqp/execserver
rm -rf /deqp/modules/egl
rm -rf /deqp/framework
find -iname '*cmake*' -o -name '*ninja*' -o -name '*.o' -o -name '*.a' | xargs rm -rf
strip external/vulkancts/modules/vulkan/deqp-vk
du -sh *
rm -rf /VK-GL-CTS
popd
