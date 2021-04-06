#!/bin/bash

set -e
set -o xtrace

############### Install packages for baremetal testing
apt-get install -y ca-certificates
sed -i -e 's/http:\/\/deb/https:\/\/deb/g' /etc/apt/sources.list
apt-get update

apt-get install -y --no-remove \
        abootimg \
        cpio \
        fastboot \
        netcat \
        nginx-full \
        procps \
        python3-distutils \
        python3-minimal \
        python3-serial \
        python3.7 \
        rsync \
        snmp \
        unzip \
        wget

# setup nginx
sed -i '/gzip_/ s/#\ //g' /etc/nginx/nginx.conf
cp .gitlab-ci/bare-metal/nginx-default-site  /etc/nginx/sites-enabled/default

# setup SNMPv2 SMI MIB
wget https://raw.githubusercontent.com/net-snmp/net-snmp/master/mibs/SNMPv2-SMI.txt \
    -O /usr/share/snmp/mibs/SNMPv2-SMI.txt

arch=arm64 . .gitlab-ci/container/baremetal_build.sh
arch=armhf . .gitlab-ci/container/baremetal_build.sh
