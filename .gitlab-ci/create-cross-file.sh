#!/bin/bash

arch=$1
cross_file="/cross_file-$arch.txt"
/usr/share/meson/debcrossgen --arch $arch -o "$cross_file"
# Explicitly set ccache path for cross compilers
sed -i "s|/usr/bin/\([^-]*\)-linux-gnu\([^-]*\)-g|/usr/lib/ccache/\\1-linux-gnu\\2-g|g" "$cross_file"
if [ "$arch" = "i386" ]; then
    # Work around a bug in debcrossgen that should be fixed in the next release
    sed -i "s|cpu_family = 'i686'|cpu_family = 'x86'|g" "$cross_file"
fi
# Rely on qemu-user being configured in binfmt_misc on the host
sed -i -e '/\[properties\]/a\' -e "needs_exe_wrapper = False" "$cross_file"
