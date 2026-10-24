# XCloud360 — Roadmap execução P8+

Consolidação de **24 agents research** (4 waves) + inventário local. Próxima sessão executa direto.

## Estado atual (2026-09-08)

- **h264play.elf32 v0.3.1-p7.1c** rodando @ 24.20 fps em Xbox 360 RGH via Aurora
- Pipeline: embedded H.264 stream (2.8MB) → libavcodec pure-C decode (27.7ms) → BT.601 CPU convert direct-FB tiled (13.7ms) → console_pset present
- Target: 60fps sustained end-to-end
- Bloqueios conhecidos: SMT hanga sob Aurora, AltiVec builtins GCC 9.2 quebrado

## Resources locais disponíveis

```
~/green-nx/                — clone completo, source production Switch xCloud
~/xmplayer/                — clone completo, pthread wrapper + ao_xenon
~/libpeer/                 — patchado (XCLOUD360 + ECDSA gate) + build-pc/
~/ffmpeg-xenon/            — cross-compiled, libavcodec.a + libavutil.a
~/devkitxenon/             — xenon-gcc 9.2.0 toolchain
~/libxenon/                — source libxenon (drivers, headers)

/mnt/storage/projetos/XCloud360/
├── app/h264play/          — atual, funcional 24fps
├── app/h264dec/           — benchmark decoder-only
├── tools/xcloud-capture/  — capture-rtp PC + GOLDEN synth stream
├── tools/h264-feeder/     — Python TCP feeder length-prefixed
├── research/              — 10 docs research consolidados
│   ├── altivec/           — patch surgical ffmpeg + xen_altivec_asm.h
│   ├── av-sync-webrtc/    — DESIGN + sample.c + servo docs
│   ├── h264-lowlatency/   — sample_decoder.c 616 linhas
│   ├── lwip-tcp-client/   — tcp_stream_client.c + PATCH-length-prefix.md
│   ├── yuv-shader-poc/    — HLSL + build-shaders.sh Wine
│   ├── deep-dive-dead-media-*.md
│   ├── libpeer-sctp-datachannels-xcloud-*.md
│   ├── opus-xenon-portability-*.md
│   ├── rtcp-feedback-loop-*.md
│   ├── xcloud-input-wire-format-*.md
│   └── xell-reload-return-to-xell-*.md
└── deps/
    ├── build-libpeer-xenon.sh    — script cross-compile deps
    ├── shim/include/xenon_missing.h
    └── patches/{mbedtls,usrsctp}-xenon.patch
```

## Roadmap linear — DEV-LOOP path (fase 1, ~2 dias)

Prova pipeline funcional sem depender de xCloud real. Devbox alimenta stream via TCP.

### P8.1 — Aplicar patch AltiVec ffmpeg (ganho 3-4× convert)

```bash
cd ~/ffmpeg-xenon
patch -p1 < /mnt/storage/projetos/XCloud360/research/altivec/patched-ffmpeg.diff
# Reconfigure sem --disable-altivec, add -maltivec -mabi=altivec
./configure ...  # comando exato em research/altivec/README-PATCH.md
make -j4 libavcodec/libavcodec.a
```

**Delivera:** libavcodec.a com AltiVec ativo. Convert atual 13.7ms → ~3-4ms teórico.

### P8.2 — Migrar tcp_stream_client.c pra length-prefix

Sample code em `research/lwip-tcp-client/PATCH-length-prefix.md`. +28 linhas / -22 removidas. Elimina 1 frame lookahead (~16ms latency).

### P8.3 — Integrar TCP recv no h264play

Modificar `app/h264play/source/main.c`:
- Adicionar `#include <network/network.h>` + `network_init()` + `network_poll()` no loop
- Substituir embedded_stream loop por drain-de-ring (do tcp_stream_client)
- Manter direct-FB convert e xenon_sound_init pré-existentes

**Gotcha:** `network_init()` sob Aurora — checar `netif.ip_addr.addr != 0` antes de re-init (agent 4).

### P8.4 — Deploy Xbox + rodar feeder

```bash
# Devbox: rodar feeder
python3 /mnt/storage/projetos/XCloud360/tools/h264-feeder/h264_feeder.py \
  --stream tools/xcloud-capture/output/GOLDEN-synth-720p60-baseline/stream.h264 \
  --port 5555 --loop --fps 60

# Deploy Xbox (ELF renomeado default.elf sob DashLaunch)
./scripts/deploy-all-tests
```

**Verify:** h264play conecta, decode+render em ~35-45 fps (AltiVec+tiled). Loop infinito estável.

## Roadmap linear — XCLOUD REAL path (fase 2, ~1-2 semanas)

Depois de dev-loop provado.

### P9.1 — Cross-compile libpeer + deps pra Xenon

```bash
bash /mnt/storage/projetos/XCloud360/deps/build-libpeer-xenon.sh
```

Script pronto do agent 22. mbedtls → usrsctp → libsrtp → libpeer. Estimativa 2-3MB ELF final.

**Riscos médios:**
- usrsctp threading: precisa validar `xenon_run_thread()` wrapper (usar `~/xmplayer/mplayer/libxenon_miss/xenon_pthread.c` como base)
- malloc thread-safety newlib: mutex wrap `_malloc_r/_free_r`

### P9.2 — Refactor libpeer socket.c BSD→lwip RAW

libxenon fixou `LWIP_SOCKET=0`. Precisa adapter:
- `socket(AF_INET, SOCK_DGRAM, 0)` → `udp_new()`
- `bind()` → `udp_bind()`
- `sendto()` → `udp_sendto()`
- `recvfrom()` → callback `udp_recv()` + ring buffer

Reusar pattern do meu `tcp_stream_client.c` (agora length-prefixed). ~4-6h manual.

### P9.3 — Cross-compile libopus

```bash
git clone https://github.com/xiph/opus ~/libopus
cd ~/libopus
./autogen.sh
./configure --host=powerpc-none-eabi --disable-shared --enable-static \
  --enable-fixed-point --disable-asm --disable-rtcd --disable-intrinsics \
  --disable-doc --disable-tests --disable-extra-programs \
  CC=xenon-gcc CFLAGS="-m32 -mpowerpc64 -mcpu=cell -mhard-float -DXENON"
make -j4
```

**Delivera:** libopus.a ~420-550KB.

### P9.4 — Portar green-nx video_jitter + RTCP helpers

Copiar de `~/green-nx/src/switch/stream/`:
- `video_jitter.hpp/cpp` (300 linhas, adapt C++17 → C++11 se necessário, depende só `<vector>/<deque>`)
- `audio_jitter.hpp/cpp` + `audio_player.hpp/cpp` (adapt libnx → libxenon audio API)
- RTCP helpers (`send_nack`, `send_remb`, `send_receiver_report`) — extrair de `engine.cpp`

**Nota:** agent 23 provou que PR#262 do libpeer NÃO tem esses helpers (agent 16 alucinou). Precisa portar do green-nx.

### P9.5 — Wire libpeer no h264play

- `PeerConfiguration.onvideotrack` → alimenta jitter buffer → decoder
- 4 data channels (input/control/message/chat) na ordem correta (agent 13)
- ECDSA cert + SDP munging já aplicado no libpeer patched
- Retry-once pattern do capture-rtp já aplicado

### P9.6 — Input xCloud gamepad

Sample completo em `research/xcloud-input-wire-format-2026-09-07.md`:
- 38B binary packet (agent 11)
- 125Hz tick
- **BE bswap crítico** — Xenon é BE, packet é LE, put<T> precisa swap
- x360-controle já integrado no Xbox — usa `get_controller_data()` da libxenon

## Roadmap linear — SMT + XeLL boot path (fase 3, opcional)

Pra destravar multi-thread (P7.2). Precisa Sergio setup manual USB.

### P10.1 — Download XellLaunch2.xex

Aguardando agent 25 (URL exato). Colocar em `/USB/XellLaunch/default.xex`.

### P10.2 — Config XeLL Reloaded autoboot

USB FAT32 root: `xenon.elf` (renomeado h264play.elf32).

XeLL scanneia devices order: uda0 → udb0 → udc0 → sda0 → dvd0. Filelist: `xell.conf` → `kboot.conf` → `xenon.elf`.

### P10.3 — Sequência boot funcional

```
Aurora → XellLaunch/default.xex → syscall backdoor → XeLL Reloaded → 
  autoboot USB xenon.elf → h264play com SMT válido → 
  return via exit(0) cascade
```

Latency: 10-20s reboot cycle. HDMI pisca 1-2×.

### P10.4 — Re-habilitar P7.2 pipeline 2-core

```c
xenon_thread_startup();  // agora funciona
xenon_run_thread_task(2, stack, worker_convert);
```

Ganho esperado: 30fps → 55-60fps (decode paralelo com convert).

## Parâmetros numéricos consolidados

| Param | Valor | Fonte |
|---|---|---|
| DTLS_WAIT_SECS | 12 | agent 9 |
| MAX_SESSION_ATTEMPTS cloud | 2 | agent 9 |
| retry cooldown | 3s | agent 9 |
| PLI cooldown | 1000ms hard | agent 16 |
| NACK | imediato/gap | agent 16 |
| RR+REMB period | 1000ms | agent 16 |
| REMB steady | 8-10 Mbps | agent 16 |
| V_HOLD_MS | 150 | agent 17 |
| A_SERVO_TARGET_MS | 100 | agent 17 |
| A_PREBUFFER_MS | 80 | agent 17 |
| SYNC_DRIFT_ALARM_MS | 50 | agent 17 |
| SYNC_DRIFT_RESET_MS | 250 | agent 17 |
| TCP_MSS libxenon | 1476 | agent 4 |
| TCP_WND | 5.9KB default, bump 23KB se >5Mbps | agent 4 |
| SO_RCVBUF Xbox | 64KB (drop-not-buffer) | agent 6 |
| RING_CAPACITY | 1MB (bump de 512KB) | agent 20 |
| Input rate | 125Hz | agent 11 |
| Audio buffer target | 100ms + 1000ms cap | agent 17 |
| Peak RAM total | ~25MB / 480MB budget | agent 24 |

## Contradições resolvidas

1. **PR#262 RTCP helpers:** agent 16 alucinou, agent 23 confirmou. NÃO tem. Portar do green-nx.
2. **Wire framing:** length-prefix wins. Migrar tcp_stream_client.
3. **Audio servo Xenon:** hardware fixed 48kHz. MVP skip servo (buffer 200ms), doc limite 45min. Full path = software linear resample + PI.

## Deferrals (não urgentes)

- P4 v2 rendertest2 + P4.5 yuvsynth — irrelevantes com shader Xenos path
- Xenos GPU shader YUV→RGB (rshadercompiler+Wine) — só se AltiVec+SMT não bater 60fps
- xCloud account isolation — testar quando tiver pipeline real
- ffmpeg pthreads frame-threading — só se P7.2 não bater

## Ordem de execução recomendada próxima sessão

1. **P8.1** patch AltiVec + rebuild (30min)
2. **P8.2** migração length-prefix (1h)
3. **P8.3** integração TCP no h264play (2-3h)
4. **P8.4** deploy + teste (Sergio hw feedback)
5. Se AltiVec+network path OK → **P9.1** cross-compile libpeer (1-2 dias)
6. **P9.2-9.6** libpeer wire + jitter + input (1 semana)
7. **P10.1-10.4** XeLL boot + SMT (paralelo, quando Sergio setup USB)

## Comandos deploy consolidados

```bash
# Serve ELF via HTTP netboot
./scripts/serve-xell app/h264play/h264play.elf32

# OU deploy FTP direto (Aurora)
./scripts/deploy-all-tests

# Devbox feeder pro dev-loop
python3 tools/h264-feeder/h264_feeder.py --stream <path> --port 5555 --loop --fps 60
```
