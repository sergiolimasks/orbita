#!/usr/bin/env bash
# XCloud360 stage 4 — offline end-to-end test of the Xbox-side WebRTC receiver
# against a local pion "xCloud" (NO real xCloud, no accounts/tokens).
#
#   run_e2e.sh [target=host|ppc] [loss=0] [seconds=15] [h264 file]
#
#   host = x86-64 build; ppc = ppc32 BIG-endian build under qemu-ppc (same
#   byte order as Xenon). loss = fraction of incoming video RTP packets the
#   receiver's UDP layer drops before SRTP (exercises NACK/retransmission).
# Signaling goes through files in the run directory (offer/answer/cands).
# Uses ephemeral UDP ports on 127.0.0.1 only (never 9000-9004).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
T="${1:-host}"
LOSS="${2:-0}"
SECS="${3:-15}"
H264="${4:-$REPO/tools/xcloud-capture/output/GOLDEN-synth-720p60-baseline-5s/stream.h264}"
W="${OUTROOT:-$HOME/webrtc-xenon}"
GO="$W/tools/go/bin/go"
export GOPATH="$W/tools/gopath" GOMODCACHE="$W/tools/gopath/pkg/mod" GOFLAGS=-mod=mod

[[ -f "$W/$T/lib/libpeer-xc.a" ]] || "$REPO/deps/webrtc-xenon/build.sh" "$T"
"$HERE/build_host.sh" "$T" > /dev/null
PION="$W/tools/peer_pion"
if [[ ! -x "$PION" || "$HERE/peer_pion/main.go" -nt "$PION" ]]; then
  (cd "$HERE/peer_pion" && "$GO" mod tidy >/dev/null 2>&1 && "$GO" build -o "$PION" .)
fi

RUN="$W/e2e/$T-loss$LOSS${XCW_LOSS_ONCE:+once}${XCW_MT:+-mt}${XCW_XSM:+-xsm}-$(date +%H%M%S)"
mkdir -p "$RUN"
echo "=== E2E $T loss=$LOSS${XCW_LOSS_ONCE:+ (each seq dropped at most once)} ${SECS}s stream=$(basename "$(dirname "$H264")")/$(basename "$H264") -> $RUN"
"$PION" -dir "$RUN" -h264 "$H264" -fps "${PION_FPS:-30}" -seconds $((SECS + 30)) > "$RUN/pion.log" 2>&1 &
PPID_PION=$!
RECV="$W/$T/test/e2e_recv"
set +e
if [[ $T == ppc ]]; then
  source "$HOME/scratch/wf/ppc-tools/env-ppc.sh"
  XCW_LOSS="$LOSS" XCW_LOSS_AFTER="${XCW_LOSS_AFTER:-300}" "$QEMU_PPC" -cpu 7450 "$RECV" --dir "$RUN" --seconds "$SECS" ${XCW_MT:+--mt} ${XCW_XSM:+--xsm} > "$RUN/recv.log" 2>&1
else
  XCW_LOSS="$LOSS" XCW_LOSS_AFTER="${XCW_LOSS_AFTER:-300}" /usr/bin/time -o "$RUN/recv.time" -f "%U %S %e"       "$RECV" --dir "$RUN" --seconds "$SECS" ${XCW_MT:+--mt} ${XCW_XSM:+--xsm} > "$RUN/recv.log" 2>&1
fi
RRC=$?
wait "$PPID_PION"
set -e
grep -E "\[e2e\] (offer|answer|done|xsm events:)|state ->|handshake done|SPS|PPS" "$RUN/recv.log" | head -20 || true
tail -2 "$RUN/recv.log" | head -1
python3 "$HERE/check_e2e.py" "$RUN" "$LOSS"
