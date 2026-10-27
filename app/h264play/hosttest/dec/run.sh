#!/bin/sh
# orbita v0.21.25 — DIAG.TXT do decoder (source/dec), DECODE-PERF.md
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/xc360-dec-hosttest.$$
mkdir -p "$OUT"
gcc -O1 -g -Wall -Wextra -Werror -std=gnu99 -I"$APP/source" -o "$OUT/dec_test" "$HERE/dec_test.c" "$APP/source/dec/xc_decdiag.c"
"$OUT/dec_test"; rc=$?
rm -rf "$OUT"
exit $rc
