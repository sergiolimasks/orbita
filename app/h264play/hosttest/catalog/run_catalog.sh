#!/bin/sh
# XCloud360 — testes do host da camada de capas (source/catalog).
# Uso: app/h264play/hosttest/catalog/run_catalog.sh [unit|live|all]   (padrão unit)
#   unit: fixtures gravadas, fetch falso, ASan+UBSan (sem rede)
#   live: SIGL/displaycatalog/store-images de verdade pelo NOSSO HTTPS
#         (só endpoints públicos, sem token), com hora do host e sem hora
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
SRC="$APP/source/login"
CAT="$APP/source/catalog"
OUT=${TMPDIR:-/tmp}/xc360-catalog-hosttest
MBED=${TMPDIR:-/tmp}/xc360-login-hosttest/mbedtls
mkdir -p "$OUT"
WHAT=${1:-unit}

if [ ! -f "$MBED/libmbedtls-xc360.a" ]; then
    HOST=1 OUT="$MBED" "$REPO/deps/build-mbedtls-xenon.sh" >/dev/null
fi
EPOCH=$(date +%s)
CF="-DXWD_HOSTTEST -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I$SRC -I$MBED/include \
    -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\" -DXC_BUILD_EPOCH=${EPOCH}LL -DXCC_NO_LOGIN_HOOKS=1"
COMMON="$SRC/http_parse.c $SRC/dns_msg.c $SRC/xbox_auth.c $SRC/cjson/cJSON.c $SRC/https.c $SRC/ca_roots.c \
    $HERE/../login/port_posix.c $HERE/xcc_port_posix.c \
    $CAT/xc_catalog.c $CAT/xcc_parse.c $CAT/xcc_image.c $CAT/xcc_net.c $APP/source/xc_wdog.c"

rc=0
if [ "$WHAT" = unit ] || [ "$WHAT" = all ]; then
    rm -rf "$OUT/store"
    gcc $CF -fsanitize=address,undefined -o "$OUT/catalog_unit" "$HERE/catalog_unit.c" $COMMON \
        "$MBED/libmbedtls-xc360.a" -lpthread
    "$OUT/catalog_unit" "$HERE/fixtures" "$OUT/store" || rc=1
fi
if [ "$WHAT" = live ] || [ "$WHAT" = all ]; then
    gcc $CF -o "$OUT/catalog_live" "$HERE/catalog_live.c" $COMMON "$MBED/libmbedtls-xc360.a" -lpthread
    echo "--- live (hora do host)"
    rm -rf "$OUT/live"; mkdir -p "$OUT/live"
    "$OUT/catalog_live" "$HERE/fixtures" "$OUT/live" || rc=1
    echo "--- live (sem hora: bootstrap pelo Date, como no Xbox)"
    rm -rf "$OUT/live"; mkdir -p "$OUT/live"
    XC_TIME_UNKNOWN=1 "$OUT/catalog_live" "$HERE/fixtures" "$OUT/live" || rc=1
fi
[ $rc -eq 0 ] && echo "CATALOG HOST TESTS PASSED" || echo "CATALOG HOST TESTS FAILED"
exit $rc
