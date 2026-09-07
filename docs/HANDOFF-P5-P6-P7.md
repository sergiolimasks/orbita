# HANDOFF — P5/P6/P7 sessão 2026-09-07

## Status geral

Pipeline **end-to-end** entregue em software (aguarda validação hardware):

```
GOLDEN STREAM (sintético)  →  libavcodec H.264 decoder  →  BT.601 CPU YUV→RGB  →  console_pset present
[tools/xcloud-capture/output/  [app/h264dec, app/h264play]  [inline no h264play]      [libxenon]
 GOLDEN-synth-720p60-baseline/]
```

## Fases

### P5 — xcloud-capture (COMPLETED via pivot)

**Objetivo:** capturar stream H.264 real do xCloud pra decoder dev.

**Rota original — capture-rtp** (`tools/xcloud-capture/rtp/capture-rtp`):
- Compilou e roda ponta a ponta até ICE CONNECTED
- libpeer patchado com todos os hunks obrigatórios (Green-NX):
  - Bio callback retorna `WANT_READ` em vez de -1/0 (evita CONN_EOF prematuro)
  - Role DTLS reinicializa quando answer traz `a=setup:passive`
  - `mbedtls_ssl_conf_handshake_timeout(1000, 4000)` + `set_mtu(1200)` + `session_reset` antes de handshake_client
  - Retry loop de 5 attempts por dead-media path
- **Bloqueio residual**: xCloud provisiona sessões com "dead media path" — ICE fecha, ClientHello enviado, xCloud responde 0 pacotes DTLS. Green-NX documenta isso como fault server-side legítimo. Retry ajuda mas sessão de teste não fechou (2/2 dead na sessão).

**Pivot — GOLDEN synth** (`tools/xcloud-capture/output/GOLDEN-synth-720p60-baseline/`):
- 30s de H.264 gerado por ffmpeg + libx264 batendo specs xCloud exatas
- Constrained Baseline profile_idc=0x42, level 3.1, 1280×720@60fps, yuv420p, CAVLC, 0 B-frames
- ~4.5 Mbps, GOP 2s (120 frames), SPS/PPS por IDR
- 16.5 MB stream.h264 + 618 KB audio.opus + analysis.json/txt + sha256 + session-meta

**Versão embedável 5s** (`GOLDEN-synth-720p60-baseline-5s/`): 2.8 MB — usado nos apps h264dec/h264play.

### P6 — H.264 decoder base decision + build (COMPLETED)

**ADR-007** (`docs/adr/ADR-007-h264-decoder-base.md`): rota escolhida = **FFmpeg build próprio salvage do xmplayer/Ced2911**. Rejeitados: edge264 (LE-only), OpenH264 (sem SIMD PPC = 15-25fps).

**Build ffmpeg pra Xenon** (`~/ffmpeg-xenon/`):
- Fonte: `~/xmplayer/mplayer/ffmpeg` (Ced2911, LibXenonProject)
- Configure: `--arch=powerpc --cpu=cell --enable-cross-compile --cc=xenon-gcc --disable-altivec --disable-everything --enable-decoder=h264 --enable-parser=h264`
- Patches aplicados:
  - `libavutil/xenon/bswap.h`: remover `#include <byteswap.h>` (glibc-only, usa `__builtin_bswap*`)
  - `config.h`: `HAVE_STRERROR_R 0` (libxenon libc sem strerror_r)
  - `config.mak`: remover `-Werror=implicit-function-declaration -Werror=missing-prototypes`
  - `libavcodec/h264dsp.c`: restaurar guard `if (HAVE_ALTIVEC) ff_h264dsp_init_ppc(...)`
  - `libavcodec/dsputil.c`: mesmo `if (ARCH_PPC && HAVE_ALTIVEC)`
- **AltiVec DESABILITADO** — GCC 9.2 do xenon-gcc não suporta builtins `__builtin_vec_sum4s`, `__builtin_vec_msum`, `__builtin_vec_mladd` requeridos por `libavcodec/ppc/*.c`. Perf baseline será pure-C.
- Output: `libavcodec.a` (19 MB), `libavutil.a` (1.2 MB), 0 undefined refs, símbolos H.264 core presentes (`avcodec_decode_video2`, `ff_h264_decode_init`, etc.)

**app/h264dec** (`app/h264dec/h264dec.elf32` 5.1 MB, sha256 eb2e451052b454e2...):
- Skeleton benchmark: embedded 5s stream → libavcodec decode → mede fps
- Reporta `mean ms/frame` + `effective fps` + verdict PASS(≥55)/PARTIAL(≥30)/FAIL(<30)
- BSS 1.5 MB, text 5.1 MB, entry 0x801d3400 — cabe folgado em 512 MB
- **Aguarda teste hardware**

### P7.0 — YUV→RGB CPU fallback (COMPLETED)

**app/h264play** (`app/h264play/h264play.elf32` 5.1 MB, sha256 23da361f78132d11...):
- Pipeline completo: decode → BT.601 integer Q10 convert → console_pset present
- Reporta por segundo: dec_ms + cvt_ms + tot_ms + fps
- Baseline realista: se decode>=45fps e convert+present dentro de 5ms, passa 60fps
- Se falhar, aponta pra P7.1 (VMX128 SIMD) ou P7.2 (Xenos shader)

## Como testar (Sergio)

Devbox → Xbox 360 via HTTP netboot:

```bash
# Devbox: serve o ELF
cd /mnt/storage/projetos/XCloud360
./scripts/serve-xell app/h264play/h264play.elf32
# ou pra só o decoder benchmark:
./scripts/serve-xell app/h264dec/h264dec.elf32
```

No Xbox 360 RGH com XeLL Reloaded:
1. XeLL menu → "Load from network"
2. URL: `http://<devbox-ip>:8032/build/xenon.elf`
3. Boot

Reportar:
- **h264dec**: mean_ms/frame, fps, verdict
- **h264play**: dec_ms, cvt_ms, tot_ms, fps por segundo

## Next steps por resultado hardware

| Verdict h264play | Rota |
|---|---|
| PASS >=55fps | Já ganhou. Substitui GOLDEN synth por captura xCloud real quando dead-media pass, plug decoder no capture-rtp |
| PARTIAL 30-55fps | P7.1: acelerar convert em VMX128 (baixo risco, já sabemos AltiVec builtins não linkam mas asm direto sim). Se ainda ruim, P7.2 shader Xenos |
| FAIL <30fps | Decoder é o gargalo. P7.2 obrigatório: Xenos shader path exige shadercompiler.exe (XNA 3.0 + VS 2008 em VM Windows) — bloqueio ferramenta |

## Blocos residuais

- **xCloud dead-media**: bug real server-side, mitigado por retry mas não eliminado. GOLDEN synth resolve por hora.
- **VMX128 SIMD**: GCC 9.2 xenon-gcc não suporta `__builtin_vec_*`. Alternativas: inline asm (`__asm__` blocks), .S handwritten, ou usar `sepbi-vmx128` binutils patch (Sebastian Biallas 2007, nunca mergeou upstream).
- **Xenos shader compiler**: proprietary. Fallback: reusar shaders binários de XBMC-360 se houver YUV→RGB pronto.

## Arquivos-chave

```
/mnt/storage/projetos/XCloud360/
├── docs/
│   ├── adr/ADR-007-h264-decoder-base.md    (decisão de rota P6)
│   └── HANDOFF-P5-P6-P7.md                 (este doc)
├── tools/xcloud-capture/
│   ├── rtp/capture-rtp                     (build PC, patches libpeer + retry)
│   └── output/
│       ├── GOLDEN-synth-720p60-baseline/       (30s stream + analysis)
│       └── GOLDEN-synth-720p60-baseline-5s/    (5s stream pra embed)
├── app/
│   ├── h264dec/h264dec.elf32               (decoder benchmark)
│   └── h264play/h264play.elf32             (pipeline completo)
└── scripts/serve-xell                       (HTTP netboot XeLL)

~/ffmpeg-xenon/                               (libavcodec.a + libavutil.a pra Xenon)
~/libpeer/                                    (patchado — bio + role + timeout + mtu)
~/green-nx/                                   (referência protocol + patches)
~/xmplayer/                                   (referência build ffmpeg)
```
