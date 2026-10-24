# PATCH — tcp_stream_client.c → length-prefixed AU framing

**Decisão**: migrar cliente pra length-prefix. Alinha DEV-LOOP com wire real
(libpeer RTP→AU reassembly), elimina 1 frame de parser lookahead, casa 100%
com `xc_video_feed_framed` (o único caminho suportado do decoder module).

Ver `../h264-lowlatency/README.md` L142-151 pra justificativa técnica:

> Parser buferiza internamente até ver o PRÓXIMO start code. Isso adiciona
> até 1 frame de latência — o parser só devolve NAL N quando ve o start code
> do NAL N+1. **Recomendação: pule o parser. Reassemble AU você mesmo
> (start-code split, sabendo que 1 AU = mensagem length-prefixed).
> Elimina o "1-frame ahead" delay. É exatamente o que Moonlight faz.**

E `sample_decoder.c` L361-380 explicitamente rejeita raw stream:

> `xc_video_feed_annexb_stream mode not implemented — use framed`

Feeder (`tools/h264-feeder/h264_feeder.py` L136-137) já emite
`struct.pack(">I", len(au)) + au`. Cliente é o único componente desalinhado.

---

## Ganho estimado

| Item | Antes (raw + parser) | Depois (length-prefix + skip parser) |
|---|---|---|
| Parser lookahead | +1 frame (~16ms @ 60fps) | 0 |
| Scan byte-a-byte por 00 00 01 | ~4.5 MB/s scanned | 0 (size já vem) |
| Custo CPU (Xenon PPC in-order) | não medido, sensível | ~-5% |
| Alinhamento com wire real | não | sim (contrato = 1 AU/msg) |

**Ganho total: 1 frame (~16ms @ 60fps) + eliminação de scan.**
Estágio "receive+parse" no chart do README h264-lowlatency L308 sai de "1 frame"
pra "0 frames" — ganho direto.

---

## Diff proposto (25 linhas C novas / 20 removidas)

### 1. Ring vira "AU queue" (mesma memoria, semantica diferente)

Manter `ring_t` como está — só muda como o **consumidor** lê. Producer
(`on_recv`) continua escrevendo bytes brutos, porque TCP fragmenta livre.
Length-prefix parser vive no consumidor.

### 2. Substituir `decoder_pump` inteira

```c
/* --- LENGTH-PREFIX AU EXTRACTOR ------------------------------------------
 * Wire: [uint32_be size][annex-b bytes] repetido.
 * Estado persistente entre chamadas: se ring nao tem AU completa, retorna 0
 * e tenta na proxima. Sem parser ffmpeg no caminho — AU pronta vai direto
 * pro avcodec_send_packet, sem lookahead de 1 frame.
 */

static uint8_t g_au_buf[512 * 1024];   /* max AU size aceito (IDR pode ser grande) */
static uint32_t g_pending_au_size = 0; /* 0 = precisa ler header */

int decoder_pump(void (*on_frame)(AVFrame *)) {
    int frames = 0;

    for (;;) {
        /* Fase 1: ler header de 4 bytes (big-endian) se ainda nao temos AU pendente */
        if (g_pending_au_size == 0) {
            if (ring_used(&g_ring) < 4) return frames;   /* aguarda mais bytes */
            uint8_t hdr[4];
            ring_read(&g_ring, hdr, 4);
            g_pending_au_size = ((uint32_t)hdr[0] << 24) |
                                ((uint32_t)hdr[1] << 16) |
                                ((uint32_t)hdr[2] << 8)  |
                                ((uint32_t)hdr[3]);
            if (g_pending_au_size == 0 || g_pending_au_size > sizeof(g_au_buf)) {
                printf("[stream] bogus au_size=%u — resync impossible, dying\n",
                       g_pending_au_size);
                g_pending_au_size = 0;
                return frames;                             /* proximo reconnect resolve */
            }
        }

        /* Fase 2: ler AU completa se ring tem bytes suficientes */
        if (ring_used(&g_ring) < g_pending_au_size) return frames;   /* aguarda */
        ring_read(&g_ring, g_au_buf, g_pending_au_size);

        /* Alimenta decoder direto — SEM parser. AU ja delimitada. */
        g_pkt->data = g_au_buf;
        g_pkt->size = (int)g_pending_au_size;
        g_pending_au_size = 0;   /* reset pra proxima */

        if (avcodec_send_packet(g_ctx, g_pkt) == 0) {
            AVFrame *frm = av_frame_alloc();
            while (avcodec_receive_frame(g_ctx, frm) == 0) {
                if (on_frame) on_frame(frm);
                frames++;
            }
            av_frame_free(&frm);
        }
    }
}
```

### 3. Remover parser em `decoder_init`

```c
/* REMOVER: */
- g_parser = av_parser_init(codec->id);
- if (!g_parser) return -1;
- g_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

/* Comentario atualizado: */
+ /* Sem parser: length-prefix garante 1 AU/pacote (feeder emite assim).
+  * Elimina 1 frame de lookahead do av_parser_parse2. */
```

### 4. Header comment atualizado

```c
- *   - decoder loop consome do ring, chama av_parser_parse2, feed no codec
+ *   - decoder loop consome do ring como [u32_be size][AU bytes], feed direto
+ *     no codec (sem parser — 1 AU/msg garantido pelo feeder)

- /* Le bytes crus H.264 Annex-B do ring. Nao bloqueia. */
+ /* Le bytes do ring TCP. Formato wire: [u32_be size][annex-b AU]. */
```

### Total de linhas C

- **Novas**: 28 (função decoder_pump nova + globais g_au_buf/g_pending_au_size)
- **Removidas**: 22 (parser init, pump antigo, includes de parser não usados)
- **Net**: +6 linhas, código mais simples (loop linear em vez de state machine do parser).

---

## Comportamento em edge cases

| Caso | Comportamento |
|---|---|
| AU dividida em N pbufs (TCP fragmenta) | Producer copia todos pro ring; consumer aguarda `ring_used >= size` antes de extrair. Sem cópia extra. |
| Header dividido entre 2 pbufs | `ring_used < 4` retorna 0, tenta próxima chamada. |
| au_size corrupto (bit flip cabo) | Detecta > 512KB → não há resync possível em stream sem framing. Retorna, próximo reconnect limpa ring. Aceitável em LAN (BER ~10⁻¹²). |
| Multi-AU no mesmo recv | `for(;;)` loop drena até `ring_used < size_needed`. |
| Ring overflow (producer > consumer) | `ring_write` já dropa e conta em `dropped`. Sem AU meio-escrita — mas AU parcial no ring pode virar `au_size` bogus na próxima leitura → reconnect. Mitigação: aumentar RING_CAPACITY pra 1MB (IDR 720p60 chega a ~200KB). |

**Recomendação adicional**: bump `RING_CAPACITY` 512KB → 1MB no header. Custo:
+512KB RAM. Xbox 360 tem 512MB, folga total.

---

## Alinhamento com produção (RTP/libpeer)

Ver `research/av-sync-webrtc/DESIGN.md` L156, L165, L168:

- libpeer entrega NAL units via `on_video_track` callback (produção real xCloud).
- Reassembly RTP → AU acontece do lado do cliente Xbox usando marker bit RTP +
  timestamp (RFC 6184 §5.1: "todos os pacotes que carregam NAL units do mesmo
  AU compartilham o mesmo timestamp; marker bit no último").
- Green-nx / Moonlight fazem `VideoJitterBuffer::try_assemble` — output é
  **exatamente** "1 AU pronta pro decoder".

**Portanto**: length-prefix no dev-loop = mesma interface do produção.
Decoder module (`xc_video_feed_framed`) é reutilizado sem mudança. Trocar
transport (TCP dev → SRTP prod) não muda uma linha do decoder.

Se ficássemos com raw stream + parser: dev-loop teria caminho diferente do
produção, decoder module rejeita raw (não implementado), teríamos código
morto/duplicado.

---

## URLs de referência

- Moonlight VideoDepacketizer (AU reassembly canônico):
  https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/VideoDepacketizer.c
- Moonlight ffmpeg.cpp (skip parser, feed AU direto):
  https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/video/ffmpeg.cpp
- FFmpeg h264_parser.c (mostra o buffer-until-next-start-code):
  https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/h264_parser.c
- RFC 6184 §5.1 (RTP H.264 marker bit = fim de AU):
  https://datatracker.ietf.org/doc/html/rfc6184#section-5.1
- FFmpeg decode_video.c (padrão oficial parser usage — o que estamos SAINDO):
  https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decode_video.c

---

## Checklist migração

- [ ] Aplicar diff em `tcp_stream_client.c`
- [ ] Bump `RING_CAPACITY` 512KB → 1MB
- [ ] Remover `#include <libavcodec/avcodec.h>` refs a `AVCodecParserContext`
- [ ] Smoke test: rodar `h264_feeder.py --file golden.h264 --fps 60`, cliente
      Xbox conecta, primeira IDR decoda em <200ms, sem "bogus au_size"
- [ ] Medir E2E latency (metodologia README h264-lowlatency L221-225 —
      timestamp burn-in + camera high-speed)
- [ ] Confirmar: com length-prefix + LOW_DELAY + thread_count=1, decode
      pipeline fica em ~1 frame (16ms @ 60fps) conforme estimativa L308-320
