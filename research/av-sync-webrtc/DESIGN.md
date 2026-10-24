# AV Sync Design — XCloud360 WebRTC receiver

Data: 2026-09-07
Fontes: green-nx v1.0.30+, Chromium WebRTC, moonlight-common-c, GStreamer rtpbin, RFC 3550/6184/7587.

## 1. Contexto e restrição

Alvo Xbox 360 RGH recebendo H.264 720p60 (~4.5 Mbps, clock 90 kHz) + Opus 48 kHz stereo (clock 48 kHz) via SRTP dentro de PeerConnection (libpeer). Decodificador de vídeo atual roda ~24 fps (pipeline saturado); áudio Opus decodifica em ~1 ms por pacote de 20 ms. Meta: `drift audio↔video < 40 ms` (imperceptível, ITU-R BT.1359-1 janela lipsync ±22 ms leading / ±90 ms trailing) sem eco, sem freeze.

## 2. Decisão-mãe: **NÃO usar cross-stream sync explícito**

O Chromium (`video/stream_synchronization.cc`) mede `relative_delay_ms` cruzando RTP↔NTP via SR e ajusta `SetMinimumPlayoutDelay` em ambos streams a cada 1 s (`kSyncInterval`, `kMinDeltaMs=30`, `kMaxChangeMs=80`). Isso pressupõe **filas de playback controláveis nos dois lados** — no navegador funciona porque tanto AudioTrack quanto VideoRenderer aceitam `minimum_playout_delay`.

Green-nx (referência que funciona em produção Switch) **abandonou** esse padrão. Motivo: `libnx audout` não expõe latência controlável, e o decoder de vídeo é o gargalo instável. A solução dele é **duas filas independentes com política master-clock implícita = server encoder**:

- Ambos os streams saem do mesmo encoder do servidor xCloud, com timestamps derivados do mesmo relógio de captura.
- Cada fila é dimensionada por sua função de falha: vídeo por `RTT + NACK round-trip` (kHoldMs=200 ms), áudio por `underrun avoidance` (target 100 ms com servo ±0.2 %).
- A convergência acontece porque **o servidor mantém o alinhamento**. O cliente só evita amplificar drift.

Para XCloud360, com um decoder ainda mais fraco que o Switch, este padrão é obrigatório. Um `StreamSynchronization` estilo Chromium exigiria calibrar `RtpToNtpEstimator` a partir de SRs RTCP a cada 1 s e injetar delay em audio quando video atrasa — mas nosso audio já roda apertado (5 ms de folga no audout), então empurrar +200 ms de silêncio quando o decoder de vídeo pisca causa audio hiccup audível, que é pior que perder lipsync momentaneamente.

## 3. Arquitetura

```
                     +----------------+
   SRTP audio  ----->|  AudioJitter   |----+       Opus 20ms packets
                     |  (seq reorder, |    |       reordered by seq,
                     |   kMaxLate=32) |    |       hole-tolerant
                     +----------------+    |
                                           v
                                    +---------------+
                                    | Audio decode  |     libopus decode
                                    | thread        |     -> int16 stereo PCM
                                    +---------------+
                                           |
                                           v
                                    +---------------+     stretch/shrink
                                    | Servo (+/-    |     +/-0.2% adjust
                                    | 0.2%) on ring |     to null crystal
                                    | depth EMA     |     drift server<->console
                                    +---------------+
                                           |
                                           v (target 100 ms EMA)
                                    +---------------+
                                    | Ring 1000 ms  |     shed > 300 ms
                                    | high=300      |     down to 150
                                    +---------------+
                                           |
                                           v (48 kHz stereo int16)
                                     audout (5-10 ms hw bufs, primed)


   SRTP video  ----->+----------------+
                     |  VideoJitter   |          Assembly by RTP timestamp
                     |  kHoldMs=200   |          (all packets of one AU
                     |  kMaxFrames=32 |          share the same 90 kHz ts).
                     |  Wait for      |          NACK on gap. On hold-timeout,
                     |  IDR after     |          drop + request IDR.
                     |  drop          |
                     +----------------+
                              |
                              v (complete AU, in decode order)
                     +----------------+
                     | Decode thread  |          libavcodec H.264 hw-decode.
                     | (worker)       |          Publishes latest frame to
                     +----------------+          renderer via shared_frame_
                              |
                              v (present latest)
                     +----------------+          Render thread drops old
                     | Renderer at    |          decoded frames if a newer
                     | vsync/60Hz     |          one arrives — audio never
                     +----------------+          waits for video.
```

Nunca há acesso cruzado. Audio nunca espera vídeo. Vídeo nunca espera audio.

## 4. Política de master clock

**Master implícito = audio ring depth**, que na prática rastreia o encoder do servidor. Motivos:

1. Audio é o único stream com playout rate fisicamente fixo (audout consome exatamente 48000 sps).
2. Audio underrun é audível (click / pop); video hiccup de 1 frame passa despercebido.
3. Se o encoder do servidor sofre CPU spike e atrasa 30 ms, ambos os streams atrasam juntos e o offset relativo se mantém.

O video decode roda tão rápido quanto consegue. Se decoder = 24 fps mas source = 60 fps, o renderer descarta frames antigos ao apresentar — o áudio segue em wall-clock e o vídeo "vídeo lento" fica mostrando o frame mais recente disponível. Isso causa lipsync degradado (video atrasado em relação ao audio pelo delta decode), mas **não causa drift acumulativo**: quando decoder recupera, video volta ao alinhamento em 1-2 frames.

## 5. Constantes recomendadas para XCloud360

| Parâmetro                    | Valor          | Racional                                                                              |
| ---------------------------- | -------------- | ------------------------------------------------------------------------------------- |
| `V_HOLD_MS`                  | **150 ms**     | Menor que green-nx (200) porque Xbox está mais perto do servidor (LAN Live). Preserva margem NACK sem inflar latência. |
| `V_MAX_FRAMES`               | 32             | Cap de segurança, mesmo do green-nx. Nunca deve ser atingido em rede saudável.        |
| `V_MAX_AU_BYTES`             | 2 MiB          | Bound duro pra memória.                                                               |
| `A_MAX_LATE_PACKETS`         | 32             | ~640 ms de folga Opus @ 20 ms; muito além de qualquer reordering real.                |
| `A_RING_MS`                  | 1000           | Capacidade total. Nunca cheia em prática.                                             |
| `A_PREBUFFER_MS`             | 80             | Primeiro fill antes de começar playback.                                              |
| `A_RESUME_MS`                | 40             | Reprime raso após underrun (recupera rápido).                                         |
| `A_SERVO_TARGET_MS`          | **100**        | Ponto de operação do servo.                                                           |
| `A_SERVO_GAIN`               | 30e-6 (30 ppm/ms) | Igual green-nx. Deriva devagar.                                                    |
| `A_SERVO_EMA_ALPHA`          | 0.02           | Constante de tempo ~1 s.                                                              |
| `A_SERVO_MAX_ADJ`            | 2000e-6 (0.2 %) | Inaudível.                                                                           |
| `A_HIGH_WATERMARK_MS`        | 300            | Se ultrapassar, shed até target.                                                      |
| `A_SHED_TARGET_MS`           | 150            | Alvo pós-shed.                                                                        |
| `SYNC_DRIFT_ALARM_MS`        | 50             | Se `abs(video_latency - audio_latency) > 50` por > 3 s: log warn + request IDR (não reseta buffers). |
| `SYNC_DRIFT_RESET_MS`        | 250            | Só reset hard (audio flush + IDR) acima disso — evita boom-loop.                      |

**Não use `SYNC_DRIFT_RESET_MS < 200`** — o Chromium usa `kMaxChangeMs=80` como slew rate exatamente pra evitar isso, e o kMaxDeltaDelayMs=10s como sanidade.

## 6. RTP timestamp: reconciliação 90 kHz vs 48 kHz

**Não reconcilia.** Cada stream reordena por sua sequência RTP e monta AU por timestamp local. A conversão entre 90 kHz e 48 kHz só é necessária se você for calcular `relative_delay_ms` estilo Chromium — o que nós **não** fazemos.

Se for necessário (para diagnóstico / logs), a fórmula é:

```
capture_time_ntp = SR.ntp_ts + (rtp_ts - SR.rtp_ts) / clock_rate_hz
```

Onde `SR.ntp_ts` e `SR.rtp_ts` vêm do Sender Report RTCP mais recente daquele SSRC (RFC 3550 §6.4.1). O `RtpToNtpEstimator` do Chromium mantém uma regressão linear sobre múltiplos SRs para lidar com o wrap-around do RTP timestamp (32 bits, wrap a cada ~13 h no áudio, ~13 h no vídeo).

Para XCloud360 v1: logue os SRs recebidos (contador `sr_received`), calcule offset apenas para telemetria, **não realimente no controle de buffer**.

## 7. Primeiro frame: sync inicial

- Audio: descartar todos os pacotes até o primeiro packet sem gap. Depois primeira pré-buffer de 80 ms. Só então iniciar playback.
- Video: descartar tudo até o primeiro IDR completo (SPS+PPS+IDR AU inteiro). `waiting_keyframe_ = true` como estado inicial. Enviar PLI/FIR imediatamente após negociação.
- **Não bloquear áudio esperando vídeo.** Se áudio começa primeiro, ok — usuário vê fundo preto por ≤500 ms enquanto IDR chega. É melhor que áudio atrasado.

## 8. Detecção de out-of-sync

Métrica: `drift_ms = wall_clock_at_last_video_render - wall_clock_at_last_audio_sample_played`, mas ambos derivados do **capture_time do servidor** (via SR RTP→NTP), não local. Se não tem SR ainda, use `receive_time - jitter_delay` como proxy.

- `|drift| > 50 ms`: log WARN, request IDR (talvez decoder atrasou).
- `|drift| > 100 ms` por > 3 s: bump `V_HOLD_MS` transitório +50 ms.
- `|drift| > 250 ms`: reset — flush audio ring (mantém prime silêncio 80 ms), reset video jitter, request IDR. Rate limit: 1 por 30 s.

## 9. Correção de drift

Prefira **stretch de audio** (green-nx servo, ±0.2 %) a drop de frame. Motivos:
- Stretch é inaudível. Frame drop é visível como stutter.
- Servo age em cada frame de 20 ms → correção contínua, não bursty.
- Drop de frame é reservado pra recuperação de falha (packet loss não recuperado), não pra sync.

**Não faça "bass boost"** (mexer em EQ pra mascarar). Pra correção positiva ou negativa, use resampler linear de alta qualidade (o green-nx implementa in-place por interpolação de posição fracionária) — ver `pcm_out_` no `audio_player.cpp` v1.0.30.

## 10. Referências no repositório green-nx

Todas as URLs `https://github.com/rmrf404/green-nx/blob/main/`:

- `src/switch/stream/video_jitter.hpp` — API pública (Emit/NackFn, Stats, reset/receive)
- `src/switch/stream/video_jitter.cpp` — `kHoldMs=200`, `kMaxFrames=32`, drain loop com hold-timeout, `waiting_keyframe_` state machine
- `src/switch/stream/audio_jitter.hpp` — sequência-ordenada, `kMaxLate=32` (constante em `audio_jitter.cpp`), 1 Opus por RTP packet
- `src/switch/stream/audio_jitter.cpp` — push/pop by seq, gap tolerance
- `src/switch/stream/audio_player.hpp` — 3-thread arch (submit / decode / audout writer), Stats detalhado (ema_ms, adj_ppm)
- `src/switch/stream/audio_player.cpp` — servo constants (`kServoTargetMs=100`, `kServoGain=30e-6`, `kServoMaxAdj=2000e-6`, `kServoEmaAlpha=0.02`), `kHighWatermarkMs=300`, `kShedTargetMs=150`, `kPrebufferMs=80`, `kResumeMs=40`, ring 1000 ms
- `src/switch/stream/engine.cpp` — `on_video`/`on_audio` callbacks conectando libpeer → jitter → decoder; watchdogs de stall (10 s sem mídia, 15 s sem decode); NÃO chama sync cross-stream

## 11. Referências externas ordenadas por utilidade

1. Chromium `video/stream_synchronization.{h,cc}` — algoritmo canônico, útil pra entender mas **não copiar** para XCloud360. `kMinDeltaMs=30`, `kMaxChangeMs=80`, `kFilterLength=4`. https://webrtc.googlesource.com/src/+/refs/heads/main/video/stream_synchronization.cc
2. Chromium `video/rtp_streams_synchronizer2.cc` — glue que roda a cada 1 s, chama `GetStreamSyncOffsetInMs`. Ilustra como consumir SR NTP.
3. `RtpToNtpEstimator` (Chromium `rtc_base/rtp_to_ntp_estimator.{h,cc}`) — regressão linear RTP↔NTP a partir de SRs.
4. GStreamer `rtpjitterbuffer` docs — modo `slave` (default) escrava wall-clock local em rtpbin usando SR NTP. Modo `synced` para RFC 7273. https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpjitterbuffer.html
5. RFC 7587 §4.1 — Opus RTP: **clock 48000 Hz sempre**, mesmo se stream interno for 8/12/16/24 kHz. Timestamp increment por packet = frame_ms × 48 (20 ms → 960).
6. RFC 6184 §5.1 — H.264 RTP: clock 90000 Hz, timestamp = sampling instant do primeiro octeto codificado do frame. Todos os pacotes que carregam NAL units do mesmo access unit compartilham o mesmo timestamp; marker bit no último.
7. RFC 3550 §6.4.1 — Sender Report: NTP timestamp (64-bit) + RTP timestamp do mesmo instante. Um por SSRC. **É a única correlação inter-stream oficial em RTP.**
8. Moonlight `AudioStream.c` — GameStream faz FEC RS + reorder por seq, sem cross-stream sync. Mesmo padrão.
9. Moonlight `VideoDepacketizer.c` — desmontagem FU-A com bit reassembly bitmap, wait for `waitingForIdrFrame`. Padrão idêntico ao green-nx `VideoJitterBuffer::try_assemble`.

## 12. Benchmarks de cloud gaming (drift medido)

Dados públicos (arxiv, blog engineering):
- **Stadia** (Google, morto): design docs falavam de "sub-16 ms end-to-end input latency budget". Sync interno usava um receive-side clock única para audio+video (patente US20200404037A1).
- **GeForce Now** (Nvidia): sync via GameStream protocol, mesmo padrão do Moonlight (encoder timestamps + client passa por buffer separado). Sem cross-stream sync explícito na Moonlight reimplementation.
- **xCloud** (Microsoft): stack WebRTC com PeerConnection. O sync usa SR RTCP quando disponível mas a maior parte do trabalho é feita no encoder (mesma timeline pra A e V no lado do servidor). Fonte: Green-NX + Xbox Live specs vazadas 2023.
- **Amazon Luna**: WebRTC também. Sem info pública sobre sync interna.

Consenso da indústria: **em cloud gaming ao vivo, quanto menos o cliente mexer em sync, melhor**. O servidor é o clock. O cliente drena com buffers independentes o mais raso possível.

## 13. O que fazer diferente do green-nx

XCloud360 tem 3 restrições que green-nx não tem:

1. **CPU mais fraca** (PowerPC 3.2 GHz vs. Switch ARM 1 GHz — mas green-nx tem HW decoder NVTEGRA, XCloud360 depende de decoder de software CPU). ⇒ Decoder mais lento ⇒ risco maior de vídeo atrasar. ⇒ Reduzir `V_HOLD_MS` pra 150 (menos margem NACK compensada por eventos de reset mais frequentes).
2. **Memória apertada** (512 MB total, ~200 MB disponível pro app). ⇒ Não subir `V_MAX_FRAMES` acima de 32, `A_RING_MS` acima de 1000.
3. **Audio output** provavelmente XAudio2/libxenon som (48 kHz nativo, ok). ⇒ Se latência controlável < 20 ms hw buffer, use audio-driven sync direto (drop video frames velhas). Se latência hw > 30 ms, use padrão green-nx (buffers independentes).

## 14. Roadmap de implementação

1. **P1** (agora): implementar `xc_video_jitter.c` + `xc_audio_jitter.c` + `xc_audio_ring.c` sem sync cross-stream. Constants da tabela §5. Passa 60 fps em rede local sem drift visível.
2. **P2**: adicionar telemetria drift (mede via SR RTP→NTP, só loga).
3. **P3**: se telemetria P2 mostrar drift > 50 ms recorrente, ativar servo audio (stretch ±0.2 %).
4. **P4** (opcional, futuro): se telemetria mostrar padrão de drift permanente, implementar `StreamSynchronization` mini estilo Chromium com `SetMinimumPlayoutDelay` no audio (não no vídeo — vídeo já é best-effort).

Sample C de referência: ver `sample.c` neste diretório.
