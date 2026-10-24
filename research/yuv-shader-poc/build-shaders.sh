#!/usr/bin/env bash
# build-shaders.sh - compilar HLSL -> .psu/.vsu no Linux via Wine + XNA.
# Testado em Ubuntu 24.04 (devbox). Rode UMA VEZ para gerar os blobs;
# depois disso o build do jogo so precisa dos .h embutidos.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/bin"
SHD="$HERE/shaders"

# ------------------------------------------------------------------------------
# STEP 1 (uma unica vez): instalar Wine + XNA 3.0 + rshadercompiler.exe
# ------------------------------------------------------------------------------
# rshadercompiler e C#/.NET 3.0 + XNA 1.0 dll (32-bit Windows).
# Nao roda nativo em Linux -- so via Wine + winetricks:
#
#   sudo apt install wine winetricks
#   WINEARCH=win32 WINEPREFIX=~/.wine-xna wine wineboot
#   WINEPREFIX=~/.wine-xna winetricks -q dotnet35sp1 xna31
#
# Depois clone e builde:
#   git clone https://github.com/fgsfdsfgs/rshadercompiler
#   cd rshadercompiler
#   WINEPREFIX=~/.wine-xna wine 'C:\windows\Microsoft.NET\Framework\v3.5\MSBuild.exe' \
#       rshadercompiler.sln /p:Configuration=Release
#   cp rshadercompiler/bin/Release/*.exe rshadercompiler/bin/Release/*.dll \
#      libs/*.dll  $BIN/
#
# Alternativa sem Wine: NENHUMA. XNANative1.dll (CompileEffectForXbox) e closed-source
# Microsoft, so existe pra x86 Windows. XenosRecomp descompila bytecode -> HLSL PC, nao
# faz o caminho inverso.

# ------------------------------------------------------------------------------
# STEP 2: compilar shaders (roda toda vez que HLSL muda)
# ------------------------------------------------------------------------------
mkdir -p "$BIN"

WINEPREFIX="${WINEPREFIX:-$HOME/.wine-xna}" \
  wine "$BIN/rshadercompiler.exe" "$SHD/yuv_ps.hlsl" "$SHD/yuv_ps.psu" ps

WINEPREFIX="${WINEPREFIX:-$HOME/.wine-xna}" \
  wine "$BIN/rshadercompiler.exe" "$SHD/yuv_vs.hlsl" "$SHD/yuv_vs.vsu" vs

# ------------------------------------------------------------------------------
# STEP 3: embutir blobs no ELF via xxd -i (build-time, sem runtime FS I/O)
# ------------------------------------------------------------------------------
cd "$SHD"
xxd -i yuv_ps.psu > "$HERE/src/yuv_ps_blob.h"
xxd -i yuv_vs.vsu > "$HERE/src/yuv_vs_blob.h"

# yuv_ps_blob.h vira: unsigned char yuv_ps_psu[] = { 0x10, 0x2a, ... };
#                    unsigned int  yuv_ps_psu_len = 300;
# yuv_present.c ja declara `extern const uint8_t g_yuv_ps[]` -- ajuste os names
# no header ou faca um wrapper:
#
#   const uint8_t *g_yuv_ps = yuv_ps_psu;
#   const uint32_t g_yuv_ps_len = yuv_ps_psu_len;

echo "OK. Blobs em $SHD/*.psu + headers em $HERE/src/*_blob.h"
