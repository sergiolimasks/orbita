#!/usr/bin/env bash
# XCloud360 stage 4 — every offline WebRTC check, in order:
#   1. xc_proto.c vs green-nx C++ (x86-64 + ppc32 BE)
#   2. xc_jitter.c vs the C++ VideoJitterBuffer (differential, random traces)
#   3. E2E receiver vs pion: host + ppc32-BE(qemu), no loss / 3% loss (each
#      packet lost at most once -> must be fully repaired by NACK) /
#      3% independent loss (double losses -> green-nx drop + IDR resync)
#   4. host bench (scaling reference for the Xbox bench ELF)
# usage: run_all.sh [seconds per E2E run, default 12]
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
S="${1:-12}"
W="${OUTROOT:-$HOME/webrtc-xenon}"
for t in host ppc; do [[ -f "$W/$t/lib/libpeer-xc.a" ]] || "$REPO/deps/webrtc-xenon/build.sh" "$t" > /dev/null; done
"$HERE/build_host.sh" host > /dev/null
declare -a R
echo "### 1. protocol golden"; "$HERE/proto_golden.sh"; R+=("proto_golden: $?")
echo "### 2. jitter differential"; "$W/host/test/jitter_test" 300; R+=("jitter_test: $?")
for t in host ppc; do
  for mode in "0" "0.03 once" "0.03" "0.02 mt" "0.02 xsm"; do
    set -- $mode
    if [[ "${2:-}" == xsm ]]; then
      out="$(XCW_XSM=1 "$HERE/run_e2e.sh" "$t" "$1" "$S" 2>&1)"
      echo "$out" | grep "xsm events:"
    elif [[ "${2:-}" == mt ]]; then
      out="$(XCW_MT=1 "$HERE/run_e2e.sh" "$t" "$1" "$S" 2>&1)"
    elif [[ "${2:-}" == once ]]; then
      out="$(XCW_LOSS_ONCE=1 "$HERE/run_e2e.sh" "$t" "$1" "$S" 2>&1)"
    else
      out="$("$HERE/run_e2e.sh" "$t" "$1" "$S" 2>&1)"
    fi
    echo "$out" | grep -E "^(===|video|audio|nack|setup|rtcp|input|rtt|memory|cpu|VERDICT)"
    R+=("e2e $t loss=$mode: $(echo "$out" | grep VERDICT)")
  done
done
echo "### 4. host bench"; "$REPO/deps/webrtc-xenon/build-bench.sh" host > /dev/null 2>&1; R+=("host bench: $?")
echo; echo "### SUMMARY"; printf '  %s\n' "${R[@]}"
