#!/bin/sh
# XCloud360 stage 3 — testes do host da sessão xCloud (sem rede, GSSV falso).
# Uso: app/h264play/hosttest/session/run_session.sh [unit|xenon|golden|all]  (padrão all)
#   unit:   session_test (gcc + ASan/UBSan, -Wall -Wextra -Werror)
#   xenon:  compila source/session/*.c com o xenon-gcc e as CFLAGS do app
#           (MACHDEP + -g -O2 -Wall + defines do login), conta warnings
#   golden: regenera golden.h a partir do json.hpp do green-nx (precisa g++)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
SRC="$APP/source"
OUT=${TMPDIR:-/tmp}/xc360-session-hosttest
mkdir -p "$OUT"
WHAT=${1:-all}
rc=0

if [ "$WHAT" = golden ]; then
    GNX=${GREEN_NX:-$HOME/green-nx}
    g++ -std=c++17 -I "$GNX/vendor" -o "$OUT/gen_golden" "$HERE/gen_golden.cpp"
    "$OUT/gen_golden" > "$HERE/golden.h"
    echo "golden.h regenerado"
    exit 0
fi

if [ "$WHAT" = unit ] || [ "$WHAT" = all ]; then
    CF="-O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -I$SRC/login"
    gcc $CF -fsanitize=address,undefined -fno-sanitize-recover=undefined -o "$OUT/session_test" \
        "$HERE/session_test.c" "$HERE/mock_port.c" \
        "$SRC/session/xc_session.c" "$SRC/session/xs_machine.c" \
        "$SRC/login/xbox_auth.c" "$SRC/login/cjson/cJSON.c" -lpthread
    "$OUT/session_test" || rc=1
fi

if [ "$WHAT" = xenon ] || [ "$WHAT" = all ]; then
    DEVKITXENON=${DEVKITXENON:-$HOME/devkitxenon}
    export PATH="$DEVKITXENON/bin:$DEVKITXENON/usr/bin:$PATH"
    if ! command -v xenon-gcc >/dev/null 2>&1; then
        echo "xenon-gcc nao encontrado (source scripts/env.sh)"; exit 1
    fi
    MBEDTLS_DIR=${MBEDTLS_DIR:-$HOME/mbedtls-xenon}
    # = Makefile: MACHDEP do $(DEVKITXENON)/rules + CFLAGS base + LOGIN_CFLAGS
    MACHDEP="-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float"
    XCF="-g -O2 -Wall $MACHDEP -I$DEVKITXENON/usr/include -DXCLOUD360_MENU=1 -DXCLOUD360_LOGIN=1 \
         -I$MBEDTLS_DIR/include -DMBEDTLS_CONFIG_FILE=\"xc360_mbedtls_config.h\" -DXC_BUILD_EPOCH=$(date +%s)LL"
    : > "$OUT/xenon.log"
    for f in xc_session xs_machine xs_task xs_port_xenon; do
        xenon-gcc $XCF ${XENON_EXTRA:-} -c "$SRC/session/$f.c" -o "$OUT/$f.o" 2>>"$OUT/xenon.log" || rc=1
    done
    W=$(grep -c "warning:" "$OUT/xenon.log" || true)
    E=$(grep -c "error:" "$OUT/xenon.log" || true)
    cat "$OUT/xenon.log"
    xenon-size "$OUT"/xc_session.o "$OUT"/xs_machine.o "$OUT"/xs_task.o "$OUT"/xs_port_xenon.o 2>/dev/null || true
    echo "xenon-gcc: $W warnings, $E erros"
    [ "$W" = 0 ] && [ "$E" = 0 ] || rc=1
fi

[ $rc -eq 0 ] && echo "SESSION TESTS PASSED" || echo "SESSION TESTS FAILED"
exit $rc
