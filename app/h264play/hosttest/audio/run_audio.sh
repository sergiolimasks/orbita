#!/bin/bash
# XCloud360 v0.18 — host tests of the Xbox audio core.
#   source/audio/aud_core.c (the file the ELF builds) + libopus built for the
#   host from the same 1.5.2 source (deps/build-opus-xenon.sh), float and fixed.
#
# usage: hosttest/audio/run_audio.sh        run everything (exit 1 on failure)
#        hosttest/audio/run_audio.sh gen    regenerate source/audio/aud_tone_stream.c
#        hosttest/audio/run_audio.sh bench  host decode cost per 20 ms frame
set -e
H=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$H/../.." && pwd)
REPO=$(cd "$APP/../.." && pwd)
OUT=${OPUS_OUT:-$HOME/opus-xenon}
[ -f "$OUT/host/libopus-float.a" ] || { echo "no $OUT/host/libopus-float.a: run deps/build-opus-xenon.sh"; exit 1; }
B=$(mktemp -d "${TMPDIR:-/tmp}/xc360-audio.XXXXXX")
trap 'rm -rf "$B"' EXIT
INC="-I$OUT/include -I$APP/source/audio"
CW="-O2 -g -Wall -Wextra"

gcc $CW $INC "$H/gen_tone.c" "$OUT/host/libopus-float.a" -lm -o "$B/gen_tone"
"$B/gen_tone" "$B/aud_tone_stream.c" "$B/tone.pkts" 2>/dev/null
if [ "${1:-}" = gen ]; then
    cp "$B/aud_tone_stream.c" "$APP/source/audio/aud_tone_stream.c"
    echo "regenerated source/audio/aud_tone_stream.c"
    exit 0
fi
if cmp -s "$B/aud_tone_stream.c" "$APP/source/audio/aud_tone_stream.c"; then
    echo "[tone] source/audio/aud_tone_stream.c matches a fresh encode"
else
    echo "[tone] WARN: source/audio/aud_tone_stream.c differs from a fresh encode ($0 gen)"
fi

# Real xCloud audio, if some capture-rtp run kept audio.opus.raw (not in git).
for f in $(ls -t "$REPO"/tools/xcloud-capture/output/*/audio.opus.raw 2>/dev/null); do
    if [ -s "$f" ] && python3 "$H/split_rtp_raw.py" "$f" "$B/xcloud.pkts" >/dev/null; then
        echo "[xcloud] vector from $f"
        break
    fi
done

rc=0
for v in ${VARIANTS:-float fixed}; do
    gcc $CW $INC "$H/aud_test.c" "$APP/source/audio/aud_core.c" "$OUT/host/libopus-$v.a" \
        -lm -lpthread -o "$B/aud_test-$v"
    if [ "${1:-}" = bench ]; then
        for p in "$B"/*.pkts; do "$B/aud_test-$v" bench "$p"; done
        continue
    fi
    echo "=== libopus $v"
    "$B/aud_test-$v" "$B" || rc=1
done
exit $rc
