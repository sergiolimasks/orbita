#!/bin/sh
# orbita — mockups da proposta de Configurações em menu lateral
# (MENU-SETTINGS-PLAN.md). Só host, não entra no run.sh nem no ELF.
# Uso: app/h264play/hosttest/ui/run_settings_v2.sh [out_dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
UI="$APP/source/ui"
OUTDIR=${1:-$HERE/out}
TMP=${TMPDIR:-/tmp}/xc360-settings-v2.$$
mkdir -p "$TMP" "$OUTDIR"
gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -o "$TMP/settings_v2" "$HERE/settings_v2.c" "$HERE/xui_png.c" \
    "$UI/xui_gfx.c" "$UI/xui_text.c" "$APP/source/catalog/xcc_image.c" -lz -lm
"$TMP/settings_v2" "$HERE/../catalog/fixtures" "$OUTDIR"
rm -rf "$TMP"
