#!/bin/bash
# XCloud360 v0.18 — libopus 1.5.2 (BSD-3-Clause, xiph.org) static for the
# Xenon, plus host builds for app/h264play/hosttest/audio.
#
#   ~/opus-xenon/libopus-float.a   libopus-fixed.a   (xenon-gcc, -mno-altivec)
#   ~/opus-xenon/host/libopus-{float,fixed}.a        (gcc, host tests)
#   ~/opus-xenon/include/*.h, COPYING
#
# No SIMD of any kind: deps/opus-xenon.mk compiles only the generic C sources
# from upstream's *.mk lists (no x86/arm/mips dirs, no RTCD) and the Xenon
# build uses -mno-altivec (Xenon lacks 22 AltiVec integer ops). The result is
# checked with vmxaudit.py. No DNN code (DRED/OSCE/deep PLC are off).
#
# usage: deps/build-opus-xenon.sh     (OPUS_SRC=..., OPUS_OUT=..., J=2)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SRC=${OPUS_SRC:-$HOME/opus-src}
OUT=${OPUS_OUT:-$HOME/opus-xenon}
TAG=v1.5.2
COMMIT=ddbe48383984d56acd9e1ab6a090c54ca6b735a6
J=${J:-2}

if [ ! -d "$SRC/.git" ]; then
    git clone -q --depth 1 --branch $TAG https://github.com/xiph/opus.git "$SRC"
fi
got=$(git -C "$SRC" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "[opus] $SRC is at $got, want $TAG ($COMMIT)"; exit 1; }

source "$REPO/scripts/env.sh" >/dev/null
XEN="-DXENON -m32 -mno-altivec -fno-pic -mpowerpc64 -mhard-float -O2 -ffunction-sections -fdata-sections"
mkdir -p "$OUT/host" "$OUT/include"
for v in float fixed; do
    F=0; [ $v = fixed ] && F=1
    nice -n 10 make -s -j$J -f "$HERE/opus-xenon.mk" OPUS_SRC="$SRC" OUT="$OUT/build-xenon-$v" \
        CC=xenon-gcc AR=xenon-ar XCFLAGS="$XEN" FIXED=$F
    cp "$OUT/build-xenon-$v/libopus.a" "$OUT/libopus-$v.a"
    nice -n 10 make -s -j$J -f "$HERE/opus-xenon.mk" OPUS_SRC="$SRC" OUT="$OUT/build-host-$v" \
        CC=gcc AR=ar XCFLAGS="-O2 -g" FIXED=$F
    cp "$OUT/build-host-$v/libopus.a" "$OUT/host/libopus-$v.a"
done
cp "$SRC"/include/*.h "$OUT/include/"
cp "$SRC/COPYING" "$OUT/"
ls -la "$OUT"/libopus-*.a "$OUT"/host/libopus-*.a

AUDIT=${VMXAUDIT:-$HOME/ffmpeg-xenon-mt-altivec/xcloud360/vmxaudit.py}
if [ -f "$AUDIT" ]; then
    python3 "$AUDIT" "$OUT/libopus-float.a" "$OUT/libopus-fixed.a" | tail -3
else
    echo "[opus] vmxaudit.py not found ($AUDIT) - audit the final ELF"
fi
