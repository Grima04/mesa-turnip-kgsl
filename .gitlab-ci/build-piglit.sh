#!/bin/bash

set -ex

if [ -n "$INCLUDE_OPENCL_TESTS" ]; then
    PIGLIT_OPTS="-DPIGLIT_BUILD_CL_TESTS=ON"
fi

git clone https://gitlab.freedesktop.org/mesa/piglit.git --single-branch --no-checkout /piglit
pushd /piglit
git checkout c702d2bbf28b01a18ce613f386a4ffef03f6c0c9
patch -p1 <$OLDPWD/.gitlab-ci/piglit/disable-vs_in.diff
cmake -S . -B . -G Ninja -DCMAKE_BUILD_TYPE=Release $PIGLIT_OPTS
ninja
find -name .git -o -name '*ninja*' -o -iname '*cmake*' -o -name '*.[chao]' | xargs rm -rf
rm -rf target_api
popd
