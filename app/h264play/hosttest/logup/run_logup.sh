#!/bin/sh
# XCloud360 v0.20.1 — testes do host do log beta (source/login/xc_logup.c).
# Uso: app/h264play/hosttest/logup/run_logup.sh [unit|live|all]   (padrão unit)
#   unit: transporte e relógio falsos, ASan+UBSan (sem rede)
#   live: 1 lote real pra x360.pistatrack.com pelo NOSSO https.c, com hora
#         do host e sem hora (XC_TIME_UNKNOWN=1, bootstrap pelo Date, como no
#         Xbox). Chave: ~/credentials/xc360-logs-key (só aqui, via ambiente).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
SRC="$APP/source/login"
OUT=${TMPDIR:-/tmp}/xc360-logup-hosttest
MBED=${TMPDIR:-/tmp}/xc360-login-hosttest/mbedtls
mkdir -p "$OUT"
WHAT=${1:-unit}
if [ ! -f "$MBED/libmbedtls-xc360.a" ]; then
    HOST=1 OUT="$MBED" "$REPO/deps/build-mbedtls-xenon.sh" >/dev/null
fi
EPOCH=$(date +%s)
CF="-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I$SRC -I$MBED/include \
    -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\" -DXC_BUILD_EPOCH=${EPOCH}LL"
COMMON="$SRC/xc_logup.c $SRC/http_parse.c $SRC/dns_msg.c $SRC/https.c $SRC/ca_roots.c $HERE/../login/port_posix.c"
rc=0
if [ "$WHAT" = unit ] || [ "$WHAT" = all ]; then
    rm -rf "$OUT/store"
    gcc $CF -fsanitize=address,undefined -o "$OUT/logup_unit" "$HERE/logup_unit.c" $COMMON \
        "$MBED/libmbedtls-xc360.a" -lpthread
    "$OUT/logup_unit" "$OUT/store" || rc=1
fi
if [ "$WHAT" = live ] || [ "$WHAT" = all ]; then
    KEYF="$HOME/credentials/xc360-logs-key"
    [ -f "$KEYF" ] || { echo "sem $KEYF"; exit 1; }
    gcc $CF -o "$OUT/logup_live" "$HERE/logup_live.c" $COMMON "$MBED/libmbedtls-xc360.a" -lpthread
    echo "--- live (hora do host)"
    rm -rf "$OUT/live"
    XLU_LIVE_KEY=$(tr -d ' \r\n' < "$KEYF") "$OUT/logup_live" "$OUT/live" || rc=1
    echo "--- live (sem hora: bootstrap pelo Date, como no Xbox)"
    rm -rf "$OUT/live"
    XLU_LIVE_KEY=$(tr -d ' \r\n' < "$KEYF") XC_TIME_UNKNOWN=1 "$OUT/logup_live" "$OUT/live" || rc=1
    rm -rf "$OUT/live"
fi
[ $rc -eq 0 ] && echo "LOGUP HOST TESTS PASSED" || echo "LOGUP HOST TESTS FAILED"
exit $rc
