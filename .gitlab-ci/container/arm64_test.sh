#!/bin/bash

arch=arm64

INCLUDE_PIGLIT=1
PIGLIT_BUILD_TARGETS="piglit_replayer"

. .gitlab-ci/container/baremetal_build.sh
