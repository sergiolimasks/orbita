# ADR-007 — H.264 CBP decoder base decision (Xenon RGH)

**Status:** DRAFT — decision pending Sergio.
**Date:** 2026-09-07
**Context:** P6. GOLDEN STREAM já capturado (CBP 3.1, 1280x720@60, CAVLC, yuv420p, 0 B-frames, ~4.5Mbps, GOP 2s, ~9KB p50 / 30KB max NAL). Alvo perf: 60fps sustentado (fallback 55-60). Toolchain: `xenon-gcc 9.2.0` (libxenon devkitxenon), PowerPC 32-bit BE, VMX128 SIMD, sem hardware Xenos block H.264.

---

## TL;DR — Ranking

| # | Base | Esforço | Perf esperada | Risco | Licença |
|---|------|---------|--------------|-------|---------|
| **1º** | **FFmpeg fork WiiU pattern (own build p/ Xenon)** | 8-14 dias | 45-60fps (com AltiVec) | Médio | LGPL |
| **2º** | **XMPlayer/Ced2911 salvage (libavcodec já buildado p/ libxenon)** | 5-10 dias | 25-40fps sem otim | Baixo (código existe) | GPL |
| **3º** | **OpenH264 (Cisco) minimal + ANSI-C fallback** | 12-20 dias | 20-35fps (sem SIMD) | Alto (BE não validado) | BSD-2 |
| 4º (rejeitado) | edge264 | — | — | LE-only, BE explicitamente não suportado | ISC |
| 5º (fallback) | Baseline decoder from-scratch | 30-60 dias | 40-55fps otim custom | Muito alto (tempo) | own |
| GPU-assist | Xenos shader kernels (paper Baeza 2007) | 20+ dias adicional | +15-25fps boost | Alto (não open) | — |

**Recomendação:** **Rota 1 (FFmpeg próprio, seguindo padrão `wiiu.sh`)** com **Rota 2 como salvage de código** — copiar o que XMPlayer/Ced2911 já provou funcionar e refazer build config limpo para xenon-gcc 9.2.

---

## 1. FFmpeg build próprio (padrão wiiu.sh) — **1ª OPÇÃO**

### Evidência que funciona
- **`hrydgard/ppsspp-ffmpeg`** com `wiiu.sh` cross-compila ffmpeg completo para PowerPC 750 (Wii U, também BE, também no-OS):
  ```
  --enable-cross-compile --arch=powerpc --cpu=750 
  --cc=powerpc-eabi-gcc --cross-prefix=powerpc-eabi-
  --extra-cflags="-O3 -DWORDS_BIGENDIAN -mcpu=750 -meabi -mhard-float"
  --disable-everything (+ enable seletivo)
  ```
- **`GaryOderNichts/FFmpeg-wiiu`** — port ffmpeg completo pro Wii U (Espresso PPC 750CL, mesmo mundo).
- **`Wolf3s/ffmpeg-xbox`** — port ffmpeg pro XBMC-360 (X/SDK, não libxenon, mas confirma que ffmpeg builda pra Xenon). 2 branches: `ffmpeg-ffplay` (original) + `ffmpeg-...-updated` (zlib+pthreads bump).
- **`LibXenonProject/xmplayer`** — MPlayer + libavcodec já rodou em RGH via libxenon (versão bundled é antiga, mas prova PoC).
- **`libavcodec/ppc/h264_altivec.c`** existe upstream: IDCT (`ff_h264_idct8_add4_altivec`), MC template (`h264_template_altivec.c` com `vec_mladd`/`vec_perm`), deblock, CAVLC helpers.

### Config recomendada (baseada em wiiu.sh, adaptada Xenon)
```bash
./configure \
  --target-os=none --arch=powerpc --cpu=cell \
  --enable-cross-compile \
  --cc=xenon-gcc --cross-prefix=xenon- --nm=xenon-nm \
  --extra-cflags="-O3 -m32 -mhard-float -mcpu=cell -mtune=cell \
                  -maltivec -mabi=altivec \
                  -DWORDS_BIGENDIAN -D__powerpc__ -D__XENON__ \
                  -ffunction-sections -fdata-sections \
                  -fno-strict-aliasing" \
  --disable-shared --enable-static \
  --disable-everything \
  --disable-avdevice --disable-avfilter --disable-postproc \
  --disable-encoders --disable-muxers --disable-network \
  --disable-programs --disable-doc \
  --enable-decoder=h264 \
  --enable-parser=h264 \
  --enable-demuxer=h264 \
  --enable-protocol=file \
  --enable-altivec
```
> `--cpu=cell` é o mais próximo do Xenon suportado upstream (PS3 PPU / Cell BE tem AltiVec regular, sem VMX128). Xenon = subset de AltiVec + adições VMX128; upstream ffmpeg só usa AltiVec base, o que é OK.

### Footprint esperado
- libavcodec.a stripped, decoder-only H.264+parser: **~2.5-4 MB** (mais que rodável nos 512MB RAM do Xenon).
- ffmpeg completo (mesmo com `--disable-everything`): headers e symbols ainda pesados, mas P6 usa apenas libavcodec API direta — não linka programs.

### Perf esperada
- H.264 AltiVec no Cell PPU (mesmo clock 3.2GHz, mesma família): **~35-50fps 720p8 CBP**. Xenon com VMX128 poderia melhorar levemente.
- Sem AltiVec (C puro): ~15-25fps 720p60 → **inviável**.
- Kernels custom VMX128 substituindo AltiVec: potencial +10-15% (usando 128 vregs para melhor scheduling do MC/IDCT).

### Riscos
- **VMX128 no ffmpeg upstream: NULO.** Otimizações são só AltiVec (32-reg subset). Ganho requer patches locais nossos.
- **GCC 9.2 AltiVec BE**: OK (mainstream). Flag `-DWORDS_BIGENDIAN` obrigatória senão bytestream reader quebra.
- **Endian bug ppc BE**: Debian bug #870048 (test failures ffmpeg BE) — não impacta decoder H.264 em runtime, é só PCM edge case.
- **Alignment**: libavcodec assume alinhamentos 16B em vários buffers; Xenon precisa `memalign(16, ...)` ou macros ffmpeg (`av_malloc`).
- **`__XENON__` define**: precisa checar 3-4 arquivos que assumem POSIX (`libavutil/time.c`, thread stubs). ppsspp-ffmpeg wiiu.sh já tem template dos stubs.

### Esforço: 8-14 dias-pessoa
- D1-2: setup cross-compile + primeira link (esbarrar em faltando `getopt`/`sysconf`/thread primitives → stubs)
- D3-5: link contra libxenon + primeiro decode 1 frame IDR (offline .h264 file)
- D6-8: pipeline 60fps + integração com renderer Xenos (P4 handoff)
- D9-14: profiling, buffer pool, otimizações locais VMX128 se necessário

---

## 2. XMPlayer/Ced2911 salvage — **2ª OPÇÃO (fastest to bring-up)**

### Evidência
- `LibXenonProject/xmplayer` (upstream oficial Ced2911) já compila e roda MPlayer + libavcodec dentro do libxenon. Makefile refere `$DEVKITXENON` (mesmo toolchain nosso).
- Fork ativo: `pinh3ad/XMPlayer`.
- User reports (GBAtemp/Digiex): "works great on 720p TV shows, plays some 1080p" — confirma ordem de magnitude 720p viável.

### Estratégia
1. Clone `LibXenonProject/xmplayer`.
2. Extrair só o subdiretório `mplayer/libavcodec/` + deps necessárias (`libavutil`, `libavformat` mínimo).
3. Descartar todo o resto (audio, subs, GUI, demuxers extras).
4. Recompilar libavcodec com nosso build system, plugar API direta (`avcodec_send_packet` / `avcodec_receive_frame`).

### Prós
- **Código com trilhas de guerra**: já resolveu stubs libxenon, alignment, endian, integração com Xenos surface.
- Menos "por que não builda" que rota 1.
- Versão ffmpeg é antiga (~2011-2013 era) mas H.264 CBP decoder é maduro desde ~2008.

### Contras
- Código bundled é **antigo** (bugs de segurança irrelevantes p/ homebrew, mas API pré-refactor 2015).
- Último release oficial 2013 → dependência abandonware.
- Pode conter hacks estranhos difícil de destrinchar.
- Licença GPL contamina se distribuir binários (OK pra homebrew, ruim se quiser fechar código depois).

### Esforço: 5-10 dias-pessoa
- D1-2: clone, dependency archaeology
- D3-5: extração mínima + rebuild com xenon-gcc 9.2
- D6-10: integração + perf tuning

---

## 3. OpenH264 (Cisco) — **3ª OPÇÃO (BSD, mas risky)**

### Evidência
- **BSD-2** — licença ideal (limpa, sem GPL contamination).
- CBP até Level 5.2 nativo (nosso stream é 3.1 = perfeito).
- **PR #3806 (2024)** — adicionou PowerPC no meson.build. **`-Werror` removido** justamente por warnings PPC → confirma que port PPC nunca foi principal.
- Assembly-heavy no Intel (asm/x86/*) e ARM (asm/arm/*) — sem SIMD nativo pra PowerPC. Roda-se **ANSI-C fallback** no Xenon.

### Prós
- Baseline-only variant existe (footprint menor que ffmpeg).
- Licença BSD.
- API mais simples que libavcodec.
- Ativamente mantido pelo Cisco (embora Meta cortou financiamento MPEG-LA em 2024).

### Contras — **RISCOS ALTOS**
- **BE não validado**: PR PowerPC assume LE ppc64le. Não achei confirmação de rodar em BE ppc32.
- **Sem SIMD PPC**: fica ANSI C puro → 15-25fps 720p60 no Xenon. **INVIÁVEL sem escrever kernels VMX128 do zero.**
- meson.build (não autoconf) → mais fricção com xenon-gcc que ainda tem workflow autoconf-ish.
- Cisco removeu binaries hospedados (2024). Só source-only.

### Veredicto
Boa 2ª opção *se* fosse LE. No Xenon BE + sem SIMD do zero, **não vale**.

### Esforço: 12-20 dias-pessoa (com riscos)

---

## 4. edge264 — **REJEITADO**

### Motivo
> "edge264 can choose among 4 backends, **the last one supporting every other little-endian CPU** by relying on Clang vector extensions."

Tabela de compilers: só Clang faz "Other ISAs", GCC só Intel+ARM+NEON. Xenon = GCC-only (xenon-gcc), BE. Nenhum backend cobre.

Autor confirmou em FOSDEM'24-'26 que BE portability não é priority. Fork `jens-duttke/edge264-mvc` idem (LE-only).

**Não usar.**

---

## 5. Baseline from-scratch — **FALLBACK EXTREMO**

### Referências
- `abhiksark/python-h264-scratch` — Python readable spec-compliant (usar como referência).
- `tzwenn/pyh264decode` — outro Python decoder.
- ITU-T H.264 spec 03/2010 = ~800 páginas mas CBP subset é ~200.
- Livro: "The H.264 Advanced Video Compression Standard" (Iain Richardson).

### Realidade
- CBP puro (CAVLC + I/P slices, 0 B-frames, sem interlace) = **~3000-5000 LoC C** honestos, não 1500.
- Kernels críticos: 4x4 IDCT, MC luma (1/4-pel) + chroma (1/8-pel), CAVLC entropy, intra prediction 4x4/16x16, deblocking filter.
- SIMD custom VMX128 obrigatório pra bater 60fps → +1000 LoC inline asm.
- Escrever + validar bit-exact contra ffmpeg reference: **30-60 dias**.

### Quando faz sentido
- Rota 1 falha catastrofica em performance E rota 5 GPU-assist já rejeitada.
- Reserva estratégica, não plano primário.

---

## Xenos GPU assist — **BOOST OPCIONAL, não substituto**

### Precedent
**Baeza et al. 2007 (SPIE 6696)**: *"Real-time high definition H.264 video decode using the Xbox 360 GPU"* — Microsoft Research paper documenta pipeline software+GPU pra 1080p H.264 no console retail.

- Autores: Juan Carlos Arevalo Baeza, William Chen, Eric Christoffersen, Daniel Dinu, Barry Friemel.
- Papel prova viabilidade: **CPU faz CAVLC + intra prediction + deblocking**, **GPU faz IDCT + MC + color convert**.
- Código NÃO é aberto (research paper Microsoft interno).

### Aplicabilidade nossa
- Já temos shader YUV→RGB (P4, P7 planejado) rodando 60fps.
- **IDCT via shader**: cada 4x4 block é `float4 * matriz 4x4` — 3600 blocks/frame @ 60fps = 216k invocações, trivial pro Xenos (48 ALUs unificados, 500MHz).
- **MC via shader**: sampling `1/4-pel` = interpolação texture com custom filter kernel — Xenos TMU faz.
- **Deblocking via shader**: mais chato (dependencies inter-MB), mas literaturas VP9/AV1 usam GPU deblock.

### Risco
- **GPU-CPU sync**: cada frame precisa upload MC data + coefficients, download reconstructed frame. Xenos↔UMA (unified memory) ajuda vs discrete GPU.
- **Complexidade**: dobra o esforço da rota escolhida.
- **PoC first**: só considerar se rota 1 sozinha ficar <45fps sustentado.

### Rating: reserva P8 (só se P7 provar necessário)

---

## Big-endian gotchas conhecidos

1. **ffmpeg bytestream**: usa macros `AV_RB32`/`AV_RL32` — endian-safe. OK.
2. **openh264**: BE não testado. Estruturas internas usam bitfields → risco de layout diferente entre BE/LE compilers.
3. **VMX128 vector endianness**: XenosRecomp confirma "reverse entire 16-byte vector" convention. Kernels ffmpeg AltiVec assumem BE por default (foram escritos era G4/G5). **BOM PRA NÓS.**
4. **Debian #870048**: ffmpeg PCM tests falham BE. Não afeta H.264.
5. **Alignment de VMX loads (`lvx`)**: aborta se addr não 16B-aligned. `av_malloc` já garante.

## VMX128 vs AltiVec — checklist compilador

- **xenon-gcc 9.2**: baseado GCC 4.7 patched (Free60/libxenon toolchain). Suporta `-maltivec` completo. VMX128 extras (128-reg extension, custom insns como `vdupw`, packed formats para gaming) **NÃO** suportados diretamente — inline asm apenas.
- **Binutils VMX128 patch** (Sebastian Biallas 2007): permite montar VMX128 mnemonics via `.long 0x...` ou inline asm. Nunca mergeou upstream.
- **`hedge-dev/XenonRecomp`**: emula VMX128 no PC — não relevante pra nós, mas o repo mapeia todas instruções, útil de referência.
- **`hayleyxyz/PPC-Altivec-IDA`**: plugin IDA que decodifica VMX128 — útil pra ler disasm de código Microsoft.

**Conclusão**: usar AltiVec base upstream ffmpeg. Se precisar squeeze extra 10-15% perf, escrever handful de kernels custom (`h264_idct_add`, `h264_luma_mc_qpel`) em asm inline VMX128 usando 96 registers extra.

---

## Decisão proposta

1. **Rota 1** (FFmpeg build próprio) como caminho principal. Config baseada em `wiiu.sh`.
2. **Rota 2** (XMPlayer salvage) como **fonte de referência**: quando Rota 1 travar em stub/link, olhar como Ced2911 resolveu.
3. **P7 = GPU-assist** só se Rota 1 ficar abaixo 45fps sustentado após otimização.
4. **NÃO edge264, NÃO openh264, NÃO from-scratch.**

---

## Fontes verificadas (URLs check-out 2026-09-07)

### Código PowerPC/Xenon relevante
- https://github.com/hrydgard/ppsspp-ffmpeg/blob/master/wiiu.sh — template exato cross-compile PPC BE
- https://github.com/GaryOderNichts/FFmpeg-wiiu — port completo ffmpeg pro Wii U (Espresso PPC 750CL BE)
- https://github.com/LibXenonProject/xmplayer — MPlayer/libavcodec já rodou em RGH via libxenon
- https://github.com/pinh3ad/XMPlayer — fork alternativo XMPlayer
- https://github.com/Wolf3s/ffmpeg-xbox — ffmpeg XBMC-360 (X/SDK, não libxenon)
- https://github.com/brentdc-nz/XBMC-360 — XBMC-360 (Microsoft XDK, referência)
- https://github.com/Free60Project/libxenon — toolchain oficial libxenon (xenon-gcc)
- https://github.com/cisco/openh264 — OpenH264 (BSD-2, PR #3806 PPC support)
- https://github.com/tvlabs/edge264 — REJEITADO (LE-only)
- https://github.com/jens-duttke/edge264-mvc — fork edge264 (idem LE-only)

### Referências técnicas
- https://www.spiedigitallibrary.org/conference-proceedings-of-spie/6696/1/Real-time-high-definition-H264-video-decode-using-the-Xbox/10.1117/12.740208.short — Baeza 2007 Xbox 360 GPU H.264 paper
- https://www.ffmpeg.org/doxygen/0.5/h264__altivec_8c.html — libavcodec/ppc/h264_altivec.c reference
- https://ffmpeg.org//doxygen/0.10/h264__template__altivec_8c.html — MC template AltiVec
- https://github.com/hedge-dev/XenonRecomp — VMX128 instruction mapping (endian handling detail)
- https://github.com/hayleyxyz/PPC-Altivec-IDA — VMX128 IDA plugin
- https://gcc.gnu.org/legacy-ml/gcc/2007-08/msg00348.html — GCC VMX128 status (never merged)
- https://binutils.sourceware.narkive.com/uWGmo3nF/patch-support-vmx128-instruction-for-powerpc — Biallas VMX128 binutils patch
- https://github.com/NationalSecurityAgency/ghidra/issues/2094 — Ghidra VMX128 support request (dá overview clean)

### Community reports
- https://gbatemp.net/threads/best-media-player-for-360.328929/ — XMPlayer user reports 720p H.264
- https://digiex.net/threads/xmplayer-mplayer-port-for-xbox-360-plays-hd-mkv-jtag-rgh.10978/ — XMPlayer release notes
- https://news.ycombinator.com/item?id=45443462 — edge264 HN thread (comentários sobre portability)

### Big-endian / gotchas
- https://bugs.debian.org/870048 — ffmpeg BE test failures (PCM edge case, não afeta H.264)
- https://en.wikipedia.org/wiki/AltiVec — AltiVec vs VMX128 comparison

---

## Next actions (dependencies para P6-execução)

- [ ] Sergio aprova Rota 1
- [ ] Sub-P6.1: setup ffmpeg source tree em `third_party/ffmpeg/` (checkout N4.x branch — última stable com AltiVec H.264 antes de refactors 2020)
- [ ] Sub-P6.2: escrever `scripts/build-ffmpeg-xenon.sh` baseado em wiiu.sh
- [ ] Sub-P6.3: primeiro link stub (esperando faltar 20+ POSIX symbols)
- [ ] Sub-P6.4: decode 1 IDR frame do golden stream offline → PPM
- [ ] Sub-P6.5: pipe 60fps stress test
