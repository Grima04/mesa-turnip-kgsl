#!/bin/bash

set -ex

DEQP_OPTIONS=(--deqp-surface-width=256 --deqp-surface-height=256)
DEQP_OPTIONS+=(--deqp-surface-type=pbuffer)
DEQP_OPTIONS+=(--deqp-gl-config-name=rgba8888d24s8ms0)
DEQP_OPTIONS+=(--deqp-visibility=hidden)

# It would be nice to be able to enable the watchdog, so that hangs in a test
# don't need to wait the full hour for the run to time out.  However, some
# shaders end up taking long enough to compile
# (dEQP-GLES31.functional.ubo.random.all_per_block_buffers.20 for example)
# that they'll sporadically trigger the watchdog.
#DEQP_OPTIONS+=(--deqp-watchdog=enable)

if [ -z "$DEQP_VER" ]; then
   echo 'DEQP_VER must be set to something like "gles2" or "gles31" for the test run'
   exit 1
fi

if [ -z "$DEQP_SKIPS" ]; then
   echo 'DEQP_SKIPS must be set to something like "deqp-default-skips.txt"'
   exit 1
fi

ARTIFACTS=`pwd`/artifacts

# Set up the driver environment.
export LD_LIBRARY_PATH=`pwd`/install/lib/
export EGL_PLATFORM=surfaceless

# the runner was failing to look for libkms in /usr/local/lib for some reason
# I never figured out.
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib

RESULTS=`pwd`/results
mkdir -p $RESULTS

# Generate test case list file
cp /deqp/mustpass/$DEQP_VER-master.txt /tmp/case-list.txt

# If the job is parallel, take the corresponding fraction of the caselist.
# Note: N~M is a gnu sed extension to match every nth line (first line is #1).
if [ -n "$CI_NODE_INDEX" ]; then
   sed -ni $CI_NODE_INDEX~$CI_NODE_TOTAL"p" /tmp/case-list.txt
fi

if [ ! -s /tmp/case-list.txt ]; then
    echo "Caselist generation failed"
    exit 1
fi

if [ -n "$DEQP_EXPECTED_FAILS" ]; then
    XFAIL="--xfail-list $ARTIFACTS/$DEQP_EXPECTED_FAILS"
fi

set +e

vulkan-cts-runner \
    --deqp /deqp/modules/$DEQP_VER/deqp-$DEQP_VER \
    --output $RESULTS/cts-runner-results.txt \
    --caselist /tmp/case-list.txt \
    --exclude-list $ARTIFACTS/$DEQP_SKIPS \
    $XFAIL \
    --job ${DEQP_PARALLEL:-1} \
    -- \
    "${DEQP_OPTIONS[@]}"
DEQP_EXITCODE=$?

if [ $DEQP_EXITCODE -ne 0 ]; then
    echo "Some unexpected results found (see cts-runner-results.txt in artifacts for full results):"
    cat $RESULTS/cts-runner-results.txt | \
        grep -v ",Pass" | \
        grep -v ",Skip" | \
        grep -v ",ExpectedFail" > \
        $RESULTS/cts-runner-unexpected-results.txt
    head -n 50 $RESULTS/cts-runner-unexpected-results.txt
fi

exit $DEQP_EXITCODE
