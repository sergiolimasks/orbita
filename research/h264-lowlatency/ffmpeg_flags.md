# FFmpeg libavcodec H.264 low-latency flags — cheat sheet

Todos os flags aplicados via `AVCodecContext` **antes** de `avcodec_open2()`.
Fonte: moonlight-qt/app/streaming/video/ffmpeg.cpp linhas 505+ e código do
`libavcodec/h264dec.c`. Testado em ffmpeg 4.x, 5.x, 6.x.

## Aplicar sempre

| Flag | Efeito medido | Trade-off |
|------|---------------|-----------|
| `flags \|= AV_CODEC_FLAG_LOW_DELAY` | -1 frame reorder buffer (~16-33ms) | zero |
| `thread_type = FF_THREAD_SLICE` | Elimina delay de thread_count-1 frames | precisa multi-slice encoder |
| `thread_count = 1` a `2` | 1 = zero delay; 2 = throughput +40% | 2 exige coordenação |
| `delay = 0` | Confirma zero pipeline buffer | precisa stream sem B-frame |
| `has_b_frames = 0` | Elimina 1 frame delay | quebra se stream tiver B |

## Aplicar se quer "show something even if broken"

| Flag | Efeito |
|------|--------|
| `flags \|= AV_CODEC_FLAG_OUTPUT_CORRUPT` | Emite frame parcialmente decodificado |
| `flags2 \|= AV_CODEC_FLAG2_SHOW_ALL` | Mostra tudo desde o primeiro pacote (mesmo pre-IDR) |
| `flags2 \|= AV_CODEC_FLAG2_FAST` | Pula alguns loop filter edge cases; -5-15% CPU |

## NÃO aplicar (comum em confusão)

| Flag | Por quê |
|------|---------|
| `FF_THREAD_FRAME` | **CAUSADOR PRINCIPAL DE LATÊNCIA.** Adiciona thread_count-1 frames de delay. |
| `AV_CODEC_FLAG_TRUNCATED` | Deprecated; use parser ou framed input. |
| `refs = N > 1` sem match do encoder | Aloca memória extra sem ganho. |

## Equivalente CLI (pra sanity check via ffplay)

```
ffplay -fflags nobuffer -flags low_delay -framedrop \
       -strict experimental -avioflags direct \
       -probesize 32 -analyzeduration 0 \
       -sync ext -vf setpts=0 \
       -threads 1 tcp://server:port
```

Isso reproduz o pipeline. Se ffplay dá latência alta com isso, o problema é do
encoder, não do decoder.

## Não-óbvios que ajudam

- **`AV_INPUT_BUFFER_PADDING_SIZE`** no fim do buffer: obrigatório senão SIMD
  overshoot lê lixo e às vezes segfault. Já feito no `sample_decoder.c`.
- **`av_packet_unref()` antes de reusar** `AVPacket`. Deixar buffer velho no pkt
  às vezes decoder segura ref, atrasa.
- **`AV_NOPTS_VALUE` em pts/dts** se você não tem timing preciso. Passar timestamps
  errados faz decoder tentar reorder e mata latência.
- **`avcodec_flush_buffers()`** após IDR request. Zera reference frames, próximo
  IDR decodifica sem tentar buscar refs velhas.

## Build-time considerations pro Xbox 360

Ao compilar libavcodec pro Xenon:
- `--enable-small` = binário menor (importante pra 512MB RAM total)
- `--disable-everything` + `--enable-decoder=h264` = zero bloat
- `--enable-optimizations --enable-asm` = usar PPC assembly do libavcodec
- **`--cpu=cell`** ou `--cpu=ppc750` são os mais próximos ao Xenon disponíveis
  no configure default (Xenon = PPC64 in-order 3.2GHz VMX128, semelhante ao
  Cell PPE)
- **Custom `libavcodec/ppc/h264dsp.c` com VMX128 intrinsics**: existe patchset
  não-oficial (ver ffmpeg-xbox fork Wolf3s) — pode dar 20-40% speedup em decode.

## Threads: 1 ou 2?

Xenon tem 3 cores hardware, 2 threads/core = 6 hw threads. Uso típico:
- Thread 0: main loop + input
- Thread 1: renderer (GPU submit)
- Thread 2: audio
- Threads 3-5: **livres**

Portanto `thread_count = 2` slice-threaded roda em cores livres sem prejudicar
outros subsistemas. **Sempre teste com 1 primeiro** — se decoder consegue
sustentar 60fps single-threaded, mantenha 1 (zero overhead de sync).

Benchmark caseiro: mede `avcodec_send_packet` + `avcodec_receive_frame` round
trip. Se ficar consistentemente < 16ms com thread_count=1, tá bom.
