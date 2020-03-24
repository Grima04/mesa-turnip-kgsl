#!/bin/bash

set -e
set -o xtrace

CROSS_FILE=/cross_file-"$CROSS".txt

# Delete unused bin and includes from artifacts to save space.
rm -rf install/bin install/include

# Strip the drivers in the artifacts to cut 80% of the artifacts size.
if [ -n "$CROSS" ]; then
    STRIP=`sed -n -E "s/strip\s*=\s*'(.*)'/\1/p" "$CROSS_FILE"`
    if [ -z "$STRIP" ]; then
        echo "Failed to find strip command in cross file"
        exit 1
    fi
else
    STRIP="strip"
fi
find install -name \*.so -exec $STRIP {} \;

# Test runs don't pull down the git tree, so put the dEQP helper
# script and associated bits there.
cp VERSION install/
cp -Rp .gitlab-ci/deqp* install/
cp -Rp .gitlab-ci/piglit install/
cp -Rp .gitlab-ci/traces.yml install/
cp -Rp .gitlab-ci/tracie install/
cp -Rp .gitlab-ci/tracie-runner-gl.sh install/
cp -Rp .gitlab-ci/tracie-runner-vk.sh install/
cp -Rp .gitlab-ci/fossils.yml install/
cp -Rp .gitlab-ci/fossils install/
cp -Rp .gitlab-ci/fossilize-runner.sh install/
cp -Rp .gitlab-ci/deqp-runner.sh install/
cp -Rp .gitlab-ci/deqp-*-fails.txt install/
cp -Rp .gitlab-ci/deqp-*-skips.txt install/

# Tar up the install dir so that symlinks and hardlinks aren't each
# packed separately in the zip file.
mkdir -p artifacts/
tar -cf artifacts/install.tar install

# If the container has LAVA stuff, prepare the artifacts for LAVA jobs
if [ -d /lava-files ]; then
    # Copy kernel and device trees for LAVA
    cp /lava-files/*Image artifacts/.
    cp /lava-files/*.dtb artifacts/.

    # Pack ramdisk for LAVA
    mkdir -p /lava-files/rootfs-${CROSS:-arm64}/install
    cp -a install/* /lava-files/rootfs-${CROSS:-arm64}/install/.

    pushd /lava-files/rootfs-${CROSS:-arm64}/
    find -H  |  cpio -H newc -o | gzip -c - > $CI_PROJECT_DIR/artifacts/lava-rootfs-${CROSS:-arm64}.cpio.gz
    popd

    # Store job ID so the test stage can build URLs to the artifacts
    echo $CI_JOB_ID > artifacts/build_job_id.txt

    # Pass needed files to the test stage
    cp $CI_PROJECT_DIR/.gitlab-ci/generate_lava.py artifacts/.
    cp $CI_PROJECT_DIR/.gitlab-ci/lava-deqp.yml.jinja2 artifacts/.
fi
