#!/bin/sh

export CCACHE_COMPILERCHECK=content
export CCACHE_COMPRESS=true
export CCACHE_DIR=/cache/mesa/ccache
export PATH=/usr/lib/ccache:$PATH

ccache --show-stats
