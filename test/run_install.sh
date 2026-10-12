#!/bin/bash
#
# Install libmodbusmq into a throwaway prefix and build test/consumer against
# it with find_package, the way pxos and other downstream projects use it.
#
# usage: run_install.sh <build dir>
#
set -e

BUILD=$(realpath "$1")
SRC=$(realpath "$(dirname "$0")/..")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

#
# DESTDIR rather than --prefix: modbusmq.pc carries the configured prefix, as
# pkg-config files do, and a staged install read back through
# PKG_CONFIG_SYSROOT_DIR is exactly how a Yocto sysroot consumes it.
#
PREFIX=$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$BUILD/CMakeCache.txt")
DESTDIR="$WORK/root" cmake --install "$BUILD" > /dev/null
STAGE="$WORK/root$PREFIX"

cmake -S "$SRC/test/consumer" -B "$WORK/build" -DCMAKE_PREFIX_PATH="$STAGE" > /dev/null
cmake --build "$WORK/build" > /dev/null

"$WORK/build/consumer_static"
LD_LIBRARY_PATH="$STAGE/lib:$STAGE/lib64:$LD_LIBRARY_PATH" "$WORK/build/consumer_shared"

# the archive must not need the shared lib at run time
if ldd "$WORK/build/consumer_static" | grep -q libmodbusmq
then
    echo "FAIL: consumer_static links libmodbusmq.so"
    exit 1
fi

# pkg-config: headers from --cflags, the archive from libdir, and whatever
# it needs from Libs.private
export PKG_CONFIG_SYSROOT_DIR="$WORK/root"
export PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig:$STAGE/lib64/pkgconfig"
LIBDIR=$(pkg-config --variable=libdir modbusmq)
cc -o "$WORK/consumer_pc" "$SRC/test/consumer/consumer.c" \
   $(pkg-config --cflags modbusmq) "$LIBDIR/libmodbusmq.a" \
   $(pkg-config --static --libs-only-l modbusmq | sed 's/-lmodbusmq//')
"$WORK/consumer_pc"

echo "install test passed"
