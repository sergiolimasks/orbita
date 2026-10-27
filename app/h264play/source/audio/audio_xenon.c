/*
 * XCloud360 v0.18 — Xbox glue for the audio core (see audio.h).
 *
 * Sound output: libxenon drivers/xenon_sound. The driver's header, its own
 * xenon_tone() and xmplayer's ao_xenon (AF_FORMAT_S16_LE) all agree the DAC
 * takes 48 kHz stereo signed 16-bit LITTLE-endian, so on this big-endian CPU
 * every sample is byte-swapped (AUDIO_PCM_SWAP=1, the default; =0 if a
 * hardware test ever proves otherwise — symptom: loud white noise).
 * The DMA ring is 32 descriptors of 2048 bytes and xenon_sound_submit only
 * marks COMPLETE descriptors valid, so we always submit exactly 512 frames
 * (2048 bytes), like xmplayer's outburst.
 */
#include "audio.h"
#include "aud_core.h"

#include <stdio.h>
#include <string.h>

#include <ppc/timebase.h>
#include <xenon_sound/sound.h>

#ifndef XCLOUD360_AUDIO_TARGET_MS
#define XCLOUD360_AUDIO_TARGET_MS 60
#endif
#ifndef XCLOUD360_AUDIO_HW_LOW
#define XCLOUD360_AUDIO_HW_LOW AUD_HW_LOW_DEFAULT
#endif
#ifndef XCLOUD360_AUDIO_PCM_SWAP
#define XCLOUD360_AUDIO_PCM_SWAP 1
#endif

extern const uint8_t aud_tone_stream[];
extern const size_t  aud_tone_stream_len;
extern const int     aud_tone_stream_pkts;

static inline void lwsync(void) { __asm__ __volatile__("lwsync" ::: "memory"); }

static uint64_t now_us(void) { return mftb() / (PPC_TIMEBASE_FREQ / 1000000ULL); }

/* ------------------------------------------------------------ device */
static int dev_queued(void *u) {
    (void)u;
    return xenon_sound_get_unplayed() / 4;
}

static uint16_t g_dev_buf[AUD_BLOCK * AUD_CH] __attribute__((aligned(128)));

/* v0.21.31: volume da saída do console (XC360/VOLUME.TXT, %). A saída P2 de AV
 * do Xbox 360 E é nível de linha: com fone ligado direto o som fica baixo. */
#ifndef ORBITA_AUDIO_GAIN_DEFAULT
#define ORBITA_AUDIO_GAIN_DEFAULT 100
#endif
static volatile int g_gain_q8 = ORBITA_AUDIO_GAIN_DEFAULT * 256 / 100;
void audio_set_volume_pct(int pct) {
    if (pct < 50) pct = 50;
    if (pct > 400) pct = 400;
    g_gain_q8 = pct * 256 / 100;
}
int audio_volume_pct(void) { return g_gain_q8 * 100 / 256; }

static void dev_submit(void *u, const int16_t *pcm, int frames) {
    (void)u;
    int g = g_gain_q8;
    for (int i = 0; i < frames * AUD_CH; i++) {
        int v = pcm[i];
        if (g != 256) {
            v = (v * g) >> 8;
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
        }
#if XCLOUD360_AUDIO_PCM_SWAP
        g_dev_buf[i] = __builtin_bswap16((uint16_t)(int16_t)v);
#else
        g_dev_buf[i] = (uint16_t)(int16_t)v;
#endif
    }
    xenon_sound_submit(g_dev_buf, frames * AUD_CH * 2);
}

/* ------------------------------------------------------------- state */
static aud_ring_t    g_ring __attribute__((aligned(128)));
static aud_engine_t *volatile g_eng = NULL;   /* main publishes once (hw_up) */

/* main -> consumer: requested source, bumped sequence. */
static volatile int      g_req_src = AUDIO_SRC_NONE;
static volatile unsigned g_req_seq = 0;
static unsigned          g_ack_seq = 0;            /* consumer only */
static volatile int      g_active = AUDIO_SRC_NONE; /* consumer writes, cb reads */

static volatile uint32_t g_rx_drop_idle = 0, g_rx_bad = 0;

/* test source (consumer thread) */
static uint32_t g_tone_off[512];
static int      g_tone_n = 0, g_tone_i = 0;
static uint32_t g_tone_seq = 0;
static uint64_t g_tone_due = 0;

/* main-side snapshots for per-period STAT averages */
static uint64_t g_prev_dec_us = 0;
static uint32_t g_prev_dec_fr = 0;

/* ------------------------------------------------------------- init */
/* v0.18.0 first hardware test: red screen early in boot, before the network
 * came up. Since then NOTHING audio touches hardware, lwIP or libopus at
 * boot: audio_init() is plain memory work; the sound chip and the Opus
 * engine come up on the first PLAY / X (hw_up, main thread, breadcrumbs on
 * screen). v0.20.0: the :9004 pcb (Opus from the PC) is gone; packets only
 * come from the on-Xbox WebRTC (audio_rx_packet). Anything fails -> that session is silent, the game goes on. */
static volatile int g_hw_state = 0;    /* 0 = not tried, 1 = up, -1 = failed */

int audio_init(void) {
    aud_ring_init(&g_ring);
    /* index the embedded test stream: [u16 BE len][payload]... */
    size_t o = 0;
    g_tone_n = 0;
    while (o + 2 <= aud_tone_stream_len && g_tone_n < (int)(sizeof(g_tone_off) / sizeof(g_tone_off[0]))) {
        size_t len = (size_t)aud_tone_stream[o] << 8 | aud_tone_stream[o + 1];
        if (!len || o + 2 + len > aud_tone_stream_len) break;
        g_tone_off[g_tone_n++] = (uint32_t)o;
        o += 2 + len;
    }
    printf("[audio] adiado: som + opus sobem no 1o jogo/teste (tom %d pacotes)\n", g_tone_n);
    return 0;
}

/* main thread, first session only. Returns 1 if audio is usable. */
static int hw_up(void) {
    if (g_hw_state) return g_hw_state > 0;
    g_hw_state = -1;                       /* a crash below leaves it "failed" */
    printf("[audio] 1/4 xenon_sound_init...\n");
    xenon_sound_init();
    printf("[audio] 2/4 som ok, unplayed=%d B; opus...\n", xenon_sound_get_unplayed());
    aud_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.target_ms = XCLOUD360_AUDIO_TARGET_MS;
    cfg.hw_low = XCLOUD360_AUDIO_HW_LOW;
    aud_out_t out = { dev_queued, dev_submit, NULL };
    aud_engine_t *e = aud_create(&cfg, &out, now_us);
    if (!e) {
        printf("[audio] 3/4 opus FALHOU (sem memoria?) - jogo segue sem som\n");
        return 0;
    }
    printf("[audio] 3/4 opus ok (alvo %d ms, hw_low %d)\n", XCLOUD360_AUDIO_TARGET_MS,
           XCLOUD360_AUDIO_HW_LOW);
    lwsync();
    g_eng = e;                             /* publish to the consumer */
    g_hw_state = 1;
    printf("[audio] 4/4 pronto\n");
    return 1;
}

/* ---------------------------------------------------------- control */
void audio_session_begin(int src) {
    if (src != AUDIO_SRC_NONE && !hw_up()) return;   /* silent session */
    g_req_src = src;
    lwsync();
    g_req_seq++;
}

void audio_session_end(void) { audio_session_begin(AUDIO_SRC_NONE); }

int audio_rx_packet(uint16_t seq, uint32_t ts, const uint8_t *p, int len) {
    /* stage 5: only while a WebRTC session is the active source;
     * otherwise counted as idle. */
    if (g_active != AUDIO_SRC_RTC) { g_rx_drop_idle++; return -1; }
    if (len < 1 || len > AUD_MAX_PAYLOAD) { g_rx_bad++; return -1; }
    return aud_ring_push(&g_ring, seq, ts, p, len);
}

/* --------------------------------------------------------- consumer */
static void tone_inject(void) {
    uint64_t t = now_us();
    if (t - g_tone_due > 200000 && t > g_tone_due) g_tone_due = t;   /* long stall: no burst */
    while (t >= g_tone_due && g_tone_n > 0) {
        uint32_t o = g_tone_off[g_tone_i];
        int len = aud_tone_stream[o] << 8 | aud_tone_stream[o + 1];
        aud_ring_push(&g_ring, (uint16_t)g_tone_seq, g_tone_seq * AUD_FRAME, aud_tone_stream + o + 2, len);
        g_tone_seq++;
        if (++g_tone_i == g_tone_n) g_tone_i = 0;
        g_tone_due += 20000;
    }
}

int audio_service(void) {
    if (!g_eng) return 0;
    lwsync();                              /* engine fields after the pointer */
    unsigned rs = g_req_seq;
    if (rs != g_ack_seq) {
        lwsync();
        int src = g_req_src;
        g_ack_seq = rs;
        if (src != AUDIO_SRC_NONE) {
            aud_reset(g_eng, &g_ring);
            g_tone_i = 0;
            g_tone_seq = 0;
            g_tone_due = now_us();
        } else {
            aud_stop(g_eng);
        }
        lwsync();
        g_active = src;
    }
    if (g_active == AUDIO_SRC_NONE) return 0;
    if (g_active == AUDIO_SRC_TEST) tone_inject();
    return aud_service(g_eng, &g_ring);
}

/* ------------------------------------------------------------- stats */
int audio_stat(char *dst, size_t n) {
    if (!g_eng || n == 0) return 0;
    aud_stats_t s;
    aud_get_stats(g_eng, &s);   /* racy word reads, only for telemetry */
    if (s.dec_frames < g_prev_dec_fr) g_prev_dec_fr = 0, g_prev_dec_us = 0;   /* new session */
    uint32_t df = s.dec_frames - g_prev_dec_fr;
    double dms = df ? (double)(s.dec_us - g_prev_dec_us) / 1000.0 / df : 0;
    g_prev_dec_fr = s.dec_frames;
    g_prev_dec_us = s.dec_us;
    int m = snprintf(dst, n, " a_pkts=%u a_lost=%u a_buf_ms=%d a_under=%u a_dec_ms=%.2f",
                     s.pkts, s.lost, s.buf_ms, s.under, dms);
    return m < 0 ? 0 : (m >= (int)n ? (int)n - 1 : m);
}

void audio_summary(char *dst, size_t n) {
    if (!g_eng) { snprintf(dst, n, "audio: sem engine"); return; }
    aud_stats_t s;
    aud_get_stats(g_eng, &s);
    double avg = s.dec_frames ? (double)s.dec_us / 1000.0 / s.dec_frames : 0;
    snprintf(dst, n, "audio %u pac, opus %.2f ms/20ms (max %.2f), buf %d ms, perdas %u, falhas %u",
             s.pkts, avg, s.dec_us_max / 1000.0, s.buf_ms, s.lost, s.under);
    printf("[audio] sessao: pkts=%u lost=%u fec=%u late=%u dup=%u under=%u resync=%u dec_err=%u "
           "str=%u shr=%u spl=%u dec=%.3f ms max=%.3f ms buf=%d ms rx_idle=%u rx_bad=%u ring_ovf=%u\n",
           s.pkts, s.lost, s.fec, s.late, s.dup, s.under, s.resync, s.dec_err, s.stretch, s.shrink,
           s.splices, avg, s.dec_us_max / 1000.0, s.buf_ms, (unsigned)g_rx_drop_idle,
           (unsigned)g_rx_bad, (unsigned)g_ring.overflow);
}
