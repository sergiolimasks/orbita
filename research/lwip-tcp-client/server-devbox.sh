#!/usr/bin/env bash
# Devbox — serve H.264 Annex-B em loop sobre TCP pro Xbox 360.
# Testado em Ubuntu 24.04.
#
# 3 opcoes: netcat (simples), socat (robusto), ffmpeg (transcode on-the-fly).
# Use socat pra dev; ffmpeg quando precisar taxa/GOP controlada.

set -euo pipefail

STREAM_FILE="${1:-stream.h264}"
PORT="${2:-9000}"

if [ ! -f "$STREAM_FILE" ]; then
  cat <<EOF
uso: $0 <stream.h264> [porta=9000]

gerar um stream.h264 de teste (baixa latencia, 5 Mbps, 30fps, IDR a cada 30f):
  ffmpeg -re -i qualquerinput.mp4 \\
    -c:v libx264 -preset ultrafast -tune zerolatency \\
    -x264-params "keyint=30:min-keyint=30:scenecut=0:bframes=0:repeat-headers=1" \\
    -b:v 5M -maxrate 5M -bufsize 1M \\
    -pix_fmt yuv420p -f h264 stream.h264
EOF
  exit 1
fi

# ---- OPCAO A: socat com respawn + loop do arquivo ----
# TCP_NODELAY reduz jitter (nao segura ate MSS-full).
# fork = aceita reconexoes do Xbox sem cair.
echo "[server] listening on 0.0.0.0:${PORT}, streaming $STREAM_FILE in loop"
socat -u \
  TCP-LISTEN:"$PORT",fork,reuseaddr,so-nodelay=1 \
  SYSTEM:"while true; do cat '$STREAM_FILE'; done"

# ---- OPCAO B: ffmpeg direto pro TCP (bit-perfect real-time pacing) ----
# ffmpeg -re -stream_loop -1 -i "$STREAM_FILE" \
#   -c:v copy -f h264 -flush_packets 1 \
#   "tcp://0.0.0.0:${PORT}?listen=1&tcp_nodelay=1"

# ---- OPCAO C: python one-liner sem deps ----
# python3 -c "
# import socket
# s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
# s.bind(('0.0.0.0',$PORT)); s.listen(1)
# print('waiting client...')
# while True:
#     c,a=s.accept(); c.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
#     print('client',a)
#     try:
#         while True:
#             with open('$STREAM_FILE','rb') as f:
#                 while chunk:=f.read(65536): c.sendall(chunk)
#     except (BrokenPipeError,ConnectionResetError): print('client gone')
# "
