#!/bin/sh
# orbita v0.21.28 — FatFs + fat_diskio.c reais sobre uma imagem FAT32 nova (mkfs.fat).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/xc360-fatfs-hosttest
mkdir -p "$OUT/inc"
# o disc_io.h do libxenon puxa xetypes.h; no host bastam os tipos
cat > "$OUT/inc/xetypes.h" <<'EOF'
#include <stdbool.h>
#include <stdint.h>
typedef int32_t s32;
typedef uint32_t u32;
EOF
INC="${LIBXENON_INC:-$HOME/libxenon-gip/usr/include}"
mkdir -p "$OUT/inc/diskio"
cp "$INC/diskio/disc_io.h" "$OUT/inc/diskio/disc_io.h"
rm -f "$OUT/fresh.img"
MKFS=$(command -v mkfs.fat || echo /usr/sbin/mkfs.fat)
"$MKFS" -C -F 32 "$OUT/fresh.img" 65536 >/dev/null
gcc -O1 -g -Wall -Wno-unused-function -fsanitize=address,undefined -I"$OUT/inc" -I"$APP/source/login" \
    "$HERE/fatfs_test.c" "$APP/source/login/fat_diskio.c" "$APP/source/login/fatfs/ff.c" -o "$OUT/fatfs_test"
"$OUT/fatfs_test" "$OUT/fresh.img" && echo "FATFS HOST TESTS PASSED" || { echo "FATFS HOST TESTS FAILED"; exit 1; }
