#!/bin/sh
# Host-side (x86, gcc) test of gpu_present.c scheduling. No Xbox, no libxenon.
# Usage: app/h264play/hosttest/run.sh   (from anywhere)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/xc360-gpu-hosttest.$$
mkdir -p "$OUT"
CFLAGS="-O1 -g -Wall -Wextra -Wno-unused-parameter -I$HERE/mock -DXCLOUD360_GPU=1 -DXCLOUD360_VSYNC=0"
gcc $CFLAGS -include "$HERE/mock/hosttest_prelude.h" -c "$HERE/../source/gpu_present.c" -o "$OUT/gpu_present.o"
gcc $CFLAGS -c "$HERE/sim.c" -o "$OUT/sim.o"
gcc $CFLAGS -c "$HERE/fail.c" -o "$OUT/fail.o"
gcc -o "$OUT/sim" "$OUT/sim.o" "$OUT/fail.o" "$OUT/gpu_present.o" -Wl,--wrap=Xe_Fatal
rc=0
for s in 0 1 2 3 4 5 100 101 102 103 104; do "$OUT/sim" $s || rc=1; done
# v0.16: menu (modelo/navegação). v0.20.0: o teste da conexão de controle
# com o launcher do PC (ctrl_test, :9002) saiu junto com o modo PC.
gcc $CFLAGS -o "$OUT/menu_test" "$HERE/menu_test.c" "$HERE/../source/menu_model.c"
"$OUT/menu_test" || rc=1
# v0.20: menu novo (source/ui): testes de unidade + mockup (PNGs em hosttest/ui/out).
"$HERE/ui/run.sh" || rc=1
# HDMI 1080p: EDID reais, escolha do modo, scaler, dump2mode.py (VIDEO-1080P.md).
"$HERE/video/run.sh" || rc=1
# v0.21.25: DIAG.TXT do decoder (DECODE-PERF.md)
"$HERE/dec/run.sh" || rc=1
# v0.21.0: Wi-Fi interno (protocolo, WPA2, driver contra firmware/AP falsos), WIFI.md.
"$HERE/wifi/run.sh" || rc=1
# v0.21.14: locks do app em duas threads (ThreadSanitizer, ordem/ABBA, relatório da espera).
"$HERE/locks/run_locks.sh" || rc=1
# v0.21.28: FatFs + fat_diskio.c reais numa imagem FAT32 nova (o 1º f_mkdir XC360 dos testadores).
"$HERE/fatfs/run_fatfs.sh" || rc=1
rm -rf "$OUT"
[ $rc -eq 0 ] && echo "ALL HOST TESTS PASSED" || echo "HOST TESTS FAILED"
exit $rc
