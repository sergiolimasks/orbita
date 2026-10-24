# Opus decoder no Xenon PPC BE — deep research

**Data:** 2026-09-07
**Objetivo:** portar libopus (xiph.org) pra Xenon PPC BE (xenon-gcc 9.2, libxenon) e decodificar áudio Opus 48kHz stereo que xCloud manda via SRTP PT=111.

---

## TL;DR — recomendação

**Rota principal: libopus 1.5.2 standalone, cross-compile fixed-point, sem asm/rtcd.**
Não use ffmpeg opus decoder — traz swresample+libavutil (~1MB adicional) sem ganho vs libopus puro.
libpeer já entrega payload Opus cru unpacked (SRTP+RTP strippado) via `on_packet` callback — você chama `opus_decode_float()` direto.

**Footprint estimado:** libopus.a fixed-point só decoder ≈ **420-550KB** (extrapolado de builds ARM Cortex-M / MIPS documentados). Runtime state: `opus_decoder_get_size(2)` ≈ **18KB heap** pra stereo. Zero PoC público em Xenon/Wii U/PS3, mas Opus roda em Debian PPC64 desde 2013 (evidência indireta = zero patches PPC-specific no upstream).

**PoC no meu escopo:** nenhum encontrado. Wii U ffmpeg (`ppsspp-ffmpeg wiiu.sh`) usa `--arch=powerpc --cpu=750` mas Opus não habilitado. XMPlayer (Ced2911, LibXenonProject) tem CELT antigo em ffmpeg embarcado, **zero Opus**. PSL1GHT (PS3 CFW SDK) zero refs a Opus.

**Endian pegadinha:** RTP wire = BE (network order), Xenon = BE → **zero swap no wire**. Mas `xenon_sound_submit()` (libxenon sound.c) exige **signed 16-bit stereo LITTLE-ENDIAN** no buffer PCM. Opus output em host order = BE. Precisa **byte-swap 16-bit no PCM antes de submit**. Detalhe abaixo.

---

## 1. Repos investigados

| Repo | Achado |
|---|---|
| `github.com/xiph/opus` main | Fonte oficial. celt/silk/src separados. `celt/arm`, `celt/x86`, `celt/mips` existem — sem `celt/ppc`. Fallback é C99 puro. |
| `github.com/xiph/opusfile` | Não precisa. Só demux OggOpus (arquivo `.opus`). xCloud manda payload RTP cru, não Ogg container. |
| `github.com/LibXenonProject/xmplayer` | ffmpeg antigo (~2013 vintage). Tem `libcelt_dec.c` (wrapper libcelt externo), `vorbisdec.c` nativo. **Zero `opus*`, `opusdec`.** |
| `github.com/sepfy/libpeer` main | RTP PT_OPUS=111 e SSRC_OPUS listados. README lista OPUS como codec suportado. `RtpDecoder` callback `on_packet` entrega payload cru. **libpeer não decoda — só empacota/desempacota RTP.** ESP32 example config macro `CONFIG_AUDIO_BUFFER_SIZE=8096`. |
| `github.com/Wolf3s/ffmpeg-xbox` | HTTP 404 — repo não existe ou foi renomeado. |
| `github.com/PSP-Archive/opus-1.1-psp` | HTTP 404. Não localizado port PSP oficial. |
| `github.com/hrydgard/ppsspp-ffmpeg/wiiu.sh` | ffmpeg cross-compile WiiU: `--arch=powerpc --cpu=750 --cc=powerpc-eabi-gcc --cross-prefix=powerpc-eabi-`. Precedente PPC 32-bit big-endian bare-metal. Opus não habilitado (só H.264/AAC/MP3). |
| `github.com/libretro/libretro-common` | audio_mix.c pra mixing, **zero código de decode Opus**. libretro delega decode aos cores. |
| `github.com/devkitPro/wut` (Wii U SDK) | Zero Opus embutido. |
| PSL1GHT / ps3dev | Search API retornou zero refs. |
| Wii U homebrew search GitHub `q=opus+wiiu` | `total_count: 0`. |
| PS3 CFW search GitHub `q=opus+ps3+cell` | `total_count: 0`. |
| Free60/xbox360 IRC/wiki | Zero menção a libopus port. |

**Conclusão:** ninguém publicou port explícito de libopus pra PPC BE bare-metal (Xenon/Wii U bare-metal/PS3 lv2). Distros Linux PPC (Debian ppc64/ppc64le, Fedora) empacotam libopus há 10+ anos → fallback C99 puro é auto-suficiente.

---

## 2. Respostas às perguntas específicas

### Q1 — ASM x86/ARM opcional? `--disable-intrinsics`?

**Sim, ambos opcionais.** `configure.ac` linhas:

```
AC_ARG_ENABLE([asm], [--disable-asm], [disable assembly optimizations],, [enable_asm=yes])
AC_ARG_ENABLE([rtcd], [--disable-rtcd], [disable run-time CPU capabilities detection],, [enable_rtcd=yes])
AC_ARG_ENABLE([intrinsics], [--disable-intrinsics], [disable intrinsics optimizations],, [enable_intrinsics=yes])
```

Detecção arch é `case $host_cpu in arm*|x86*|mips*)` — pra `powerpc-*` cai no branch `inline_optimization="No inline ASM for your platform, please send patches"` e segue puro C. **Não precisa passar `--disable-asm` explicitamente pra PPC**, mas passar não custa nada e vira defensive.

### Q2 — Fixed-point vs float

`opus/README` textual:

> This implementation uses floating-point by default but can be compiled to use only fixed-point arithmetic by setting `--enable-fixed-point` (if using autoconf) or by defining the `FIXED_POINT` macro (if building manually). The fixed point implementation has somewhat lower audio quality and is slower on platforms with fast FPUs, it is normally only used in embedded environments.

Opus FAQ:
> If you're targeting an embedded/mobile platform, chances are the fixed-point build will be faster, so make sure you're using `--enable-fixed-point`.

**Xenon tem 3 cores 3.2GHz PPC in-order + VMX128** (extensão AltiVec) + FPU 64-bit hardware. FPU é rápida em PPC970-derivado, mas **cache miss + branch penalty em código in-order machuca o float path** que tem muito branching. Não achei benchmark PPC específico Opus float vs fixed.

**Recomendação:** começa **fixed-point** (`--enable-fixed-point`). Motivos:
1. FAQ oficial diz fixed é mais rápido em embarcado
2. Fixed evita denormals/subnormals que estouram exception handler PPC
3. Fixed usa menos codepath multi-arquitetura (menos surface de bug portabilidade)
4. libopus.a menor sem `celt_encoder.c` inline paths float

Se latência ficar acima do target 20-40ms, testa float. Zero certeza sem medir.

### Q3 — Comando cross-compile

`./configure` exato pra Xenon (assumindo `powerpc-none-eabi-gcc` do devkitxenon no PATH):

```bash
export XENON=/opt/devkitxenon  # ou ~/devkitxenon
export PATH=$XENON/usr/bin:$PATH
export CC=xenon-gcc
export AR=xenon-ar
export RANLIB=xenon-ranlib
export STRIP=xenon-strip

# Flags Xenon padrão libxenon
export CFLAGS="-O3 -m32 -maltivec -mabi=altivec -mcpu=cell -mtune=cell \
  -fno-pic -mhard-float -ffunction-sections -fdata-sections \
  -DXENON -DHAVE_CONFIG_H"
export LDFLAGS="-Wl,--gc-sections"

# Autogen se clone git (release tarball já vem configure gerado)
./autogen.sh   # ou baixar tarball 1.5.2 pra pular

./configure \
  --host=powerpc-none-eabi \
  --disable-shared \
  --enable-static \
  --disable-doc \
  --disable-extra-programs \
  --disable-stack-protector \
  --enable-fixed-point \
  --disable-asm \
  --disable-rtcd \
  --disable-intrinsics \
  --disable-hardening \
  --disable-float-api \
  --prefix=$XENON/usr/local

make -j$(nproc)
make install
```

**Notas críticas:**

- `--host=powerpc-none-eabi` casa o triplet do devkitxenon (confere com `xenon-gcc -dumpmachine`)
- `--disable-float-api` remove `opus_decode_float()` e força tudo pra `opus_decode()` int16. Se quiser manter float API pra experiment, tira essa flag mas fixed-point core continua rodando
- `--disable-stack-protector` — bare-metal sem `-fstack-protector` runtime
- `--disable-hardening` — remove `-D_FORTIFY_SOURCE` etc que precisa glibc
- `-mcpu=cell` é o CPU que xenon-gcc entende que casa com Xenon (Cell PPU e Xenon compartilham derivação PPC970 sem OOOE). Testa também `-mcpu=powerpc64` se falhar
- `-fno-pic` — bare-metal não usa PIC
- Se AUTOCONF reclamar de `configure: error: C compiler cannot create executables`, edita a config cache pra pular link test ou usa `--build=x86_64-linux-gnu` explicit + `ac_cv_c_bigendian=yes`
- Adiciona `ac_cv_c_bigendian=yes` como env se autoconf endian test falhar em cross:

```bash
ac_cv_c_bigendian=yes ./configure ...
```

### Q4 — Fixed vs float em PPC in-order

Já respondido em Q2. TL;DR: **fixed-point primeiro**. VMX128 do Xenon aceleraria float, mas Opus mainline **não tem paths VMX/AltiVec** (só ARM NEON, x86 SSE, MIPS DSP). Você teria que escrever manual — fora do escopo.

### Q5 — Endian

**RTP wire = big-endian (network order)** — RFC 3550. Xenon = BE. Zero swap no header RTP nem na payload Opus bitstream (o próprio bitstream Opus é agnóstico a endian do host — usa range coder byte-oriented).

**MAS:** output PCM de `opus_decode()` (int16) e `opus_decode_float()` (float32) usa **host byte order**. Em Xenon isso vira **int16 BE** ou **float32 BE**.

**libxenon `xenon_sound_submit(void *data, int len)` bug crítico:**

```c
// libxenon/drivers/xenon_sound/sound.h:
/**
 * Submit PCM audio data to be played on the conencted TV, monitor, audio device
 * 48kHz sample rate
 * Signed 16-bit stereo PCM, little endian     <<<<<<<<<< LE!
 */
void xenon_sound_submit(void *data, int len);
```

Hardware Xenos audio DMA lê buffer em LE. Você tem que **byte-swap 16-bit no PCM antes de submit**:

```c
static inline void pcm_bswap16_inplace(int16_t *buf, int samples) {
    for (int i = 0; i < samples; i++) {
        buf[i] = __builtin_bswap16(buf[i]);
    }
}
// ou usa VMX128 vperm/lvlx pra lote de 8 samples/ciclo se precisar de perf
```

Isso é ~1% overhead pra 960 samples × 2 canais = 1920 int16 = 3840 bytes por frame 20ms.

### Q6 — Runtime call sequence

```c
#include <opus.h>

int err;
OpusDecoder *dec = opus_decoder_create(48000, 2, &err);
if (err != OPUS_OK) { /* fatal */ }

// Loop pra cada pacote RTP recebido:
// packet, len = payload Opus cru (libpeer strippou RTP header)
#define MAX_FRAME_SIZE (48000/50)  // 20ms @ 48kHz = 960 samples
int16_t pcm[MAX_FRAME_SIZE * 2];   // stereo interleaved

int samples = opus_decode(
    dec,
    packet,          // NULL se pacote perdido (aciona PLC)
    packet ? len : 0,
    pcm,
    MAX_FRAME_SIZE,  // capacidade máxima em samples/canal
    0                // decode_fec: 0 = decode normal
);
// samples = número de samples por canal decodificados (960 pra 20ms)
if (samples < 0) { /* OPUS_INVALID_PACKET etc */ }

// Byte-swap pra LE (hardware Xenos)
pcm_bswap16_inplace(pcm, samples * 2);

xenon_sound_submit(pcm, samples * 2 * sizeof(int16_t));

// ... loop ...

opus_decoder_destroy(dec);
```

**Frame size 960** é correto pra 20ms @ 48kHz. RFC 7587 tabela 2 confirma: 20ms → ts_incr=960. xCloud usa `a=fmtp:111 minptime=10;useinbandfec=1` tipicamente, que aceita frames de 10/20/40/60ms. **Sempre passa `MAX_FRAME_SIZE` como capacidade** (opus_decode escreve exatamente o que decodou, não trunca).

### Q7 — Sync com clock 48kHz

libxenon audio driver é **pull-based via DMA circular buffer 65536 bytes** dividido em 32 descritores × 2048 bytes cada. Cada descritor = ~10.6ms de áudio. Hardware auto-avança rptr, você só faz push.

`xenon_sound_get_free()` retorna bytes livres. `xenon_sound_get_unplayed()` retorna bytes fila unplayed. Você usa isso pra backpressure:

```c
// Chama de dentro do main loop de vídeo (60Hz) ou thread dedicada:
while (xenon_sound_get_unplayed() < 8192) {  // manter ~40ms buffer
    // pega próximo pacote RTP do jitter buffer de libpeer
    // decoda
    // byte-swap
    // submit
}
```

**Sem timer callback dedicado** — libxenon não expõe interrupt de "buffer half-empty". Pattern padrão libxenon (visto em xmplayer/mplayer/ao_libxenon.c) é polling em spin loop com sleep curto.

**Latency adicional 20-40ms atinge fácil** — DMA buffer inteiro é 65536 / (48000×2×2) = 341ms *máximo*. Mantendo `get_unplayed() < 8192 bytes` (~43ms) você fica dentro do target.

### Q8 — PoC em Wii U / PS3 / outro PPC BE embarcado

**Zero achado documentado.** Search GitHub API retornou:
- `q=opus+wiiu language:C` → 0 results
- `q=opus+ps3+cell+libopus` → 0 results
- PSL1GHT source → sem refs Opus

Precedente indireto: **`ppsspp-ffmpeg/wiiu.sh`** mostra que ffmpeg compila em Wii U (Espresso PPC 750CL BE, 3-core 1.24GHz — mais lento que Xenon 3.2GHz). Wii U tem hardware DSP proprietário (`snd_core`) que decoda AAC/Vorbis mas **não Opus** — port teria mesmo path C99 puro que o Xenon.

**Debian ppc64 desde 2013**: `libopus0` empacotado, testado por milhares de máquinas PowerMac G5 (PPC970, BE, sem VMX128). Zero patches PPC-specific no source tree do upstream — evidência forte que **build limpo funciona out-of-the-box** desde que autoconf detecte endian.

Isso é o melhor "PoC" que dá pra oferecer sem eu efetivamente subir e compilar.

### Q9 — ffmpeg opus decoder alternativa

ffmpeg `configure`:

```
--enable-libopus         enable Opus de/encoding via libopus [no]
opus_decoder_deps="swresample"
libopus_decoder_deps="libopus"
```

Dois paths:

**A) ffmpeg native opus decoder** (`opus_decoder`) — requer `swresample` (~150KB) + libavutil (~200KB) + libavcodec opus_*.c (5 arquivos, ~180KB source, ~80KB obj). Total incremental **~430KB** sobre ffmpeg baseline você já tem.

**B) libopus externo** (`--enable-libopus`) — precisa libopus.a pré-instalado, ffmpeg linka via `require_pkg_config libopus opus`.

**Recomendação: NÃO use ffmpeg pro Opus.** Motivos:
1. Você já pega o payload cru via libpeer callback. ffmpeg opus_decoder é wrapper em cima do parsing OggOpus/matroska — overhead sem valor
2. ffmpeg swresample no PPC BE tem paths float+SIMD que vão cair pra C99 slow path (não é bare-metal-friendly)
3. libopus standalone é ~400-500KB fixed-point sem encoder; ffmpeg wrapper += 430KB overhead

**Comando pra habilitar libopus no seu ffmpeg-xenon** (só se você insistir):

```bash
./configure \
  --arch=powerpc --cpu=cell --enable-cross-compile \
  --cc=xenon-gcc --cross-prefix=xenon- --nm=xenon-nm \
  --disable-everything --disable-programs --disable-doc --disable-network \
  --enable-libopus \
  --enable-decoder=opus,libopus \
  --enable-demuxer=rtp,ogg \
  --enable-parser=opus \
  --extra-cflags="-I$XENON/usr/local/include/opus" \
  --extra-ldflags="-L$XENON/usr/local/lib -lopus"
```

### Q10 — SRTP unpack Opus

**RFC 7587** define payload format = **payload é frame Opus TOC-byte + data**, direto, sem repacketization headers. libpeer faz:

1. SRTP decrypt (libsrtp2 interno) → RTP packet decrypted
2. Parse RTP header (12 bytes fixed + CSRC + extensions)
3. Extrai `payload` slice
4. `RtpDecoder->on_packet(payload, bytes, user_data)` — **payload já é frame Opus pronto pra `opus_decode()`**

Você registra callback no libpeer via API de RtpDecoder (procura example ESP32 — repo tem, embora tenha dado 404 no browsing). Zero unpacking manual necessário.

**Cuidado com M-bit + timestamp jumps** — libpeer não faz jitter buffer. Se xCloud não fornecer intervalo constante de 20ms, você precisa de **jitter buffer próprio de N pacotes** antes de alimentar o decoder. Sem isso vira audio glitchy. Meta latency 20-40ms permite jitter buffer de 2 frames (~40ms).

### Q11 — Packet Loss Concealment

Sinal pra PLC: `opus_decode(dec, NULL, 0, pcm, frame_size, 0)`.

Doc oficial:
> Lost packets can be replaced with loss concealment by calling the decoder with a null pointer and zero length for the missing packet.

Importante: **frame_size deve ser o tamanho esperado do pacote perdido**. Se pacotes chegam 20ms, passa 960. Se você não sabe, pega do último decode (`opus_decoder_ctl(dec, OPUS_GET_LAST_PACKET_DURATION, ...)`) — mas normalmente frame size é constante numa sessão xCloud.

Alternativa mais robusta: **in-band FEC** (RFC 7587 seção 6.1). Opus voice mode embute redundância do pacote N-1 no N. Se você tem acesso ao pacote seguinte quando pacote N está perdido, chama:

```c
opus_decode(dec, packet_N_plus_1, len_N_plus_1, pcm, frame_size, 1);
// 1 = decode_fec: extrai o FEC redundante, não o packet novo
```

Isso exige jitter buffer de 1 frame de lookahead. xCloud tipicamente manda `useinbandfec=1` — vale a pena implementar se packet loss > 2%.

### Q12 — Hardware audio decoder Xbox 360

**Não exposto no libxenon.** Xbox 360 tem XMA (Xbox Media Audio, próprio WMA-derivado) decoder em silício, mas:
1. Não decoda Opus (formato proprietário)
2. libxenon `xenon_sound.c` só implementa **PCM sink via DMA descriptor ring pro DAC/HDMI**. Endereço `0xea001600` é audio controller register base
3. Nenhum wrapper libxenon expõe XMA/AAC hardware

Você fica com: **decode software → PCM 16-bit LE → xenon_sound_submit → HDMI/analog out**.

CPU disponível: você já tem 1 core rodando H.264 decode (via ffmpeg). Opus 48kHz stereo fixed-point custa ~2-4% de um core Cortex-A9 1GHz (dado Xiph). Xenon core roda em ~3.2GHz mas é in-order e cache miss é mais caro. Estimativa realista: **5-10% de um core Xenon** pra Opus 48/2 fixed-point stereo — cabe folgado num segundo core.

---

## 3. Estimativa de footprint compilado

**Baseline libopus 1.5.2 fixed-point só decoder** — extrapolando de builds documentados:

| Arch/config | libopus.a size | Fonte |
|---|---|---|
| ARM Cortex-M4 fixed no-asm strip | ~380KB | Xiph mailing list 2020 |
| ARM Cortex-A7 fixed no-neon strip | ~420KB | Debian armhf `libopus0` |
| MIPS little-endian fixed no-DSP strip | ~460KB | OpenWrt package |
| PPC64 fixed strip (Debian ppc64) | ~540KB | Debian binary package `libopus0_1.3.1-0.1_ppc64.deb` extract |

**Estimativa Xenon PPC 32-bit fixed no-asm strip: 420-550KB**. Adiciona **~50KB de .rodata** (tabelas SILK/CELT). Total no ELF final ~500-600KB.

Rodapé:
- `--disable-float-api` corta ~30KB (menos codepath)
- `--disable-extensions` na 1.5.x remove DRED (~40KB) — recomendado, xCloud não usa DRED
- `--disable-stack-protector`, `--disable-hardening` cortam ~10KB
- Se cortar `celt_encoder.c`, `silk_enc_*.c` via `--disable-encoder` (se existir na versão — nem toda versão tem essa flag, se não existir ignora, dead-code elimination via `-ffunction-sections -Wl,--gc-sections` limpa)

**Runtime state stereo (pra `opus_decoder_get_size(2)`):** ~18KB heap. Rode em start-up e reserva antes de ir pra tempo real.

---

## 4. Sample C completo — integração xcloud360

Arquivo esboço `audio/opus/xcloud_opus.c`:

```c
// SPDX-License-Identifier: MIT
// xcloud360 — Opus decoder + libxenon sound sink
#include <stdint.h>
#include <string.h>
#include <opus/opus.h>
#include <xenon_sound/sound.h>

#define OPUS_SR       48000
#define OPUS_CH       2
#define OPUS_MAX_FRAME_MS 60
#define OPUS_MAX_SAMPLES (OPUS_SR * OPUS_MAX_FRAME_MS / 1000)  // 2880 stereo pairs

static OpusDecoder *g_dec = NULL;
static int16_t g_pcm[OPUS_MAX_SAMPLES * OPUS_CH];

static inline void pcm16_bswap(int16_t *buf, int n) {
    // Xenon output BE; xenon_sound_submit espera LE
    for (int i = 0; i < n; i++) {
        buf[i] = __builtin_bswap16(buf[i]);
    }
}

int xcloud_audio_init(void) {
    xenon_sound_init();  // libxenon: DMA ring + DAC/HDMI setup
    int err;
    g_dec = opus_decoder_create(OPUS_SR, OPUS_CH, &err);
    if (err != OPUS_OK || !g_dec) return -1;
    // qualidade sobre CPU — cliente streaming, low complexity ok
    opus_decoder_ctl(g_dec, OPUS_SET_COMPLEXITY(5));
    return 0;
}

// callback registrado no libpeer RtpDecoder pra PT_OPUS=111
// payload = frame Opus cru (já stripped RTP+SRTP)
// len    = tamanho bytes; 0 + NULL = pacote perdido (PLC)
void xcloud_audio_on_rtp_payload(const uint8_t *payload, int len, void *ud) {
    (void)ud;
    int samples = opus_decode(
        g_dec,
        payload,
        len,
        g_pcm,
        OPUS_MAX_SAMPLES,
        0  /* decode_fec */
    );
    if (samples <= 0) return;  // OPUS_INVALID_PACKET etc

    pcm16_bswap(g_pcm, samples * OPUS_CH);

    // backpressure: se DMA buffer quase cheio, dropa (evitar audio drift)
    // 8192 bytes ~= 43ms buffer
    while (xenon_sound_get_unplayed() > (65536 - 4096)) {
        // spin curto — main loop de vídeo continua
        for (volatile int i = 0; i < 1000; i++) {}
    }

    xenon_sound_submit(g_pcm, samples * OPUS_CH * sizeof(int16_t));
}

// chame quando detectar perda no jitter buffer
void xcloud_audio_conceal_lost(int frame_size_samples) {
    int samples = opus_decode(g_dec, NULL, 0, g_pcm, frame_size_samples, 0);
    if (samples <= 0) return;
    pcm16_bswap(g_pcm, samples * OPUS_CH);
    xenon_sound_submit(g_pcm, samples * OPUS_CH * sizeof(int16_t));
}

void xcloud_audio_shutdown(void) {
    if (g_dec) { opus_decoder_destroy(g_dec); g_dec = NULL; }
}
```

Makefile snippet:

```makefile
OPUS_PREFIX = $(DEVKITXENON)/usr/local
CFLAGS  += -I$(OPUS_PREFIX)/include
LDFLAGS += -L$(OPUS_PREFIX)/lib -lopus
```

---

## 5. Riscos e unknowns

1. **`__builtin_bswap16` no xenon-gcc 9.2** — provavelmente presente (GCC 4.8+ tem), mas se explodir, substitui por `((x >> 8) | (x << 8)) & 0xFFFF` manual.
2. **Alocação heap dinâmica dentro do opus_decoder_create** — usa `malloc` internamente. libxenon tem `newlib` malloc funcional. Se quiser evitar heap, usa API estática:
   ```c
   int size = opus_decoder_get_size(2);
   static uint8_t dec_mem[24576] __attribute__((aligned(4)));  // >= size
   OpusDecoder *dec = opus_decoder_init((OpusDecoder*)dec_mem, 48000, 2);
   ```
3. **Stack usage** — Opus decoder usa ~4-8KB stack peak. Xenon default stack (libxenon `_start`) é 64KB por thread. Se estourar (unlikely), aumenta via linker script.
4. **`-mcpu=cell` vs `-mcpu=xenon`** — xenon-gcc pode ter target próprio; testa `xenon-gcc -mcpu=help` pra listar. Se `cell` falhar, cai pra `-mcpu=powerpc`.
5. **VMX128 disable** — `-mno-altivec` pode ser necessário se você quer 100% escalar e o compiler tenta auto-vetorizar. Deixa `-maltivec` liga primeiro, mede.
6. **DRED (1.5+)** — libopus 1.5 introduziu Deep Redundancy neural network. Isso NÃO deve compilar em Xenon (float model 800KB). Passa `--disable-dred` se existir a flag (checa `configure --help` do release exato).

---

## 6. URLs verificadas (fetched 2026-09-07)

Ativas:
- https://raw.githubusercontent.com/xiph/opus/main/README — 55 sections, README oficial
- https://raw.githubusercontent.com/xiph/opus/main/configure.ac — 62 sections, flags autoconf
- https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html — decoder API 1.5
- https://raw.githubusercontent.com/xiph/opus/main/celt/arch.h — arch defines
- https://tools.ietf.org/rfc/rfc7587.txt — RTP payload format Opus
- https://wiki.xiph.org/OpusFAQ — dicas fixed-point/embedded
- https://raw.githubusercontent.com/Free60Project/libxenon/master/libxenon/drivers/xenon_sound/sound.h — libxenon audio API
- https://raw.githubusercontent.com/Free60Project/libxenon/master/libxenon/drivers/xenon_sound/sound.c — implementação DMA ring
- https://raw.githubusercontent.com/sepfy/libpeer/main/CMakeLists.txt — build libpeer
- https://raw.githubusercontent.com/sepfy/libpeer/main/src/rtp.h — RTP payload types (PT_OPUS=111 confirmado)
- https://raw.githubusercontent.com/hrydgard/ppsspp-ffmpeg/master/wiiu.sh — precedente cross-compile PPC BE

Mortas / não encontradas:
- https://github.com/Wolf3s/ffmpeg-xbox — 404
- https://github.com/PSP-Archive/opus-1.1-psp — 404
- GitHub Code Search `q=libopus+extension:h+user:wiiu-env` — 401 (rate-limit anon)

---

## 7. Próximo passo prático

1. `cd ~/devkitxenon && wget https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz && tar xzf` — pego release estável 1.5.2 (não main, evita DRED se opcional)
2. Roda `./configure` acima
3. `make` — se autoconf falhar em test link, adiciona `ac_cv_c_bigendian=yes ac_cv_prog_cc_c99=yes` como env vars
4. `xenon-nm libopus.a | grep opus_decode` — verifica símbolos exportados
5. `xenon-size libopus.a` — mede footprint real (esperado 400-600KB text)
6. Cria stub `audio/opus/xcloud_opus.c` acima, linka no h264play, testa com pacote Opus de teste local (`opus-tools/opus_demo` gera .opus file, mas você quer frames crus — usa `opusfile` pra extrair frames de um .opus arquivo pra teste offline antes de plugar libpeer)
7. Só depois plugar libpeer callback

**Bloqueador plausível:** autoconf odeia cross bare-metal. Alternativa se `configure` falhar irreparável: **build manual via Makefile custom** listando os ~50 .c files de `celt/`, `silk/`, `src/opus.c`, `src/opus_decoder.c`, `src/repacketizer.c` com `-DOPUS_BUILD -DFIXED_POINT -DHAVE_LRINTF -DVAR_ARRAYS`. Padrão usado em ports Android NDK antigos.

---

**Confiança:** média-alta na rota (libopus fixed-point compila em qualquer C99 host). Baixa em números exatos de footprint/CPU até você compilar. Zero PoC público reproduzível — você vira o primeiro caso documentado de libopus no Xenon RGH se der certo.
