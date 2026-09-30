#!/bin/sh
# Build the Fusion Controller Magisk module zip.
#
# Requires:
#   - an aarch64 musl cross toolchain (e.g. from musl.cc), extracted anywhere
#   - zip and unzip installed
#
# The toolchain is discovered in this order:
#   1. $TC environment variable (path to the toolchain root dir)
#   2. ./aarch64-linux-musl-cross (extracted next to this script)
#   3. ../aarch64-linux-musl-cross (extracted at repo root)
#
# Usage:  TC=/path/to/aarch64-linux-musl-cross ./build.sh
set -e
cd "$(dirname "$0")"

# locate the cross compiler
if [ -n "$TC" ]; then
    CC="$TC/bin/aarch64-linux-musl-gcc"
elif [ -x "./aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc" ]; then
    TC="./aarch64-linux-musl-cross"
    CC="$TC/bin/aarch64-linux-musl-gcc"
elif [ -x "../aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc" ]; then
    TC="../aarch64-linux-musl-cross"
    CC="$TC/bin/aarch64-linux-musl-gcc"
else
    echo "error: no aarch64 musl toolchain found (set TC=/path/to/aarch64-linux-musl-cross)" >&2
    exit 1
fi
STRIP="$TC/bin/aarch64-linux-musl-strip"

VER=$(sed -n 's/^version=v//p' module.prop)
[ -n "$VER" ] || { echo "error: cannot read version from module.prop" >&2; exit 1; }
echo "Building Fusion Controller v$VER..."

# 1) unit tests on the host
gcc -O2 -Wall -o test_core test_main.c
./test_core

# 2) static aarch64 build
"$CC" -O2 -Wall -Wextra -static -o fusiond fusiond.c
"$STRIP" fusiond

# 3) pack the Magisk module zip
OUT="fusion_controller-v$VER.zip"
rm -f "$OUT"
zip -r -X "$OUT" \
    META-INF \
    module.prop customize.sh service.sh uninstall.sh \
    sepolicy.rule fusiond fusiond-run.sh fusionctl.sh fusion-monitor.sh

echo "built $OUT"
unzip -l "$OUT"