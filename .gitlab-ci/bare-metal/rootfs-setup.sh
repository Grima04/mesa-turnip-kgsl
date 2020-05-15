#!/bin/bash

rootfs_dst=$1

# Copy the rootfs to a temporary for our setup, as I believe changes to the
# container can end up impacting future runs.
cp -Rp $BM_ROOTFS/. $rootfs_dst

# Set up the init script that brings up the system.
cp $BM/init.sh $rootfs_dst/init

set +x
# Pass through relevant env vars from the gitlab job to the baremetal init script
touch $rootfs_dst/set-job-env-vars.sh
chmod +x $rootfs_dst/set-job-env-vars.sh
for var in \
    CI_COMMIT_BRANCH \
    CI_COMMIT_TITLE \
    CI_JOB_ID \
    CI_JOB_URL \
    CI_MERGE_REQUEST_SOURCE_BRANCH_NAME \
    CI_MERGE_REQUEST_TITLE \
    CI_NODE_INDEX \
    CI_NODE_TOTAL \
    CI_PIPELINE_ID \
    CI_RUNNER_DESCRIPTION \
    DEQP_CASELIST_FILTER \
    DEQP_EXPECTED_RENDERER \
    DEQP_NO_SAVE_RESULTS \
    DEQP_PARALLEL \
    DEQP_RUN_SUFFIX \
    DEQP_VER \
    FD_MESA_DEBUG \
    FLAKES_CHANNEL \
    IR3_SHADER_DEBUG \
    NIR_VALIDATE \
    ; do
  val=`echo ${!var} | sed 's|"||g'`
  echo "export $var=\"${val}\"" >> $rootfs_dst/set-job-env-vars.sh
done
echo "Variables passed through:"
cat $rootfs_dst/set-job-env-vars.sh
set -x

# Add the Mesa drivers we built, and make a consistent symlink to them.
mkdir -p $rootfs_dst/$CI_PROJECT_DIR
tar -C $rootfs_dst/$CI_PROJECT_DIR/ -xf $CI_PROJECT_DIR/artifacts/install.tar
ln -sf $CI_PROJECT_DIR/install $rootfs_dst/install

# Copy the deqp runner script and metadata.
cp .gitlab-ci/deqp-runner.sh $rootfs_dst/deqp/
cp .gitlab-ci/$DEQP_SKIPS $rootfs_dst/$CI_PROJECT_DIR/install/deqp-skips.txt
if [ -n "$DEQP_EXPECTED_FAILS" ]; then
  cp .gitlab-ci/$DEQP_EXPECTED_FAILS $rootfs_dst/$CI_PROJECT_DIR/install/deqp-expected-fails.txt
fi
