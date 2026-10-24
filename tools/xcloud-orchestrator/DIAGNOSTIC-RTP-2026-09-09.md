# capture-rtp diagnostico — 2026-09-09 (updated late)

## Status

RESOLVIDO. Streaming end-to-end funciona: xCloud entrega gameplay real
(AC Odyssey epilepsy warning frame confirmado decodado). capture-rtp
serve length-prefix Annex-B AUs em TCP arbitrario. Orchestrator Fase B
integrado.

## Estado terminal (final, apos fix)

```json
{
  "capture_seconds": 20,
  "final_pc_state": "completed",
  "video_bytes": 619665,
  "video_nals": 1188,
  "audio_bytes": 148932,
  "audio_frames": 999,
  "endpoint_host": "https://brs.core.gssv-play-prod.xboxlive.com",
  "tier": "P720"
}
```

Deliver rate 30-250 kbps no menu inicial, sobe pra ~2-3Mbps quando
gameplay carrega. Xbox recebe AUs Annex-B validos com IDR+SPS+PPS
prependados no primeiro frame por conexao.

## Root cause (revisao vs diagnostico anterior)

O documento anterior atribuia `video_bytes=0` a "trickle ICE ausente".
ERRADO. capture-rtp ja tinha trickle ICE completo (thread `ice_thread`
com `send_ice_candidates` + `receive_ice_candidates` a cada 200ms +
keepalive). O bloqueio real era em 2 camadas separadas:

### Bug 1 — libpeer sem os patches do Green-NX

O patch em `tools/xcloud-capture/patches/libpeer-xcloud-ecdsa-sdp.diff`
era um SUBSET do patch completo do Green-NX
(`~/green-nx/deps/patches/libpeer-switch.patch`, 1203 linhas). Faltava:

- `peer_connection_datachannel_send_text_sid` — sem isso e' impossivel
  mandar mensagens JSON no PPID_STRING que xCloud exige pra handshake +
  authorization + user_requested_resolution.
- Bit `msg[1] = channel_type` no DCEP OPEN — sem isso todo canal virava
  reliable/ordered independente do que voce pediu.
- `sctp_add_stream_mapping` no create_datachannel — sem isso
  `peer_connection_lookup_sid()` sempre retornava -1 pra canais que NOS
  abrimos (so' funcionava pra canais que o peer abre e nos ecoamos).
- Fallback demux RTP por payload type — libpeer normalmente roteia media
  por SSRC do `a=ssrc:` do answer, mas xCloud omite `a=ssrc:audio` (BUG
  do lado deles). Todo pacote de audio caia em nenhum branch e era
  descartado. Video tambem em algumas sessoes.
- `peer_connection_loop` retorna `>0` se drenou pacote, permitindo drain
  em burst do socket. Sem isso, keyframe H264 (~40 pacotes) tomava 40
  loops de 500us pra ser drenado, e o buffer estourava.
- Depayloader H.264 substituido por pass-through raw RTP (comentario
  green-nx: "libpeer's own in-arrival-order NALU assembly corrupted
  frames on any reorder/loss and produced green garbage, so it is
  bypassed").

### Bug 2 — xCloud handshake protocol nao implementado

capture-rtp fazia todo o setup ate DTLS+SCTP completar, e entao... nada.
xCloud NAO comeca a mandar RTP video enquanto o cliente nao completar
um handshake application-layer no data channel `message`:

1. Client abre canais DCEP: control (sid 0), input (sid 2), message (sid 4), chat (sid 6)
2. Client manda `{"type":"Handshake","version":"messageV1",...}` no message channel
3. xCloud responde com `{"type":"HandshakeAck",...}`
4. Client entao manda no control channel:
   - `{"message":"authorizationRequest","accessKey":"..."}`
   - `{"message":"gamepadChanged","gamepadIndex":0,"wasAdded":true}`
   - `{"message":"userRequestedResolutionUpdate","resolutionAlias":"720"}`
5. Client manda no message channel `ClientDeviceCapabilities` com
   maxBitrateKbps, resolucao, fps, hdr, hevc, systemUi, orientation, etc.
6. Client manda `client_metadata` binario no input channel.
7. Client pede IDR: `{"message":"videoKeyframeRequested"}` + RTCP PLI.
8. **SO ENTAO** xCloud comeca a bombear RTP video/audio.

Sem esse handshake, DTLS/SCTP sobem, ICE cae em COMPLETED, e o servidor
espera indefinidamente pelo Handshake — nunca manda RTP.

## Fix aplicado (sequencia)

1. **Patched libpeer com green-nx full patch** — `git stash` do
   ecdsa-sdp subset, aplicou o patch completo, revertiu CMakeLists
   (Switch-only), rebuild com `cmake --build build-pc -j$(nproc)`.
   Simbolos novos confirmados: `send_text_sid`, `request_keyframe`,
   `add_stream_mapping`.

2. **Adicionou xCloud protocol layer em capture-rtp.cpp**:
   - `on_channel_open()` callback: defer channel creation ate SCTP up,
     entao envia `message_handshake()`.
   - `on_channel_message()` callback: se recebe HandshakeAck no message
     channel, dispara sequencia completa (auth + gamepad + resolution +
     startup_messages + client_metadata + keyframe request).
   - Depayloader H.264 proprio em ~90 linhas (RFC 6184 single-NAL +
     STAP-A + FU-A → Annex-B AUs em marker bit).

3. **TCP-out server** — `--tcp-out PORT`, single client, length-prefix
   Annex-B wire, IDR gating, SPS/PPS caching, per-client keyframe
   request via RTCP PLI + xcloud `videoKeyframeRequested`.

4. **Orchestrator phase_b integracao** — para overlay 9000, spawna
   capture-rtp com `--tcp-out 9000 --seconds 3600 --no-file`, aguarda
   bind, deixa rodando; se morrer, reporta.

## Comandos

Reproduzir capture com TCP-out:
```
cd /mnt/storage/projetos/XCloud360/tools/xcloud-capture
./rtp/capture-rtp ASSASSINSCREEDODYSSEY --tcp-out 9002 --seconds 60 --no-file
# em outro terminal:
nc 127.0.0.1 9002 > /tmp/stream.bin
```

Orchestrator completo (Fase A + B):
```
cd /mnt/storage/projetos/XCloud360/tools/xcloud-orchestrator
python3 -u orchestrator.py --port 9000 --phase-b
```

Xbox pluga o pendrive existente (nao mexeu), boota → xenon.elf faz TCP
connect em devbox:9000 → recebe device-code overlay ate Sergio autorizar
no navegador → Fase B mata overlay, spawna capture-rtp, Xbox reconecta e
recebe stream real de AC Odyssey.

## Artefatos

Novos:
- `~/libpeer/` — patched com green-nx patch (git stash preserva o subset
  anterior: `git stash list`)
- `~/libpeer/build-pc/src/libpeer.a` — biblioteca com novos simbolos
- `tools/xcloud-capture/rtp/capture-rtp` — binario com handshake completo
- `tools/xcloud-capture/rtp/capture-rtp.cpp` — codigo (~800 linhas, era 636)

Modificados:
- `tools/xcloud-orchestrator/orchestrator.py` — phase_b real (era stub)

## Testes de aceite

- capture-rtp standalone: `video_bytes > 0`, session-meta.json completo.
- ffmpeg decoda stream.h264 → frames validos AC Odyssey epilepsy warning.
- TCP-out via nc: 62KB em 8s (menu inicial), primeiro AU = SPS+PPS+IDR,
  sub = P-slices. Decodavel com ffmpeg (1280x720 Main profile).
- Orchestrator phase_b smoke: hands off port 9000, capture-rtp binda em
  ~15-20s, cliente TCP recebe stream length-prefix decodavel.

## Pendencias pro proximo iteration

- **Xbox real end-to-end**: pendrive esta plugado no devbox agora
  (`/dev/sdd1` = 57.7GB, tem `xenon.elf` + `kboot.conf` intactos).
  Precisa Sergio plugar no Xbox, bootar, autorizar via device code novo,
  aguardar transicao pra Fase B, confirmar visual do gameplay na TV.
- **lwip TCP_WND**: bottleneck de ~1.16Mbps do Xbox pode limitar stream
  a resolucao/bitrate menor. Se aparecerem stutters visiveis, aumentar
  TCP_WND em `libxenon lwipopts.h` de 5904 pra 32768/65535 e rebuildar
  xenon.elf. Nao aplicado nesta sessao — deixa pra depois se necessario.
- **Reconnect handling**: se capture-rtp morrer mid-stream (tokens MSA
  expiram, xCloud slot conflict, etc), orchestrator hoje so' reporta.
  Adicionar loop de retry com backoff + reautenticacao automatica.
- **Audio**: RTP audio (Opus PT 111) chega mas Xbox player nao consome
  audio hoje. Se quiser audio no Xbox, integrar decoder Opus no
  h264play.elf.
