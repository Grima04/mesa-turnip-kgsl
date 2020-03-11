#!/bin/sh

export CCACHE_COMPILERCHECK=content
export CCACHE_COMPRESS=true
export CCACHE_DIR=/cache/mesa/ccache
export PATH=/usr/lib/ccache:$PATH

# CMake ignores $PATH, so we have to force CC/GCC to the ccache versions.
# Watch out, you can't have spaces in here because the renderdoc build fails.
export CC="/usr/lib/ccache/gcc"
export CXX="/usr/lib/ccache/g++"

ccache --show-stats
