#!/bin/sh

# Common setup among container builds before we get to building code.

export CCACHE_COMPILERCHECK=content
export CCACHE_COMPRESS=true
export CCACHE_DIR=/cache/mesa/ccache
export PATH=/usr/lib/ccache:$PATH

# CMake ignores $PATH, so we have to force CC/GCC to the ccache versions.
# Watch out, you can't have spaces in here because the renderdoc build fails.
export CC="/usr/lib/ccache/gcc"
export CXX="/usr/lib/ccache/g++"

ccache --show-stats

# Make a wrapper script for ninja to always include the -j flags
echo '#!/bin/sh -x' > /usr/local/bin/ninja
echo '/usr/bin/ninja -j${FDO_CI_CONCURRENT:-4} "$@"' >> /usr/local/bin/ninja
chmod +x /usr/local/bin/ninja

# Set MAKEFLAGS so that all make invocations in container builds include the
# flags (doesn't apply to non-container builds, but we don't run make there)
export MAKEFLAGS="-j${FDO_CI_CONCURRENT:-4}"
