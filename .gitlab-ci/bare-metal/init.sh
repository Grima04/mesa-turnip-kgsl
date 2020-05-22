#!/bin/sh

set -ex

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev || echo possibly already mounted
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts

. /set-job-env-vars.sh

echo "nameserver 8.8.8.8" > /etc/resolv.conf

export DEQP_SKIPS=deqp-skips.txt
if [ -e /install/deqp-expected-fails.txt ]; then
  export DEQP_EXPECTED_FAILS=deqp-expected-fails.txt
fi

if sh /deqp/deqp-runner.sh; then
    echo "DEQP RESULT: pass"
else
    echo "DEQP RESULT: fail"
fi

# Wait until the job would have timed out anyway, so we don't spew a "init
# exited" panic.
sleep 6000
