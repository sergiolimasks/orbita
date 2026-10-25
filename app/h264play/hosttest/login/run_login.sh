#!/bin/sh
# XCloud360 v0.17 — testes do host do login (HTTP/TLS/auth no Linux).
# Uso: app/h264play/hosttest/login/run_login.sh [unit|live|all]   (padrão all)
#   unit: parser HTTP/chunked, DNS, JSON de títulos/nomes (sem rede)
#   live: DNS+TLS+verificação nos hosts reais, GET displaycatalog, device code
#         (rodado 2x: com hora do host e com XC_TIME_UNKNOWN=1, como o Xbox)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
SRC="$APP/source/login"
OUT=${TMPDIR:-/tmp}/xc360-login-hosttest
MBED="$OUT/mbedtls"
mkdir -p "$OUT"
WHAT=${1:-all}

if [ ! -f "$MBED/libmbedtls-xc360.a" ]; then
    HOST=1 OUT="$MBED" "$REPO/deps/build-mbedtls-xenon.sh" >/dev/null
fi
EPOCH=$(date +%s)
CF="-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I$SRC -I$MBED/include \
    -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\" -DXC_BUILD_EPOCH=${EPOCH}LL"
COMMON="$SRC/http_parse.c $SRC/dns_msg.c $SRC/xbox_auth.c $SRC/cjson/cJSON.c"

rc=0
if [ "$WHAT" = unit ] || [ "$WHAT" = all ]; then
    gcc $CF -fsanitize=address,undefined -o "$OUT/login_unit" "$HERE/login_unit.c" $COMMON \
        "$HERE/port_posix.c" "$SRC/https.c" "$SRC/ca_roots.c" "$MBED/libmbedtls-xc360.a"
    "$OUT/login_unit" "${CATALOG_JSON:-$HOME/.config/xcloud360/catalog.json}" || rc=1
fi
if [ "$WHAT" = live ] || [ "$WHAT" = all ]; then
    gcc $CF -o "$OUT/login_live" "$HERE/login_live.c" $COMMON "$HERE/port_posix.c" \
        "$SRC/https.c" "$SRC/ca_roots.c" "$MBED/libmbedtls-xc360.a"
    echo "--- live (hora do host)"
    XC_STORE="$OUT/store" "$OUT/login_live" || rc=1
    echo "--- live (sem hora: bootstrap pelo Date, como no Xbox)"
    XC_STORE="$OUT/store" XC_TIME_UNKNOWN=1 "$OUT/login_live" nodc || rc=1
fi
[ $rc -eq 0 ] && echo "LOGIN HOST TESTS PASSED" || echo "LOGIN HOST TESTS FAILED"
exit $rc
