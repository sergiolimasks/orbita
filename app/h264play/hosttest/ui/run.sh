#!/bin/sh
# XCloud360 — menu novo (XUI) no host: gera a fonte/ícones se preciso, roda
# os testes de unidade e o mockup (PNGs + bench em hosttest/ui/out/).
# Dados: fixtures da camada de catálogo (hosttest/catalog/fixtures); sem
# elas o mockup roda só com placeholders.
# Uso: app/h264play/hosttest/ui/run.sh   (de qualquer lugar)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
UI="$APP/source/ui"
GEN="$APP/tools/gen_xui_assets.py"
ASSETS="$UI/xui_assets.h"
if [ ! -f "$ASSETS" ] || [ "$GEN" -nt "$ASSETS" ]; then
    python3 -I "$GEN" "$ASSETS"
fi
OUT=${TMPDIR:-/tmp}/xc360-ui-hosttest.$$
mkdir -p "$OUT" "$HERE/out"
CFLAGS="-O2 -g -Wall -Wextra -Wno-unused-parameter"
SRC="$UI/xui_gfx.c $UI/xui_text.c $UI/xui_home.c $UI/xui_tile.c $UI/xui_mm.c $APP/source/menu_model.c"
# v0.21.33: Configurações (navegação, textos e zona morta: o mesmo código do app)
SRC="$SRC $APP/source/settings/xc_cfgnav.c $APP/source/settings/xc_cfgview.c $APP/source/input/xc_padcfg.c $APP/source/ui/xc_i18n.c"
rc=0
gcc $CFLAGS -o "$OUT/xui_test" "$HERE/xui_test.c" $SRC -lm
"$OUT/xui_test" || rc=1
# Actual Remote Play screen; PPM keeps the fixture renderer dependency-free.
gcc $CFLAGS -o "$OUT/remote_mockup" "$HERE/remote_mockup.c" \
    "$UI/xui_gfx.c" "$UI/xui_text.c" "$UI/xui_home.c" "$UI/xc_i18n.c" -lm
for scene in list empty busy error scroll home; do
    "$OUT/remote_mockup" "$HERE/out/remote_$scene.ppm" "$scene" || rc=1
done
# mockup: + adaptador do catálogo e o decodificador JPEG dele (xcc_image.c);
# o resto da API do catálogo é simulado dentro do mockup.c.
gcc $CFLAGS -DXCLOUD360_LOGIN=1 -o "$OUT/mockup" "$HERE/mockup.c" "$HERE/xui_png.c" $SRC "$UI/xui_cat.c" \
    "$APP/source/catalog/xcc_image.c" -lz -lm
"$OUT/mockup" "$HERE/../catalog/fixtures" "$HERE/out" || rc=1
rm -rf "$OUT"
exit $rc
