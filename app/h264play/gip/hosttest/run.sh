#!/bin/sh
# Host test for the libxenon GIP driver (gcc + ASan/UBSan on Linux).
# usage: run.sh [libxenon-root]   (default ~/libxenon-gip-src/libxenon, i.e. a tree with the patch applied)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
L=${1:-$HOME/libxenon-gip-src/libxenon}
S=$L/drivers
OUT=${TMPDIR:-/tmp}/test_gip.$$
gcc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -D_CFE_=1 -DGIP_HOSTTEST=1 $GIPFLAGS \
    -I "$HERE" -I "$S" -I "$S/usb" \
    "$HERE/test_gip.c" "$S/usb/gip_proto.c" "$S/usb/usbgip.c" "$S/usb/usblog.c" "$S/usb/usbdevs.c" "$S/input/input.c" \
    -o "$OUT"
set +e
"$OUT" > "$OUT.log" 2>&1; rc=$?
grep -v '^USB bus\|^\[gip\] t=' "$OUT.log"
rm -f "$OUT" "$OUT.log"
exit $rc
