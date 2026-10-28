#!/bin/sh
# orbita v0.21.33 — testes do host das Configurações em menu lateral
# (MENU-SETTINGS-PLAN.md): navegação, textos/opções, zona morta (PAD.TXT, a
# conta do jogo, Medir sozinho) e NOTES.TXT. Com ASan+UBSan.
# Os PNGs das telas saem do hosttest/ui/run.sh (cenas 60-69, código real).
# Uso: app/h264play/hosttest/settings/run_settings.sh   (de qualquer lugar)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/xc360-settings-hosttest.$$
mkdir -p "$OUT"
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$OUT/settings_test" "$HERE/settings_test.c" \
    "$APP/source/settings/xc_cfgnav.c" "$APP/source/settings/xc_cfgview.c" "$APP/source/settings/xc_notes.c" \
    "$APP/source/input/xc_padcfg.c" -lm
rc=0
"$OUT/settings_test" || rc=1
rm -rf "$OUT"
exit $rc
