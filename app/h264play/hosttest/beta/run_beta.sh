#!/bin/sh
# XCloud360 / orbita v0.20.3 — testes do host da chave beta (source/beta).
# Uso: app/h264play/hosttest/beta/run_beta.sh [unit|fake|live|all]   (padrão: unit + fake)
#   unit: transporte falso, ASan+UBSan (sem rede)
#   fake: ponte FAKE local (fake_server.py, HTTP puro) com o contrato novo
#   live: x360.pistatrack.com pelo NOSSO https.c com a chave de DEV
#         (~/credentials/xc360-logs-key, só pelo ambiente; não vincula console)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
SRC="$APP/source/login"
OUT=${TMPDIR:-/tmp}/xc360-beta-hosttest
MBED=${TMPDIR:-/tmp}/xc360-login-hosttest/mbedtls
mkdir -p "$OUT"
WHAT=${1:-default}
if [ ! -f "$MBED/libmbedtls-xc360.a" ]; then
    HOST=1 OUT="$MBED" "$REPO/deps/build-mbedtls-xenon.sh" >/dev/null
fi
EPOCH=$(date +%s)
CF="-O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I$SRC -I$MBED/include \
    -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\" -DXC_BUILD_EPOCH=${EPOCH}LL"
SRCS="$APP/source/beta/xc_beta.c $APP/source/beta/xc_keypad.c $SRC/https.c $SRC/http_parse.c $SRC/dns_msg.c \
      $SRC/ca_roots.c $HERE/../login/port_posix.c"
gcc $CF -fsanitize=address,undefined -o "$OUT/beta_unit" "$HERE/beta_unit.c" $SRCS "$MBED/libmbedtls-xc360.a" -lpthread
rc=0
if [ "$WHAT" = unit ] || [ "$WHAT" = all ] || [ "$WHAT" = default ]; then
    rm -rf "$OUT/store"
    "$OUT/beta_unit" unit "$OUT/store" || rc=1
fi
if [ "$WHAT" = fake ] || [ "$WHAT" = all ] || [ "$WHAT" = default ]; then
    PORT=$((18000 + $$ % 1000))
    python3 -I "$HERE/fake_server.py" $PORT > "$OUT/fake.log" 2>&1 &
    SPID=$!
    sleep 0.6
    rm -rf "$OUT/store"
    "$OUT/beta_unit" fake "$OUT/store" $PORT || rc=1
    kill $SPID 2>/dev/null || true
fi
if [ "$WHAT" = live ] || [ "$WHAT" = all ]; then
    KEYF="$HOME/credentials/xc360-logs-key"
    [ -f "$KEYF" ] || { echo "sem $KEYF"; exit 1; }
    rm -rf "$OUT/live"
    echo "--- live (hora do host)"
    XCB_LIVE_KEY=$(tr -d ' \r\n' < "$KEYF") "$OUT/beta_unit" live "$OUT/live" || rc=1
    echo "--- live (sem hora: bootstrap pelo Date, como no Xbox)"
    XCB_LIVE_KEY=$(tr -d ' \r\n' < "$KEYF") XC_TIME_UNKNOWN=1 "$OUT/beta_unit" live "$OUT/live" || rc=1
    rm -rf "$OUT/live"
fi
[ $rc -eq 0 ] && echo "BETA HOST TESTS PASSED" || echo "BETA HOST TESTS FAILED"
exit $rc
