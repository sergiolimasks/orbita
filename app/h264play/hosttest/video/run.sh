#!/bin/sh
# orbita — hosttest do modo de vídeo (source/video, VIDEO-1080P.md).
# EDIDs reais em edid/*.hex (série zerada), escolha do modo, timing/scaler,
# e o tools/video/dump2mode.py com dumps SINTÉTICOS (fake_dump.py).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/xc360-video-hosttest.$$
mkdir -p "$OUT"
CF="-O1 -g -Wall -Wextra -Werror -std=gnu99 -I$APP/source"
SRC="$APP/source/video/xc_edid.c $APP/source/video/xc_video_mode.c"
rc=0
# 1) com a tabela 1080p do repo (placeholder ou dump real)
gcc $CF -o "$OUT/video_test" "$HERE/video_test.c" $SRC
"$OUT/video_test" "$HERE/edid" || rc=1
# 2) dump2mode.py: gen com um dump 1080p SINTÉTICO -> tabela usável
LXH=${LIBXENON_SRC:-$HOME/libxenon-gip-src/libxenon}/drivers/xenos/xenos_videomodesdata.h
if [ -f "$LXH" ]; then
    python3 "$HERE/fake_dump.py" 1080p "$LXH" > "$OUT/d1080.txt"
    python3 "$HERE/fake_dump.py" 720p "$LXH" > "$OUT/d720.txt"
    python3 "$APP/tools/video/dump2mode.py" gen "$OUT/d1080.txt" --ref720 "$OUT/d720.txt" --out "$OUT/data.h" 2>"$OUT/gen.err" || rc=1
    gcc $CF -DXCV_EXPECT_DUMP=1 -DXCV_1080P_DATA_FILE="\"$OUT/data.h\"" -o "$OUT/video_test_dump" "$HERE/video_test.c" $SRC
    "$OUT/video_test_dump" "$HERE/edid" || rc=1
    # check720: o 720p sintético É a tabela do libxenon -> 100% igual
    if python3 "$APP/tools/video/dump2mode.py" check720 "$OUT/d720.txt" --libxenon "$LXH" > "$OUT/check.txt"; then
        grep -q "IGUAL" "$OUT/check.txt" && echo "check720 (sintetico): ok" || { cat "$OUT/check.txt"; rc=1; }
    else
        cat "$OUT/check.txt"; rc=1
    fi
    # dump 720p não pode virar tabela 1080p
    if python3 "$APP/tools/video/dump2mode.py" gen "$OUT/d720.txt" --out "$OUT/x.h" 2>/dev/null; then
        echo "FALHOU: gen aceitou um dump 720p"; rc=1
    fi
    # o placeholder gerado tem que ser idêntico ao do repo quando não há dump
    python3 "$APP/tools/video/dump2mode.py" gen --out "$OUT/ph.h" >/dev/null
    if [ "$(grep -c '"sem dump"' "$APP/source/video/xc_video_1080p_data.h")" = 2 ]; then
        cmp -s "$OUT/ph.h" "$APP/source/video/xc_video_1080p_data.h" || { echo "FALHOU: placeholder do repo != gerado"; rc=1; }
    fi
else
    echo "(sem libxenon em $LXH: pulando os testes do dump2mode.py)"
fi
rm -rf "$OUT"
[ $rc -eq 0 ] && echo "VIDEO HOST TESTS PASSED" || echo "VIDEO HOST TESTS FAILED"
exit $rc
