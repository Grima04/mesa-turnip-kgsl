#!/bin/sh

set -ex

ARTIFACTS="$(pwd)/artifacts"

# Set the Vulkan driver to use.
export VK_ICD_FILENAMES="$(pwd)/install/share/vulkan/icd.d/${VK_DRIVER}_icd.x86_64.json"

# Set environment for VulkanTools' VK_LAYER_LUNARG_screenshot layer.
export VK_LAYER_PATH="$VK_LAYER_PATH:/VulkanTools/build/etc/vulkan/explicit_layer.d"
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/VulkanTools/build/lib"

# Perform a self-test to ensure tracie is working properly.
"$ARTIFACTS/tracie/tests/test.sh"

ret=0

# Run gfxreconstruct traces against the host's running X server (xvfb
# doesn't have DRI3 support).
# Set the DISPLAY env variable in each gitlab-runner's configuration
# file:
# https://docs.gitlab.com/runner/configuration/advanced-configuration.html#the-runners-section
PATH="/gfxreconstruct/build/bin:$PATH" \
    python3 $ARTIFACTS/tracie/tracie.py --file $ARTIFACTS/traces.yml --device-name $DEVICE_NAME

exit $ret
