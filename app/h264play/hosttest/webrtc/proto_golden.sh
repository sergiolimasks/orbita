#!/usr/bin/env bash
# XCloud360 stage 4 — xc_proto.c vs the original green-nx C++, byte for byte,
# on x86-64 AND on ppc32 big-endian (qemu): the C port writes the
# little-endian input reports explicitly, so both must match the LE original.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../source/webrtc"
GNX="${GREEN_NX:-$HOME/green-nx}"
T="$(mktemp -d)"
g++ -O1 -std=c++17 -I"$GNX/src/core" -I"$GNX/vendor" -I"$HERE" -o "$T/golden" \
    "$HERE/proto_golden.cpp" "$GNX/src/core/xcloud_protocol.cpp"
gcc -O2 -Wall -I"$SRC" -I"$HERE" -o "$T/c_host" "$HERE/proto_test.c" "$SRC/xc_proto.c"
"$T/golden" > "$T/golden.txt"
"$T/c_host" > "$T/c_host.txt"
rc=0
if diff -u "$T/golden.txt" "$T/c_host.txt" > "$T/diff_host.txt"; then
  echo "proto host (x86-64 LE): IDENTICAL to green-nx ($(wc -l < "$T/golden.txt") lines)"
else
  echo "proto host: DIFF"; cat "$T/diff_host.txt"; rc=1
fi
if [[ -f "$HOME/scratch/wf/ppc-tools/env-ppc.sh" ]]; then
  # shellcheck disable=SC1091
  source "$HOME/scratch/wf/ppc-tools/env-ppc.sh"
  "$PPC_CC" -O2 -m32 -mcpu=7450 -mno-altivec --sysroot="$PPC_SYSROOT" -static -I"$SRC" -I"$HERE" \
      -o "$T/c_ppc" "$HERE/proto_test.c" "$SRC/xc_proto.c"
  "$QEMU_PPC" -cpu 7450 "$T/c_ppc" > "$T/c_ppc.txt"
  if diff -u "$T/golden.txt" "$T/c_ppc.txt" > "$T/diff_ppc.txt"; then
    echo "proto ppc32 BE (qemu): IDENTICAL to green-nx"
  else
    echo "proto ppc: DIFF"; cat "$T/diff_ppc.txt"; rc=1
  fi
fi
rm -rf "$T"
exit $rc
