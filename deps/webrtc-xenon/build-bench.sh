#!/usr/bin/env bash
# XCloud360 stage 4 — WebRTC CPU benchmark (source/webrtc/bench/webrtc_bench.c)
#   build-bench.sh xenon  -> app/h264play/variants/webrtc-bench.elf32 (+ .sha256)
#   build-bench.sh host   -> ~/webrtc-xenon/host/test/webrtc_bench  (and runs it)
# Needs deps/webrtc-xenon/build.sh <target> first.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
T="${1:-xenon}"
W="${OUTROOT:-$HOME/webrtc-xenon}/$T"
DKX="${DEVKITXENON:-$HOME/devkitxenon}"
SRC="$REPO/app/h264play/source/webrtc"
MBED="$HOME/libpeer/third_party/mbedtls/include"
CFGDEF='-DMBEDTLS_CONFIG_FILE="xc360_mbedtls_webrtc_config.h"'
SC="$(grep -o 'srtp_crypto=[a-z]*' "$W/BUILDINFO.txt" | cut -d= -f2)" #'s/.*srtp_crypto=\([a-z]*\).*//p' "$W/BUILDINFO.txt")"
DEFS="-DCONFIG_USE_LWIP=0 -DCONFIG_USE_USRSCTP=1 -DLOG_REDIRECT=1 -DCONFIG_DTLS_USE_ECDSA=${ECDSA:-1} -DXCW_SRTP_CRYPTO=\"${SC:-?}\""
INCS="-I$W/include -I$W/src/libpeer -I$W/src/usrsctp -I$SRC -I$HERE/mbedtls -I$MBED"
LIBS="-L$W/lib -lpeer-xc -lusrsctp-xc -lsrtp2-xc -lmbedtls-xcwebrtc"
B="$W/bench"
mkdir -p "$B"

if [[ $T == host ]]; then
  gcc -O2 -g -Wno-deprecated-declarations $DEFS "$CFGDEF" $INCS -o "$B/webrtc_bench" "$SRC/bench/webrtc_bench.c" \
      "$SRC/xc_jitter.c" "$REPO/app/h264play/hosttest/webrtc/xc_udp_posix.c" $LIBS -lpthread -lm
  "$B/webrtc_bench" | tee "$B/host-results.txt"
  exit 0
fi

CC="$DKX/bin/xenon-gcc"
# same MACHDEP as the app / libxenon, without AltiVec
ARCH="-O2 -g -DXENON -D__XENON__ -m32 -mno-altivec -fno-pic -mpowerpc64 -mhard-float -ffunction-sections -fdata-sections -D_POSIX_THREADS=1"
SHIM="-I$HERE/shim/include -include xcw_shim.h -D_POSIX_READER_WRITER_LOCKS=1"
XINC="-I$DKX/usr/include"
"$CC" $ARCH $SHIM $DEFS "$CFGDEF" $INCS $XINC -c "$SRC/bench/webrtc_bench.c" -o "$B/webrtc_bench.o"
"$CC" $ARCH -I"$SRC" -c "$SRC/xc_jitter.c" -o "$B/xc_jitter.o"
"$CC" $ARCH $XINC -I"$SRC" -I"$W/include" -I"$HERE/mbedtls" -I"$MBED" "$CFGDEF" -c "$SRC/xc_udp_xenon.c" -o "$B/xc_udp_xenon.o"
"$CC" $ARCH $XINC -DXCLOUD360_GPU=1 -c "$REPO/deps/xenon-pthread/xenon_pthread.c" -o "$B/xenon_pthread.o"
"$CC" -m32 -mno-altivec -fno-pic -mpowerpc64 -mhard-float -L"$DKX/xenon/lib/32" -Wl,--gc-sections \
    "$B/webrtc_bench.o" "$B/xc_jitter.o" "$B/xc_udp_xenon.o" "$B/xenon_pthread.o" \
    -Wl,--start-group $LIBS -L"$DKX/usr/lib" -lxenon -lm -lc -Wl,--end-group \
    -n -T "$DKX/app.lds" -Wl,-Map,"$B/webrtc-bench.map" -o "$B/webrtc-bench.elf"
"$DKX/bin/xenon-objcopy" -O elf32-powerpc --adjust-vma 0x80000000 "$B/webrtc-bench.elf" "$B/webrtc-bench.elf32"
"$DKX/bin/xenon-strip" "$B/webrtc-bench.elf32"
python3 "$HOME/ffmpeg-xenon-mt-altivec/xcloud360/vmxaudit.py" "$B/webrtc-bench.elf" | tail -2
V="$REPO/app/h264play/variants"
mkdir -p "$V"
cp "$B/webrtc-bench.elf32" "$V/webrtc-bench.elf32"
(cd "$V" && sha256sum webrtc-bench.elf32 > webrtc-bench.elf32.sha256 && cat webrtc-bench.elf32.sha256)
ls -la "$V/webrtc-bench.elf32"

# ---- link check of the full app-side API (source/webrtc/*.c), compiled with
# the APP's own MACHDEP (devkitxenon rules: -maltivec) as the Makefile will.
echo "=== link check: source/webrtc + libs with the app MACHDEP"
APPARCH="-g -O2 -Wall -DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float"
L="$B/linktest"
mkdir -p "$L"
for f in xc_webrtc xc_webrtc_mt xc_jitter xc_proto; do
  "$CC" $APPARCH -I"$SRC" -I"$W/include" -c "$SRC/$f.c" -o "$L/$f.o"
done
"$CC" $APPARCH $XINC -I"$SRC" -I"$W/include" -I"$HERE/mbedtls" -I"$MBED" "$CFGDEF" -c "$SRC/xc_udp_xenon.c" -o "$L/xc_udp_xenon.o"
"$CC" $APPARCH -I"$SRC" -I"$W/include" -c "$SRC/bench/xcw_linktest.c" -o "$L/xcw_linktest.o"
"$CC" $APPARCH $XINC -DXCLOUD360_GPU=1 -D_POSIX_THREADS=1 -c "$REPO/deps/xenon-pthread/xenon_pthread.c" -o "$L/xenon_pthread.o"
"$CC" -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -L"$DKX/xenon/lib/32" -Wl,--gc-sections "$L"/*.o \
    -Wl,--start-group $LIBS -L"$DKX/usr/lib" -lxenon -lm -lc -Wl,--end-group \
    -n -T "$DKX/app.lds" -o "$L/xcw_linktest.elf"
echo "link OK: $L/xcw_linktest.elf ($(stat -c %s "$L/xcw_linktest.elf") bytes)"
python3 "$HOME/ffmpeg-xenon-mt-altivec/xcloud360/vmxaudit.py" "$L"/xc_*.o "$L/xcw_linktest.elf" | tail -1
