#!/usr/bin/env bash
# XCloud360 stage 4 — build the host-side test programs against the WebRTC
# libraries from deps/webrtc-xenon/build.sh.
#   build_host.sh [host|ppc]     -> $OUT/{e2e_recv,jitter_test,proto_test}
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$(cd "$HERE/../.." && pwd)"
T="${1:-host}"
W="${OUTROOT:-$HOME/webrtc-xenon}/$T"
OUT="${OUT:-$W/test}"
mkdir -p "$OUT"
case "$T" in
  host) CC=gcc; FLAGS="-O2 -g -Wall -Wextra -Wno-unused-parameter" ;;
  ppc)  source "$HOME/scratch/wf/ppc-tools/env-ppc.sh"
        CC="$PPC_CC"; FLAGS="-O2 -g -Wall -m32 -mcpu=7450 -mno-altivec --sysroot=$PPC_SYSROOT -static" ;;
  *) echo "target?"; exit 2 ;;
esac
SRC="$APP/source/webrtc"
INC="-I$SRC -I$W/include -I$APP/source/session -I$APP/source/login -I$APP/source"
LIBS="-L$W/lib -lpeer-xc -lusrsctp-xc -lsrtp2-xc -lmbedtls-xcwebrtc -lpthread -lm"
$CC $FLAGS $INC -o "$OUT/e2e_recv" "$HERE/e2e_recv.c" "$HERE/xc_udp_posix.c" \
    "$SRC/xc_webrtc.c" "$SRC/xc_webrtc_mt.c" "$SRC/xc_webrtc_xsm.c" "$HERE/fake_xsm.c" \
    "$SRC/xc_jitter.c" "$SRC/xc_proto.c" $LIBS
$CC $FLAGS -I"$SRC" -o "$OUT/proto_test" "$HERE/proto_test.c" "$SRC/xc_proto.c" -lm
if [[ $T == host ]]; then   # differential test vs the C++ jitter buffer capture-rtp uses
  REPO="$(cd "$APP/../.." && pwd)"
  gcc -O2 -g -c -I"$SRC" -o "$OUT/xc_jitter.o" "$SRC/xc_jitter.c"
  g++ -O2 -g -std=c++17 -I"$SRC" -I"$REPO/tools/xcloud-capture/rtp" -o "$OUT/jitter_test"       "$HERE/jitter_test.cpp" "$REPO/tools/xcloud-capture/rtp/video_jitter.cpp" "$OUT/xc_jitter.o"
fi
echo "built $T: $(ls "$OUT")"
