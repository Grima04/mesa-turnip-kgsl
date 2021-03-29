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
        telnet \
        unzip \
        wget

# setup nginx
sed -i '/gzip_/ s/#\ //g' /etc/nginx/nginx.conf
cp .gitlab-ci/bare-metal/nginx-default-site  /etc/nginx/sites-enabled/default
