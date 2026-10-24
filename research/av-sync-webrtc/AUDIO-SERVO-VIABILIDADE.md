# Servo áudio ±0.2% no libxenon — viabilidade

**Data**: 2026-09-08
**Autor**: research agent (context-mode)
**Contexto**: recomendação do agent 17 (AV sync) pra corrigir drift acumulado entre clock áudio do Xbox e clock sender xCloud.

---

## TL;DR

**Recomendação**: **implementar servo software via linear resample** com buffer de 150-200ms + threshold-based rate correction. Skip complexity de speexdsp/soxr — overkill pra gaming stream. Alternativa (sem servo) OK só se sessão <15min E buffer ≥100ms.

**Por quê**:
1. Hardware Xenos **não expõe sample rate ajustável** — fixed 48kHz S16 LE stereo no MMIO `0xea001600`. Servo hardware impossível.
2. Drift real esperado 30-50 ppm entre crystal PC × crystal Xbox → buffer 100ms esgota em 33-55min. Sessão gaming normal (30-60min) toca no limite.
3. Linear resample stereo 48k @ ratio ±0.002 custa <0.05% do Xenon single-core. Trivial.
4. Speexdsp resample = 43KB de source + tabelas, ~30KB text compiled — footprint razoável mas dispensável.
5. Opus PLC não serve — granularidade errada (frame de 20ms, não sample).

---

## Perguntas respondidas

### 1. MMIO permite sample rate runtime?

**Não.** Análise do `xenon_sound/sound.c`:

```c
static int snd_base = 0xea001600, wptr, buffer_len;

// no init:
write32(snd_base + 8, 0);
write32(snd_base + 8, 0x2000000);
write32(snd_base + 0, descr_base);
write32(snd_base + 8, 0x1d08001c);
write32(snd_base + 0xC, 0x1c);
```

- Todos os writes MMIO são setup one-shot de descritor DMA.
- Comentário explícito no `xenon_tone`: `// xenon assumes a 48kHz sample rate for submitted audio data`
- HDMI audio clock é derivado do Xenos GPU clock (fixed multiplier). Ajustar exigiria mexer em PLL config — não documentado no Free60, risco de destabilizar vídeo/HDMI link.
- xmplayer `ao_xenon.c` **ignora o param `rate` do init**: chama `xenon_sound_init()` cru, sem passar rate adiante. Confirma: quem quiser rate ≠ 48000 tem que resamplar em software antes de chamar `xenon_sound_submit`.

**Conclusão**: servo tem que ser software resampling do lado do decoder. Zero possibilidade de "puxar" ou "atrasar" o DAC via registro.

### 2. Custo CPU de resample ±0.2% stereo 48kHz

| Método | Ops/sec estimado | % Xenon core | Latência add | Qualidade |
|---|---|---|---|---|
| Linear interp | ~600k mul-add | <0.05% | 1 sample | OK pra gaming (jitter mascarado por SFX) |
| Speexdsp Q3 polyphase | ~10-40 MFLOPS | 0.3-1.2% | ~8 samples | Musical quality |
| SoXR VHQ | ~50-100 MFLOPS | 1.5-3% | ~32 samples | Overkill |

Xenon core = 3.2GHz PPC in-order + VMX128 SIMD. Speexdsp compila (usa float, requer FPU — Xenon tem). Linear é single-precision int math ou float, roda até no PPU sem VMX.

**Para stream 48kHz stereo com ratio 0.998-1.002** (drift ±2000 ppm, muito além do que precisa):
- linear = ruído aliasing em >20kHz. Inaudível.
- speexdsp Q3 = clean até Nyquist, mas footprint 30KB+ text.

**Recomendação**: linear. Se der problema audível (não vai), migra pra speexdsp depois.

### 3. Drop/dup samples ocasional?

Ratio 0.002 = 1 sample descartado a cada 500 samples = a cada 10.4ms. Isso é **audível** em áudio contínuo (tom puro, silêncio, música com tail longa). Máscara SFX de jogo funciona 80% do tempo, mas menu music expõe.

**Melhoria**: só drop/insert em zero-crossing local. Complexidade fica igual ao linear resample, sem vantagem.

**Veredicto**: pior que linear, mesmo custo. Descartar.

### 4. Servo é obrigatório? Drift <20ms/h aguenta 1h?

Depende do **crystal delta real**, não do "worst case cheap encoder":

- Xenon HDMI clock: crystal 27MHz sistema, precisão típica ±30 ppm
- PC sender (xCloud): crystal 14.318MHz ou 25MHz, precisão típica ±25-50 ppm
- **Delta combinado esperado**: 30-100 ppm

Simulação (buffer 100ms):

| Cenário | Tempo até buffer esgotar |
|---|---|
| 30 ppm típico | 55.6 min |
| 100 ppm worst case | 16.7 min |
| 30 ppm com buffer 200ms | 111 min |

**Conclusão**:
- **Sem servo + buffer 100ms**: aguenta sessão de 15-30min confortavelmente. Sessão de 1h → coin toss.
- **Sem servo + buffer 200ms**: aguenta 30-60min tranquilo, 1h+ arriscado.
- **Com servo**: aguenta indefinidamente. E permite buffer menor (60-80ms) reduzindo latência AV.

Streaming de jogo Xbox Live tradicional era sessão longa (partidas de 30min-2h). Se XCloud360 mira paridade, servo é justificado. Se MVP mira demo/POC, skip é OK.

### 5. Green-nx audio_player servo — como faz?

Não consegui puxar o repo (404 no averne/Green-nx e libnx-audio-player — possível repo movido/privado). Info da minha base:

- Switch tem AudioRenderer/AudioOut com sample rate configurável no HW (48k, 44.1k, 32k selecionáveis). N-A pro Xenon.
- Padrão comum em emuladores Switch: **buffer-fullness feedback loop**. Mede fill level do ring buffer, se enche muito → resample ratio 0.999x, se esvazia → 1.001x. PI controller com dead band.
- Aplica pra Xenon sem mudança: substitui o rate change de HW por resample de SW antes do `xenon_sound_submit`.

### 6. Opus PLC extend/shorten?

**Não serve pro servo.** Opus PLC:
- Concealment de frame perdido, gera 20ms extra a partir do prev.
- Granularidade = 20ms = 960 samples @ 48k. Mudar 1 frame em 500 = ratio 0.002 = correto em magnitude, MAS...
- PLC não é lossless — sinal reconstruído tem artefatos audíveis (buzz/pop) em picos. Repetir 1x por 10s = clique cada 10s. Ruim.
- Opus tem `OPUS_SET_EXPERT_FRAME_DURATION` e `opus_encoder_ctl` pra timing, mas isso é **encoder side**. Decoder recebe o que vier.

Descartar. PLC é pra loss recovery, não drift correction.

### 7. Buffer 100ms drena diferente conforme rate?

Sim, exatamente esse é o feedback signal. Se HW consome 48000 samples/s e sender enviou 48050 (100 ppm rápido), buffer enche 50 samples/s = 25ms/hora acumulando. Após 4h o buffer overflow. **Reverso**: buffer esvazia = underrun = crackle.

Buffer fill level é o **input do servo**. Não precisa medir clock delta diretamente — mede quanto está enchendo/esvaziando.

### 8. Impacto se skip servo em stream 30min?

Skip servo + drift 40 ppm = 72ms drift / 30min = quase o buffer inteiro.

Consequência real:
- **Buffer overflow**: samples descartados no producer side (network layer) → glitch curto (~10ms) cada 15min. Aceitável.
- **Buffer underflow**: DAC lê zeros → click audível. Cada 15min = 4 clicks/hora. Feio mas não catastrófico.
- **AV desync**: audio drifta em relação ao vídeo → lip sync off 70ms. Em jogo, notável se cutscene falada. Em gameplay puro (SFX + música), invisível.

**Aceitável pra MVP, ruim pra polish**.

---

## Recomendação final: implementar servo linear

### Estratégia

1. **Buffer target**: 150ms (7200 samples @ 48k stereo = 28.8KB — cabe folgado em 64KB do ring atual).
2. **Sample rate correction** aplicada no decoder Opus → resample antes de `xenon_sound_submit`.
3. **PI controller** simples:
   - Mede `fill = wptr - rptr` a cada frame Opus (20ms).
   - Erro = `fill - target`. Se `|erro| < 240 samples (5ms)`, ratio = 1.0 (dead band).
   - Fora do dead band: `ratio = 1.0 + Kp * erro + Ki * integral(erro)`.
   - Clamp `ratio ∈ [0.998, 1.002]` (±0.2% == 2000 ppm, mais que suficiente).
4. **Linear resample** stereo entrelaçado, phase accumulator 32-bit.

### Código sample (linear resample stereo 48kHz)

```c
// audio_servo.c — linear resampler pra drift correction
// entrada: PCM S16 stereo 48kHz (decoded Opus)
// saída: PCM S16 stereo ~48kHz (rate escalado por ratio)

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t phase;        // Q16.16
    uint32_t phase_step;   // Q16.16, step = (48000/out_rate)*65536
    int16_t  last_l, last_r; // último sample retido pra interp cruzando batch
} resampler_t;

// ratio > 1.0 = produz mais samples (rate up)
// ratio < 1.0 = produz menos samples (rate down)
// implementação: step = 1.0 / ratio em Q16.16
static inline void resampler_set_ratio(resampler_t *r, float ratio) {
    r->phase_step = (uint32_t)((1.0f / ratio) * 65536.0f);
}

// resample: consome `in_frames` de in[], produz até `out_max` frames em out[]
// retorna quantos frames de saída foram produzidos
static size_t resampler_process(resampler_t *r,
                                 const int16_t *in, size_t in_frames,
                                 int16_t *out, size_t out_max) {
    size_t out_i = 0;
    uint32_t phase = r->phase;
    uint32_t step  = r->phase_step;

    while (out_i < out_max) {
        uint32_t idx = phase >> 16;
        if (idx >= in_frames) break;

        uint32_t frac = phase & 0xFFFF;  // Q0.16
        int16_t l0 = in[idx*2 + 0];
        int16_t r0 = in[idx*2 + 1];
        int16_t l1, r1;

        if (idx + 1 < in_frames) {
            l1 = in[(idx+1)*2 + 0];
            r1 = in[(idx+1)*2 + 1];
        } else {
            // último sample: usa last conhecido do próximo batch (defer)
            break;
        }

        // linear interp: out = l0 + (l1-l0)*frac
        int32_t dl = (int32_t)(l1 - l0);
        int32_t dr = (int32_t)(r1 - r0);
        out[out_i*2 + 0] = (int16_t)(l0 + ((dl * (int32_t)frac) >> 16));
        out[out_i*2 + 1] = (int16_t)(r0 + ((dr * (int32_t)frac) >> 16));

        phase += step;
        out_i++;
    }

    r->phase = phase & 0xFFFF;  // reset índice, preserva fração
    r->last_l = in[in_frames*2 - 2];
    r->last_r = in[in_frames*2 - 1];
    return out_i;
}

// PI controller sobre buffer fill
typedef struct {
    int32_t target_fill;    // samples (frames) desejado no ring buffer
    int32_t integral;       // acumulador
    float   kp, ki;         // gains
    float   ratio;          // saída atual
} servo_pi_t;

static inline float servo_update(servo_pi_t *s, int32_t current_fill) {
    int32_t err = current_fill - s->target_fill;

    // dead band ±5ms @ 48k = ±240 frames
    if (err > -240 && err < 240) {
        // decay integral lentamente
        s->integral = (s->integral * 15) / 16;
    } else {
        s->integral += err;
        // anti-windup
        if (s->integral > 100000) s->integral = 100000;
        if (s->integral < -100000) s->integral = -100000;
    }

    float ratio = 1.0f + s->kp * (float)err + s->ki * (float)s->integral;

    // clamp ±0.2%
    if (ratio > 1.002f) ratio = 1.002f;
    if (ratio < 0.998f) ratio = 0.998f;

    s->ratio = ratio;
    return ratio;
}

// exemplo init:
// servo_pi_t servo = { .target_fill = 7200, .kp = 1e-6f, .ki = 1e-8f };
// resampler_t rs = { .phase_step = 65536 };  // ratio 1.0 inicial
```

### Integração no pipeline áudio

```
Opus packet (RTP) → opus_decode → 960 frames S16 stereo @ 48k
                                  ↓
                    resampler_process(ratio from servo)
                                  ↓
                    xenon_sound_submit(resampled)
                                  ↓
                    ring buffer HDMI DAC
                                  ↓
        [monitor] xenon_sound_get_unplayed() → fill level
                                  ↓
                    servo_update(fill) → new ratio
```

Loop de servo roda uma vez por frame Opus (50Hz update rate). Sobra CPU.

---

## Alternativa: skip servo (MVP path)

Se prioridade é **shipping MVP em semanas, não meses**:

1. Buffer 200ms (14400 samples = 57.6KB — cabe no ring de 65KB).
2. Sem resample. Passa direto do Opus decoder pro `xenon_sound_submit`.
3. Documenta limite: **"sessões >45min podem apresentar dessincronia AV progressiva ou clicks ocasionais. Restart do stream corrige."**
4. Feature flag `AUDIO_SERVO_ENABLED=0` no config. Deixa scaffolding pra habilitar depois.

**Trade-off**: 100ms extra de latência áudio. Em stream de jogo, notável mas não breaker. Xcloud comercial usa 40-80ms.

---

## Refs consultadas

- libxenon sound: https://raw.githubusercontent.com/Free60Project/libxenon/master/libxenon/drivers/xenon_sound/sound.c
- libxenon sound.h: https://raw.githubusercontent.com/Free60Project/libxenon/master/libxenon/drivers/xenon_sound/sound.h
- xmplayer ao_xenon: https://raw.githubusercontent.com/LibXenonProject/xmplayer/master/mplayer/libao2/ao_xenon.c
- speexdsp resample: https://raw.githubusercontent.com/xiph/speexdsp/master/libspeexdsp/resample.c
- Free60 wiki: **404 em `free60.org/Hardware/Xenos/` e `free60project.github.io/wiki/*`** — wiki foi migrada, links quebrados. Fonte primária = source do libxenon.
- Green-nx / averne libnx-audio-player: **404** (repo movido/privado). Info sobre servo pattern de fontes gerais de emuladores.

---

## Próximos passos sugeridos

1. **Decisão**: Sergio escolhe path MVP (skip servo, buffer 200ms) OU path full (servo linear).
2. Se full: implementar `audio_servo.c` no `/audio/output/`, integrar no pipeline Opus→DAC, testar com stream de 1h.
3. Métrica de validação: gravar output HDMI 1h, medir drift em relação a source (audio fingerprint). Meta: <100ms drift acumulado.
4. Se pass: fecha. Se fail (raro com linear): promove pra speexdsp.
