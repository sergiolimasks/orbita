# xCloud dead-media patches — 2026-09-07

Dois patches unificados que resolvem o dead-media path (ICE up + DTLS silent):

- `libpeer-xcloud-ecdsa-sdp.diff` (305 lines) — 5 arquivos em `~/libpeer/src/`
- `capture-rtp-retry-pattern.diff` (104 lines) — 1 arquivo em `tools/xcloud-capture/rtp/`

Ambos já foram aplicados no workspace deste commit. Este doc é pra reaplicar
em fresh clone.

## Aplicação

```bash
cd ~/libpeer
git apply /mnt/storage/projetos/XCloud360/tools/xcloud-capture/patches/libpeer-xcloud-ecdsa-sdp.diff

cd /mnt/storage/projetos/XCloud360
git apply tools/xcloud-capture/patches/capture-rtp-retry-pattern.diff
```

## Rebuild

```bash
cd ~/libpeer && cmake --build build-pc -j$(nproc) && cmake --install build-pc --prefix build-pc/dist
cd /mnt/storage/projetos/XCloud360/tools/xcloud-capture/rtp && make
```

## O que os patches fazem

### libpeer (5 arquivos)
1. `src/config.h` — `#define CONFIG_DTLS_USE_ECDSA 1` (o gate já existia em `dtls_srtp.c:90`, só precisava ativar). Bump `CONFIG_MAX_NALU_SIZE` de 10KB → 2MB.
2. `src/peer_connection.c` (linha 454) — role em `SDP_TYPE_OFFER` vira `DTLS_SRTP_ROLE_CLIENT` (era SERVER, causava double-passive stalemate).
3. `src/sdp.c` — `sdp_append_h264` reescrito com template exato green-nx (PT 102, packetization-mode=1, level-asymmetry-allowed=0, max-fs=3600, max-mbps=108000, rtcp-rsize, recvonly, goog-remb, ccm fir). `sdp_append_opus` reescrito com SAVPF, stereo=1, recvonly.
4. `src/socket.c` — `SO_RCVBUF=4MB` + `SO_SNDBUF=1MB` (default kernel Linux 208KB dropa keyframe H264 burst).
5. `src/dtls_srtp.c` — `dtls_srtp_read` remove loop de retry (WANT_READ é normal, loop congelava worker).

### capture-rtp (1 arquivo)
Retry pattern green-nx: 2 attempts max (era 5), DTLS_WAIT_SECS=12 (era 20), dead-media detection separada de erros FAILED/DISCONNECTED, cooldown 3s antes de retry pra xCloud liberar slot da conta.

## Verificação

```bash
# 1. ECDSA ativo no config
grep 'CONFIG_DTLS_USE_ECDSA' ~/libpeer/src/config.h
# saída: #define CONFIG_DTLS_USE_ECDSA 1

# 2. SDP template compilado no binário
strings tools/xcloud-capture/rtp/capture-rtp | grep 'SAVPF 102'
# saída: m=video 9 UDP/TLS/RTP/SAVPF 102

# 3. Rodar capture, dump offer.sdp gerado
tools/xcloud-capture/rtp/capture-rtp <TITLE_ID>
grep -A1 'm=video' output/*/offer.sdp
# esperado: m=video 9 UDP/TLS/RTP/SAVPF 102, com fmtp packetization-mode=1

# 4. Confirmar cert ECDSA gerado (em runtime durante handshake)
# Rodar com LOG_LEVEL=DEBUG e checar log 'local fingerprint: <sha256>'.
# Se der pra dumpar o cert (mbedtls_x509_write_crt_pem em breakpoint gdb),
# o SubjectPublicKeyInfo deve mostrar 'id-ecPublicKey / prime256v1' em vez
# de 'rsaEncryption'.
```

## Se ainda der dead-media após patches

Suspeita cai em signalling layer (não libpeer):
- Token GSSV expirado — regen via `login.sh`
- User-Agent HTTP wrong no POST /sdp — validar com tcpdump que passa
  identificação de Android app válida
- Region blocklist — xCloud sa-east-1 pode ter session cap por conta;
  testar com endpoint us-east
- TURN relay obrigatório — checar candidate types em ice-remote.json.
  Se todos `typ relay`, PR#262 hunks de TURN são úteis (já cherry-pickados
  de forma minimalista aqui: sabemos que xCloud não usa TURN)
