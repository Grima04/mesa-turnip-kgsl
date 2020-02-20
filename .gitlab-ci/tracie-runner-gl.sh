#!/bin/sh

set -ex

ARTIFACTS="$(pwd)/artifacts"

# Set up the driver environment.
export LD_LIBRARY_PATH="$(pwd)/install/lib/"

# Set environment for renderdoc libraries.
export PYTHONPATH="$PYTHONPATH:/renderdoc/build/lib"
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/renderdoc/build/lib"

# Perform a self-test to ensure tracie is working properly.
"$ARTIFACTS/tracie/tests/test.sh"

ret=0

# The renderdoc version we use can handle surfaceless.
EGL_PLATFORM=surfaceless DISPLAY= \
    "$ARTIFACTS/tracie/tracie.sh" "$ARTIFACTS/traces.yml" renderdoc \
    || ret=1

# We need a newer waffle to use surfaceless with apitrace. For now run with
# xvfb.
xvfb-run --server-args="-noreset" sh -c \
    "set -ex; \
     export LD_LIBRARY_PATH=$LD_LIBRARY_PATH; \
     export PATH=/apitrace/build:\$PATH; \
    \"$ARTIFACTS/tracie/tracie.sh\" \"$ARTIFACTS/traces.yml\" apitrace" \
    || ret=1

exit $ret
