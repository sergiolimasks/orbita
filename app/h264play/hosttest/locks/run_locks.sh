#!/bin/sh
# orbita v0.21.14 — locks do app em duas threads (ThreadSanitizer).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
SRC="$APP/source/login"
OUT=${TMPDIR:-/tmp}/xc360-locks-hosttest
MBED=${TMPDIR:-/tmp}/xc360-login-hosttest/mbedtls
mkdir -p "$OUT"
if [ ! -f "$MBED/libmbedtls-xc360.a" ]; then
    HOST=1 OUT="$MBED" "$REPO/deps/build-mbedtls-xenon.sh" >/dev/null
fi
CF="-O1 -g -Wall -Wno-unused-parameter -Wno-sign-compare -I$APP/source -I$SRC -I$MBED/include \
    -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\" -DXC_BUILD_EPOCH=$(date +%s)LL"
COMMON="$APP/source/xc_lock.c $SRC/xc_logup.c $SRC/http_parse.c $SRC/dns_msg.c $SRC/https.c $SRC/ca_roots.c $HERE/../login/port_posix.c"
gcc $CF -fsanitize=thread -o "$OUT/locks_test" "$HERE/locks_test.c" $COMMON "$MBED/libmbedtls-xc360.a" -lpthread
TSAN_OPTIONS="halt_on_error=1" setarch "$(uname -m)" -R "$OUT/locks_test" && echo "LOCKS HOST TESTS PASSED" || { echo "LOCKS HOST TESTS FAILED"; exit 1; }
