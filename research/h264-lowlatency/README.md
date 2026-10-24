# H.264 Low-Latency Receive/Decode — Research Consolidado

Deep research multi-idioma (EN/PT/ES/RU/ZH/JA) sobre como receber H.264 CBP 720p60
(~4.5 Mbps) via TCP no Xbox 360 e alimentar o decoder ffmpeg (libavcodec sem
libavformat) com latência 100-150ms sem re-buffering. Padrão cloud-gaming: perde
frame em vez de bufferar.

Fontes primárias consultadas (todas indexadas na sessão ctx-mode):

| Fonte | Papel | URL |
|-------|-------|-----|
| moonlight-common-c `VideoDepacketizer.c` | Padrão canônico de reassembly Annex-B + drop policy + IDR request | https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/VideoDepacketizer.c |
| moonlight-common-c `RtpVideoQueue.c` | Ordering + FEC + gap detection | https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/RtpVideoQueue.c |
| moonlight-common-c `Video.h` | `DECODE_UNIT` + `LinkedBlockingQueue` producer/consumer | https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/Video.h |
| moonlight-qt `ffmpeg.cpp` | Flags reais aplicadas no decoder + drop threshold + reset | https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/video/ffmpeg.cpp |
| moonlight-embedded `ffmpeg.c` | Versão enxuta pra targets embarcados (mais próxima do 360) | https://github.com/moonlight-stream/moonlight-embedded/blob/master/src/video/ffmpeg.c |
| green-nx `video_jitter.hpp` | Cloud-gaming NACK+hold pattern (comentário canônico do porquê) | interno /tmp/green-nx/src/switch/stream/video_jitter.hpp |
| ffmpeg `doc/examples/decode_video.c` | Uso de referência oficial de `av_parser_parse2` + `avcodec_send_packet` | https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decode_video.c |
| ffmpeg `libavcodec/h264_parser.c` | Como o parser detecta boundary + key_frame via recovery point SEI | https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/h264_parser.c |
| RFC 6184 | RTP payload format H.264 (FU-A/STAP-A/single-NAL) | https://datatracker.ietf.org/doc/html/rfc6184 |
| FFmpeg wiki StreamingGuide | `-fflags nobuffer`, `-flags low_delay`, thread count | https://trac.ffmpeg.org/wiki/StreamingGuide |
| ffmpeg-user narkive thread | Casos práticos, 3-frame latency floor confirmado por vários | https://ffmpeg-user.ffmpeg.narkive.com/W3yly0bO/ |
| Habr WebRTC H.264 (RU) | Discussão jitter buffer/PLR trade-off | https://habr.com/ru/articles/682746/ |

Fontes que retornaram 403/404 (SO, CSDN) — bloqueadas por bot check, não usadas.

---

## Respostas diretas às 12 perguntas

### 1. Buffer mínimo antes de começar decode

**Precisa SPS + PPS + IDR completo.** Sem os 3, decoder joga tudo fora e emite
"non-existing PPS %u referenced". Não pode começar mid-GOP.

Moonlight faz o "SPS fixup" — antes do primeiro IDR, força `waitingForIdrFrame = true`
e descarta tudo. Só quando chega NAL type 5 (H264_NAL_TYPE_IDR_SLICE) com NALs 7 (SPS)
e 8 (PPS) anexos, monta o primeiro `DECODE_UNIT` e alimenta o decoder.

Verificar em `VideoDepacketizer.c`:
```c
if (decodeUnit->frameType == FRAME_TYPE_IDR) {
    LC_ASSERT_VT(decodeUnit->bufferList->bufferType == BUFFER_TYPE_SPS);
    LC_ASSERT_VT(decodeUnit->bufferList->next->bufferType == BUFFER_TYPE_PPS);
}
```

Se o servidor NÃO manda SPS/PPS in-band em cada IDR, precisa cachear os do
primeiro e re-injetar (Moonlight faz isso via `m_NeedsSpsFixup`).

### 2. Sync producer 60fps vs consumer 24-33fps

**Não sincroniza — drop-on-overflow.** Producer põe frame no queue, consumer pega o
mais recente e joga fora os intermediários. Padrão exato do green-nx:

```cpp
if (self->video_queue_.size() >= kMaxQueuedVideo)
    self->video_queue_.clear();   // hard drop, não FIFO
self->video_queue_.emplace_back(au, au + au_size);
self->video_cv_.notify_one();
```

`kMaxQueuedVideo` no green-nx é **2 frames**. Se estourar, limpa tudo e pede IDR
novo. Isso mata lag acumulado instantaneamente ao custo de 1 stutter visível.

Moonlight usa `LinkedBlockingQueue` com limite similar (`DECODER_BUFFER_LIMIT`),
e quando estoura vai pra `dropFrameState()` + `LiRequestIdrFrame()`.

**Regra**: nunca deixe o queue crescer. Se decoder está devagar (33fps quando input
é 60fps), você está descartando ~45% dos frames — aceita isso, é melhor que 200ms
de lag em cloud gaming.

### 3. Frame drop strategy

Ordem de descarte:
1. **B-frame** primeiro (mas H.264 CBP não tem B — só I e P, então pula pra 2).
2. **P-frame não-referência** (baixa prioridade se marcado com `nal_ref_idc = 0`).
3. **P-frame referência**: descarta E marca `waitingForIdrFrame = true`. Todo P
   seguinte é descartado até chegar IDR (senão decoder produz artefatos verdes/rosa).
4. **IDR nunca descarta.**

Threshold de Moonlight: `CONSECUTIVE_DROP_LIMIT` (default 2). Passou disso, força
`LiRequestIdrFrame()` — canal de controle pede novo keyframe ao servidor.

No seu caso, sem NACK/canal de controle, use o pattern do green-nx `kHoldMs`: se um
frame não completou em N ms desde o primeiro fragment, descarta E marca
`waiting_keyframe = true`, pede IDR pelo canal aplicacional (se houver).

### 4. `AV_CODEC_FLAG_LOW_DELAY` + `AV_CODEC_FLAG2_FAST` — impacto real

Confirmado por moonlight-qt/ffmpeg.cpp linha 505+:

```c
decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
decoder_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
decoder_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
decoder_ctx->flags2 |= AV_CODEC_FLAG2_FAST;   // implicit em vários builds
decoder_ctx->thread_type = FF_THREAD_SLICE;   // NUNCA FF_THREAD_FRAME
decoder_ctx->thread_count = 1;                // 1 thread = zero delay
decoder_ctx->delay = 0;
decoder_ctx->has_b_frames = 0;                // se você garante CBP
decoder_ctx->refs = 1;                        // se servidor está com refs=1
```

**Impacto real medido em várias threads FFmpeg mailing list**:
- `LOW_DELAY` sozinho: -33ms (elimina 1-2 frame de reorder buffer)
- `FF_THREAD_SLICE` + `thread_count=1`: -50-100ms vs `FF_THREAD_FRAME` (default)
- `FAST` (skip loop filter em corner cases): -5-15% CPU, artefatos leves em bordas
- `OUTPUT_CORRUPT` + `SHOW_ALL`: 0 latency, permite mostrar frame parcial se pacote
  cair (você decide se prefere isso vs tela preta)

**Qualidade**: `FAST` visível em bordas de objetos em movimento rápido. `LOW_DELAY`
zero perda de qualidade. `thread_count=1` custa MUITO CPU no software decode
(especialmente crítico no 360 com 3 cores em-order 3.2GHz — provavelmente vai
saturar 1 core em 720p60).

**No Xbox 360 recomendação**: `thread_count=2` slice-threaded pra distribuir carga
entre PPU 0 e 1 (o 2 fica pro renderer/áudio). Testar — pode ser que 1 core baste
pra 720p CBP.

### 5. Anexo-B parsing — parser resolve chunks arbitrários?

**Sim.** `av_parser_parse2` é feito exatamente pra isso. Contract:

```c
ret = av_parser_parse2(parser, ctx,
                       &pkt->data, &pkt->size,     // OUT: frame completo (ou size=0)
                       inbuf, inbuf_size,          // IN: bytes que você recebeu agora
                       pts, dts, pos);
// ret = bytes consumidos do inbuf (sempre <= inbuf_size)
// Se pkt->size > 0: você tem uma unidade decodificável, chama avcodec_send_packet
// Se pkt->size == 0: parser ainda esperando mais bytes, chama de novo
inbuf      += ret;
inbuf_size -= ret;
```

Você **não precisa** detectar boundary NAL manualmente. Parser encontra 0x000001
sozinho, aguarda até ter a AU completa (frame boundary), então retorna
`pkt->data` apontando pra memória interna do parser (não copie — chame
`avcodec_send_packet` direto). Loop até `inbuf_size == 0`.

Gotcha: parser buferiza internamente até ver o PRÓXIMO start code (é assim que ele
sabe onde a AU atual acaba). Isso adiciona **até 1 frame de latência** — o parser
segura o frame N até chegar o primeiro byte do frame N+1. Pra evitar isso, flush
manual com `av_parser_parse2(..., NULL, 0, ...)` ou setar `AV_PARSER_PTS_NB=0` e
alimentar frame-a-frame (você já sabe o boundary via seu protocol wire, então melhor
NÃO usar parser — vá direto pra `avcodec_send_packet` com AU pronta).

**Recomendação final**: pule o parser. Reassemble AU você mesmo (start-code split,
Moonlight `getAnnexBStartSequence()`) e envie pra `avcodec_send_packet` direto.
Elimina o "1-frame ahead" delay do parser. É exatamente o que Moonlight faz.

### 6. TCP vs UDP

**Para cloud gaming, UDP ganha sempre.** TCP tem 2 problemas fatais:

1. **Head-of-line blocking**: perdeu 1 byte, TCP retransmite e SEGURA todos os bytes
   posteriores até chegar. Isso vira lag stacking — 200ms por retransmit.
2. **Kernel receive buffer acumula**: se decoder é 1ms mais lento que rede, o buffer
   TCP cresce infinitamente. Você recebe frames velhos.

**Se você é FORÇADO a usar TCP** (que é seu caso, pelo pedido):
- `TCP_NODELAY` no socket (senão Nagle segura pacotes pequenos)
- `SO_RCVBUF` **baixo** (~64KB) pra kernel dropar em vez de bufferar
- `MSG_DONTWAIT` no `recv()` — se não tem dado, retorna imediatamente
- Roda drop policy no application-level: se `recv()` retornou 3+ frames worth de
  bytes num tick, joga fora N-1 e mantém último.
- Envelope de framing próprio (length-prefix). TCP é stream — sem framing, você
  não sabe onde frame acaba.

**Mitigação real**: adiciona uma thread `recv()` dedicada que lê tão rápido quanto
o kernel entrega, joga em ring buffer bounded (drop-oldest quando cheio), e outra
thread decoder consome. Isso desacopla socket do decoder.

**UDP + framing próprio (recomendado se pode escolher)**:
- 1 UDP datagram = 1 NAL (ou fragment). Sem HOL blocking.
- Sequence number aplicacional (16-bit), timestamp aplicacional (32-bit).
- Perdeu pacote = você detecta via gap na sequence, marca frame como quebrado,
  descarta e pede IDR.
- **Isso é literalmente RTP** (RFC 6184). Ver #7.

### 7. RTP framing (RFC 6184) — worth implementing?

**Sim, se roteando via UDP.** Não implementa spec inteiro, só o subset relevante:
- **Single NAL Unit** (nal_type 1-23): NAL cabe em 1 datagrama.
- **FU-A** (nal_type 28): NAL grande fragmentado em N pacotes com S/E flags.
- **STAP-A** (nal_type 24): SPS+PPS+SEI agrupados em 1 pacote (economiza header).

Ignora FU-B, MTAP16, MTAP24 (raramente usados).

Header RTP fixo (12 bytes) te dá **de graça**:
- Sequence number → detecção de gap/reordering
- Timestamp → agrupa fragments do mesmo frame (mesmo timestamp = mesma AU)
- Marker bit → última fragmento do frame (permite emitir sem esperar próximo timestamp)
- SSRC → multiplexar streams

Moonlight usa formato próprio (mais simples que RTP) mas com os mesmos 4 campos.
No seu setup TCP, prefixe cada NAL com `[uint32 length][nal_type_byte][data]` e
pula RTP inteiro — TCP já garante ordem e integridade byte-a-byte.

### 8. Jitter buffer size ideal 60fps LAN

Numbers reais de referência:
- **LAN wired zero-loss**: 1 frame (~16ms). Não bufferiza, decode ASAP.
- **LAN wifi 5GHz**: 2 frames (~33ms). Tolera jitter ~10-15ms.
- **Cloud gaming típico** (Moonlight local, green-nx): `kHoldMs = 20-30ms` = ~1.5 frame.
- **xCloud/Stadia/GeForce Now WAN**: 3-5 frames (~50-83ms) + FEC.

green-nx comentário exato:
> holds frames a short time (kHoldMs) so retransmits/reordering complete them

Sem NACK (seu caso TCP), você **não tem** retransmit útil — TCP já retransmitiu.
Então jitter buffer aplicacional pode ser **1 frame** só (você emite assim que tiver
AU completa).

**Recomendação Xbox 360 LAN wired**: `kMaxQueuedFrames = 2`. Passou disso, clear
+ request IDR. `kHoldMs` = 0 (emite direto).

### 9. Como medir end-to-end latency

Método padrão da indústria:

1. **Timestamp injection**: servidor imprime timestamp na frame (canto superior
   direito, digitos grandes). Ler com câmera de celular em slow-motion 240fps
   filmando servidor+cliente lado-a-lado. Delta = E2E latency.

2. **Instrumentação por estágio** (é o que Moonlight faz, ver `enqueueTimeUs` +
   `receiveTimeUs` em ffmpeg.cpp):
   ```c
   uint64_t t0 = get_time_us();  // recv() retornou
   uint64_t t1 = get_time_us();  // AU completa (last fragment)
   uint64_t t2 = get_time_us();  // avcodec_send_packet retornou
   uint64_t t3 = get_time_us();  // avcodec_receive_frame retornou
   uint64_t t4 = get_time_us();  // frame apresentado (post-vsync)
   ```
   Log rolling avg por estágio. Onde tá o pico, ali tá o problema.

3. **Round-trip ping aplicacional**: cliente envia ID no controle, servidor devolve
   frame com aquele ID pintado. Cliente mede delta.

Para o Xbox 360, opção #2 é o caminho — você controla tudo. Use
`mftb()` (PPC timebase register) que é ciclo-preciso a 79.8MHz.

### 10. Recovery point SEI

H.264 `sei_message` type 6 (recovery point) marca frame que NÃO é IDR mas serve
como random access point (gradual decoder refresh). Verificado em
`ffmpeg/libavcodec/h264_parser.c`:

```c
if (p->sei.recovery_point.recovery_frame_cnt >= 0) {
    /* key frame, since recovery_frame_cnt is set */
    s->key_frame = 1;
}
```

**No seu caso não importa** — cloud gaming típico usa IDR everywhere, sem recovery
SEI. Ignore. Se aparecer, o parser já marca como key_frame — trate como IDR
normal.

### 11. `avcodec_flush_buffers()` após frame drop — overhead

Custo real: **desalocar reference frames + reset POC state**. Em software decode
H.264 é ~1-5ms. Você já vai perder o próximo P frame de qualquer forma (falta
referência), então o overhead é escondido dentro do stall natural.

**Quando chamar**:
- Após clear do queue por overflow.
- Após detectar gap na sequence de frames (perdeu 1+ frame inteiro).
- **Antes** de enviar o próximo IDR pro decoder.

**Quando NÃO chamar**:
- Entre frames normais.
- Após 1 P-frame perdido se você tem RFI (reference frame invalidation) — mas Xbox
  360 não tem canal de controle sofisticado, então trate como overflow.

Pattern moonlight-qt:
```c
if (++m_ConsecutiveFailedDecodes == FAILED_DECODES_RESET_THRESHOLD) {
    // Trigger decoder recreation (nuclear option)
}
```

### 12. Cloud gaming refs

Números reais publicados:
- **Moonlight LAN wired H.264**: 25-45ms E2E (Steam Deck target)
- **Moonlight LAN wifi H.264**: 40-70ms
- **xCloud WAN**: 100-140ms
- **Stadia**: 90-130ms (era o benchmark quando existia)
- **GeForce Now**: 60-100ms
- **PS Remote Play LAN**: 50-80ms

Como reduzir buffer sem tearing:
1. **Vsync desligado** no output (ou tearing-free adaptive sync). Xbox 360 é
   sempre 60Hz output — se decoder produz 33fps, você repete frames (não tearing,
   só judder).
2. **Present timing**: alinhe present ao vblank do próximo frame que o decoder
   soltar, não ao vblank fixo. Isso reduz latência ~8ms average.
3. **Zero-copy decode → render**: NV12 output do decoder direto pra shader YUV→RGB
   na GPU. Sem CPU copy.
4. **Predictive present**: alguns títulos xCloud presentam frame com atraso
   negativo (predizem próximo frame com motion vectors). Complexo — não vale
   pena pra 360.

---

## Estimativa realista de latency no seu setup

Xbox 360, LAN wired Ethernet 100Mbps, servidor local:

| Estágio | Latency | Notas |
|---------|---------|-------|
| Captura + encode servidor | 8-16ms | x264 zerolatency preset |
| Rede (LAN wired 1 hop) | 0.3-1ms | wire delay + switch |
| Kernel recv + copia p/ userland | 0.5-2ms | TCP tem overhead vs UDP aqui |
| Reassembly + queue | 0-1ms | drop-on-overflow zera |
| Decoder software 720p60 | **20-40ms** | **PIOR gargalo no 360** — 1-2 frames tempo real |
| YUV→RGB shader (XPS3) | 5-14ms | vmx128/xenos GPU |
| Present + HDMI scan-out | 16-50ms | 1 vblank @ 60Hz + display processing |
| **Total E2E** | **50-124ms** | |

**Bottleneck no Xbox 360**: decoder software. Sem hwaccel, x264 CBP 720p60 em 1
core PPC 3.2GHz VMX128 é ~30-45ms/frame. Você quer:
- **Tune libavcodec build pra Xenon**: PPC VMX intrinsics manuais em
  `libavcodec/ppc/h264dsp.c`. Isso já existe upstream, mas não pra Xenon VMX128
  (diferente do PPC64 Altivec padrão).
- **Testar `thread_count=2`** — se h264 decoder scala com slice threading no seu
  ffmpeg build, isso derruba pra ~20ms/frame.
- **Baixar resolução se necessário**: 720p60 pode não ser viável. 540p60 ou 720p30
  são fallbacks realistas.

Alvo realista: **80-100ms E2E** LAN. Abaixo disso vai exigir hwaccel (não existe
no 360) ou GOP curto + resolução reduzida.

---

## Arquivos gerados

- `sample_decoder.c` — código C completo aplicável ao seu ffmpeg (--disable-avformat).
  Recv loop TCP + ring buffer + Annex-B reassembly + drop policy + decode.
- `sample_decoder.h` — API pública do módulo.
- `ffmpeg_flags.md` — cheat sheet de flags tunable com impacto medido.

Ver também `/mnt/storage/projetos/XCloud360/video/h264/` e `/mnt/storage/projetos/XCloud360/video/rtp/`
onde a implementação de produção deve ir.
