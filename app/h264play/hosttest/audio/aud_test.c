/*
 * XCloud360 v0.18 — host test of source/audio/aud_core.c (the exact file the
 * Xbox builds) against libopus built for the host from the same source.
 *
 * Simulated time in 1 ms ticks, like the Xbox network thread: each tick the
 * "network" pushes the packets whose arrival time has come into the ring, the
 * engine is serviced, and a simulated DAC plays 48*(1+ppm) frames. Streams are
 * encoded here with libopus (CELT-only like xCloud) and, if present, the real
 * xCloud capture (vectors/xcloud-aco.pkts) and the embedded tone stream.
 *
 * usage: aud_test <vector dir>        (all tests; exit 1 on failure)
 *        aud_test bench <file.pkts>   (decode cost per frame)
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <opus.h>

#include "aud_core.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("    FAIL: %s  ", #c); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint64_t real_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
/* Engine clock: simulated ms, plus real time inside the tick so the decode
 * timing it measures is the host's real cost. */
static long g_sim_ms;
static uint64_t g_tick_real0;
static uint64_t sim_us(void) {
    uint64_t d = real_us() - g_tick_real0;
    return (uint64_t)g_sim_ms * 1000u + (d > 999 ? 999 : d);
}

/* ------------------------------------------------------------ streams */
typedef struct { uint16_t seq; uint32_t ts; int len; uint8_t d[1500]; double arr_ms; } spkt_t;
typedef struct { spkt_t *p; int n; } stream_t;

static stream_t encode_signal(int frames, int kind, uint16_t seq0) {
    int err;
    OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
    stream_t s = { calloc((size_t)frames, sizeof(spkt_t)), frames };
    double ph = 0;
    for (int f = 0; f < frames; f++) {
        int16_t pcm[960 * 2];
        for (int i = 0; i < 960; i++) {
            double t = (f * 960 + i) / 48000.0, v;
            if (kind == 0) v = 0.25 * sin(2 * M_PI * 440 * t);                     /* tone */
            else { v = 0.25 * sin(ph); ph += 2 * M_PI * (100 * pow(60, fmod(t, 5) / 5)) / 48000; } /* sweep */
            pcm[2 * i] = (int16_t)(v * 32767);
            pcm[2 * i + 1] = (int16_t)(v * 0.7 * 32767);
        }
        s.p[f].len = opus_encode(enc, pcm, 960, s.p[f].d, 1500);
        s.p[f].seq = (uint16_t)(seq0 + f);
        s.p[f].ts = (uint32_t)(f * 960);
    }
    opus_encoder_destroy(enc);
    return s;
}

static stream_t load_pkts(const char *path) {
    stream_t s = { NULL, 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return s;
    int cap = 0;
    uint8_t h[8];
    while (fread(h, 1, 8, f) == 8) {
        if (s.n == cap) { cap = cap ? cap * 2 : 256; s.p = realloc(s.p, (size_t)cap * sizeof(spkt_t)); }
        spkt_t *p = &s.p[s.n];
        p->len = h[0] << 8 | h[1];
        p->seq = (uint16_t)(h[2] << 8 | h[3]);
        p->ts = (uint32_t)h[4] << 24 | h[5] << 16 | h[6] << 8 | h[7];
        if (p->len > 1500 || fread(p->d, 1, (size_t)p->len, f) != (size_t)p->len) break;
        s.n++;
    }
    fclose(f);
    return s;
}

/* Reference: every packet decoded in order by a fresh decoder. */
static int16_t *ref_decode(const stream_t *s, long *nfr) {
    int err;
    OpusDecoder *d = opus_decoder_create(48000, 2, &err);
    int16_t *o = malloc((size_t)s->n * 5760 * 2 * sizeof(int16_t));
    long k = 0;
    for (int i = 0; i < s->n; i++) {
        int n = opus_decode(d, s->p[i].d, s->p[i].len, o + k * 2, 5760, 0);
        if (n > 0) k += n;
    }
    opus_decoder_destroy(d);
    *nfr = k;
    return o;
}

/* ---------------------------------------------------------------- DAC */
typedef struct {
    double ppm, acc;
    int16_t *q; long q_rd, q_wr;              /* submitted, not played */
    int16_t *out; long n_out, cap;            /* everything played */
    long first_pos;                           /* out index of 1st submitted frame */
    long starved;                             /* frames played from an empty queue */
    int submits, bad_submit;
} dac_t;

static int dac_queued(void *u) { dac_t *d = u; return (int)(d->q_wr - d->q_rd); }
static void dac_submit(void *u, const int16_t *pcm, int frames) {
    dac_t *d = u;
    if (frames != AUD_BLOCK) d->bad_submit++;
    if (d->first_pos < 0) d->first_pos = d->n_out + (d->q_wr - d->q_rd);
    memcpy(d->q + d->q_wr * 2, pcm, (size_t)frames * 4);
    d->q_wr += frames;
    d->submits++;
}
static void dac_tick(dac_t *d, int engine_playing) {
    d->acc += 48.0 * (1.0 + d->ppm * 1e-6);
    int n = (int)d->acc;
    d->acc -= n;
    for (int i = 0; i < n && d->n_out < d->cap; i++) {
        if (d->q_rd < d->q_wr) {
            d->out[d->n_out * 2] = d->q[d->q_rd * 2];
            d->out[d->n_out * 2 + 1] = d->q[d->q_rd * 2 + 1];
            d->q_rd++;
        } else {
            d->out[d->n_out * 2] = d->out[d->n_out * 2 + 1] = 0;
            if (engine_playing) d->starved++;
        }
        d->n_out++;
    }
}

/* ----------------------------------------------------------- scenario */
typedef struct {
    const char *name;
    int target_ms, no_drift;
    double ppm;
    double jitter_ms;          /* uniform extra delay 0..jitter */
    double loss;               /* random loss probability */
    int burst_every, burst_len;/* deterministic loss bursts */
    int swap_every;            /* swap packet pairs (reorder) */
    int stall_at, stall_ms;    /* network stall: packets pile up, then burst */
    int dup_every;
    double tail_ms;            /* run on after the last arrival */
    int gap_idx, gap_ms;       /* packets from gap_idx on arrive gap_ms later */
} scen_t;

typedef struct {
    aud_stats_t st;
    dac_t dac;
    int dropped;               /* packets the network removed */
    int min_buf, max_buf;      /* buf_ms in the second half */
    long max_step;             /* largest |x[n]-x[n-1]| while playing */
    double dur_s;
} result_t;

static uint32_t rng = 12345;
static double frand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

static result_t run(const scen_t *sc, const stream_t *src) {
    result_t R;
    memset(&R, 0, sizeof(R));
    rng = 777;
    int n = src->n;
    spkt_t *p = malloc((size_t)n * sizeof(spkt_t) * 2);
    int m = 0;
    for (int i = 0; i < n; i++) {
        int burst = sc->burst_every && (i % sc->burst_every) >= sc->burst_every - sc->burst_len;
        if ((sc->loss > 0 && frand() < sc->loss) || burst) { R.dropped++; continue; }
        p[m] = src->p[i];
        double a = 100.0 + i * 20.0 + (sc->jitter_ms > 0 ? frand() * sc->jitter_ms : 0);
        if (sc->gap_ms && i >= sc->gap_idx) a += sc->gap_ms;
        if (sc->stall_ms && a >= sc->stall_at && a < sc->stall_at + sc->stall_ms) a = sc->stall_at + sc->stall_ms;
        p[m].arr_ms = a;
        m++;
        if (sc->dup_every && i % sc->dup_every == 0) { p[m] = p[m - 1]; p[m].arr_ms += 3; m++; }
    }
    if (sc->swap_every)
        for (int i = 0; i + 1 < m; i += sc->swap_every) {
            double t = p[i].arr_ms; p[i].arr_ms = p[i + 1].arr_ms; p[i + 1].arr_ms = t;
        }
    /* deliver in arrival order */
    for (int i = 1; i < m; i++)
        for (int j = i; j > 0 && p[j].arr_ms < p[j - 1].arr_ms; j--) { spkt_t t = p[j]; p[j] = p[j - 1]; p[j - 1] = t; }

    double end_ms = (m ? p[m - 1].arr_ms : 0) + (sc->tail_ms > 0 ? sc->tail_ms : 400);
    long cap = (long)(end_ms * 48 * 1.01) + 96000;
    R.dac.ppm = sc->ppm;
    R.dac.q = malloc((size_t)cap * 4);
    R.dac.out = malloc((size_t)cap * 4);
    R.dac.cap = cap;
    R.dac.first_pos = -1;
    aud_cfg_t cfg = { .target_ms = sc->target_ms, .no_drift = sc->no_drift };
    aud_out_t out = { dac_queued, dac_submit, &R.dac };
    g_sim_ms = 0;
    g_tick_real0 = real_us();
    aud_engine_t *e = aud_create(&cfg, &out, sim_us);
    static aud_ring_t ring;
    aud_ring_init(&ring);
    aud_reset(e, &ring);
    R.min_buf = 1 << 30; R.max_buf = -1;
    int k = 0;
    long prev_n = 0;
    uint32_t lost_at_end = 0;
    for (long t = 0; t < (long)end_ms; t++) {
        g_sim_ms = t;
        g_tick_real0 = real_us();
        while (k < m && p[k].arr_ms <= t) { aud_ring_push(&ring, p[k].seq, p[k].ts, p[k].d, p[k].len); k++; }
        aud_service(e, &ring);
        aud_stats_t s;
        aud_get_stats(e, &s);
        /* Stats as of the end of the stream: once every packet has arrived,
         * the first PLC is just the buffer running out (correct behaviour). */
        if (k < m) { R.st = s; lost_at_end = s.lost; }
        else if (s.lost == lost_at_end) R.st = s;
        if (t > end_ms / 2 && s.playing && k < m) {
            if (s.buf_ms < R.min_buf) R.min_buf = s.buf_ms;
            if (s.buf_ms > R.max_buf) R.max_buf = s.buf_ms;
        }
        dac_tick(&R.dac, s.playing);
        prev_n = R.dac.n_out;
    }
    (void)prev_n;
    aud_stop(e);
    for (int t = 0; t < 200; t++) dac_tick(&R.dac, 0);
    for (long i = R.dac.first_pos > 0 ? R.dac.first_pos : 1; i < R.dac.n_out; i++)
        for (int c = 0; c < 2; c++) {
            long d = labs((long)R.dac.out[i * 2 + c] - R.dac.out[(i - 1) * 2 + c]);
            if (d > R.max_step) R.max_step = d;
        }
    R.dur_s = end_ms / 1000.0;
    free(p);
    free(R.dac.q);
    return R;
}

static long ref_max_step(const stream_t *s) {
    long nf, mx = 0;
    int16_t *r = ref_decode(s, &nf);
    for (long i = 1; i < nf; i++)
        for (int c = 0; c < 2; c++) {
            long d = labs((long)r[i * 2 + c] - r[(i - 1) * 2 + c]);
            if (d > mx) mx = d;
        }
    free(r);
    return mx;
}

static void show(const scen_t *sc, const result_t *R) {
    printf("  %-28s pkts=%u lost=%u(fec %u) late=%u dup=%u under=%u resync=%u starved=%ld "
           "buf=%d..%dms str=%u shr=%u spl=%u step=%ld dec=%.1fus/fr\n",
           sc->name, R->st.pkts, R->st.lost, R->st.fec, R->st.late, R->st.dup, R->st.under,
           R->st.resync, R->dac.starved, R->min_buf == 1 << 30 ? -1 : R->min_buf, R->max_buf,
           R->st.stretch, R->st.shrink, R->st.splices, R->max_step,
           R->st.dec_frames ? (double)R->st.dec_us / R->st.dec_frames : 0);
}

/* ------------------------------------------------------------ unit tests */
static void test_resample(void) {
    printf("[resample]\n");
    int16_t in[960 * 2], out[970 * 2];
    for (int i = 0; i < 960; i++) { in[2 * i] = (int16_t)(i * 10); in[2 * i + 1] = (int16_t)(-i * 10); }
    for (int m = 956; m <= 964; m++) {
        aud_resample(in, 960, out, m);
        CHECK(out[0] == 0 && out[(m - 1) * 2] == 9590 && out[(m - 1) * 2 + 1] == -9590, "endpoints m=%d", m);
        int mono = 1;
        for (int i = 1; i < m; i++) if (out[2 * i] < out[2 * (i - 1)]) mono = 0;
        CHECK(mono, "ramp stays monotonic m=%d", m);
        double want = 9590.0 * 480 / (m - 1);
        CHECK(fabs(out[480 * 2] - want) <= 1.5, "linear m=%d got %d want %.1f", m, out[960], want);
    }
}

static aud_ring_t g_tr;
static void *ring_producer(void *a) {
    (void)a;
    uint8_t b[300];
    for (uint32_t i = 0; i < 200000; i++) {
        int len = 1 + (int)(i % 299);
        for (int k = 0; k < len; k++) b[k] = (uint8_t)(i + k);
        while (aud_ring_push(&g_tr, (uint16_t)i, i * 960, b, len) != 0) { g_tr.overflow = 0; }
    }
    return NULL;
}
static void test_ring(void) {
    printf("[ring] SPSC, 2 threads, 200k packets\n");
    aud_ring_init(&g_tr);
    uint8_t big[AUD_MAX_PAYLOAD + 1] = {0};
    CHECK(aud_ring_push(&g_tr, 0, 0, big, AUD_MAX_PAYLOAD + 1) == -1, "oversize refused");
    CHECK(aud_ring_push(&g_tr, 0, 0, big, 0) == -1, "empty refused");
    g_tr.overflow = 0;
    pthread_t th;
    pthread_create(&th, NULL, ring_producer, NULL);
    uint32_t i = 0, bad = 0;
    while (i < 200000) {
        if (g_tr.tail == g_tr.head) continue;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        aud_pkt_t *s = &g_tr.slot[g_tr.tail & (AUD_RING_SLOTS - 1)];
        int len = 1 + (int)(i % 299);
        if (s->seq != (uint16_t)i || s->ts != i * 960 || s->len != len) bad++;
        else for (int k = 0; k < len; k++) if (s->data[k] != (uint8_t)(i + k)) { bad++; break; }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        g_tr.tail++;
        i++;
    }
    pthread_join(th, NULL);
    CHECK(bad == 0, "%u corrupt/out-of-order packets", bad);
}

/* Opus stack depth on a small thread stack (the Xbox decodes on the network
 * thread). Paint, decode the real stream + PLC, measure the high-water mark. */
static const stream_t *g_sk_stream;
static long g_sk_used;
static void *stack_thread(void *a) {
    size_t sz = (size_t)a;
    uint8_t *top = (uint8_t *)__builtin_frame_address(0);
    uint8_t *lo = top - sz + 8192;
    for (volatile uint8_t *q = lo; q < top - 512; q++) *q = 0xA5;
    int err;
    OpusDecoder *d = opus_decoder_create(48000, 2, &err);
    static int16_t pcm[5760 * 2];
    for (int i = 0; i < g_sk_stream->n; i++) {
        (void)!opus_decode(d, g_sk_stream->p[i].d, g_sk_stream->p[i].len, pcm, 5760, 0);
        if (i % 7 == 0) (void)!opus_decode(d, NULL, 0, pcm, 960, 0);
        if (i % 11 == 0 && i + 1 < g_sk_stream->n) (void)!opus_decode(d, g_sk_stream->p[i + 1].d, g_sk_stream->p[i + 1].len, pcm, 960, 1);
    }
    opus_decoder_destroy(d);
    volatile uint8_t *q = lo;
    while (q < top && *q == 0xA5) q++;
    g_sk_used = (long)(top - q);
    return NULL;
}
static void test_stack(const stream_t *s) {
    g_sk_stream = s;
    pthread_attr_t at;
    pthread_attr_init(&at);
    size_t sz = 256 * 1024;
    pthread_attr_setstacksize(&at, sz);
    pthread_t th;
    pthread_create(&th, &at, stack_thread, (void *)(sz - 16384));
    pthread_join(th, NULL);
    printf("[stack] opus_decode (stream + PLC + FEC) peak stack: %ld bytes\n", g_sk_used);
    CHECK(g_sk_used > 0 && g_sk_used < 48 * 1024, "stack %ld", g_sk_used);
}

/* -------------------------------------------------------------- bench */
static int bench(const char *path) {
    stream_t s = load_pkts(path);
    if (!s.n) { printf("no packets in %s\n", path); return 1; }
    int err;
    OpusDecoder *d = opus_decoder_create(48000, 2, &err);
    static int16_t pcm[5760 * 2];
    uint64_t best = ~0ull;
    for (int rep = 0; rep < 5; rep++) {
        opus_decoder_ctl(d, OPUS_RESET_STATE);
        uint64_t t0 = real_us();
        for (int i = 0; i < s.n; i++) (void)!opus_decode(d, s.p[i].d, s.p[i].len, pcm, 5760, 0);
        uint64_t dt = real_us() - t0;
        if (dt < best) best = dt;
    }
    printf("bench %s: %d frames, %.2f us/frame (%s)\n", path, s.n, (double)best / s.n, opus_get_version_string());
    return 0;
}

/* --------------------------------------------------------------- main */
#define TG 60   /* = Makefile AUDIO_TARGET_MS default */

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "bench")) return bench(argv[2]);
    const char *vdir = argc > 1 ? argv[1] : ".";
    printf("aud_test: %s, AUD_BLOCK=%d hw_low=%d\n", opus_get_version_string(), AUD_BLOCK, AUD_HW_LOW_DEFAULT);
    test_resample();
    test_ring();

    stream_t tone = encode_signal(1500, 0, 65000);      /* 30 s, seq wraps */
    stream_t tone60 = encode_signal(3000, 0, 7);        /* 60 s */
    stream_t sweep = encode_signal(1500, 1, 100);
    long ref_step = ref_max_step(&tone);
    long click = ref_step * 3 + 300;
    printf("[streams] tone ref max step %ld -> click threshold %ld\n", ref_step, click);
    test_stack(&sweep);

    printf("[scenarios] default target %d ms\n", TG);
    result_t R;

    /* 1. clean, no drift control: output must be the reference, bit-exact */
    {
        scen_t sc = { .name = "clean (bit-exact)", .target_ms = TG, .no_drift = 1, .ppm = 0, .jitter_ms = 0, .loss = 0, .burst_every = 0, .burst_len = 0, .swap_every = 0, .stall_at = 0, .stall_ms = 0, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &tone); show(&sc, &R);
        long nf; int16_t *ref = ref_decode(&tone, &nf);
        long diff = 0, cmp = 0;
        for (long i = 96; i < nf - 960 && R.dac.first_pos + i < R.dac.n_out; i++, cmp++)
            if (memcmp(&R.dac.out[(R.dac.first_pos + i) * 2], &ref[i * 2], 4)) diff++;
        CHECK(diff == 0 && cmp > nf - 2000, "%ld of %ld frames differ", diff, cmp);
        CHECK(R.st.lost == 0 && R.st.under == 0 && R.dac.starved == 0 && R.dac.bad_submit == 0, "clean");
        CHECK(R.max_step <= click, "click %ld", R.max_step);
        free(ref);
    }
    /* 2. reorder + jitter (no drift ctl): still bit-exact, nothing lost */
    {
        scen_t sc = { .name = "reorder+jitter5 (bit-exact)", .target_ms = TG, .no_drift = 1, .ppm = 0, .jitter_ms = 5, .loss = 0, .burst_every = 0, .burst_len = 0, .swap_every = 7, .stall_at = 0, .stall_ms = 0, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &tone); show(&sc, &R);
        long nf; int16_t *ref = ref_decode(&tone, &nf);
        long diff = 0;
        for (long i = 96; i < nf - 960 && R.dac.first_pos + i < R.dac.n_out; i++)
            if (memcmp(&R.dac.out[(R.dac.first_pos + i) * 2], &ref[i * 2], 4)) diff++;
        CHECK(diff == 0, "%ld frames differ", diff);
        CHECK(R.dac.starved == 0, "device ran dry while waiting for a late packet");
        CHECK(R.st.lost == 0 && R.st.late == 0 && R.st.under == 0, "reorder");
        free(ref);
    }
    /* 3. jitter 20 ms with drift control: steady level, no underrun */
    {
        scen_t sc = { .name = "jitter20", .target_ms = TG, .no_drift = 0, .ppm = 0, .jitter_ms = 20, .loss = 0, .burst_every = 0, .burst_len = 0, .swap_every = 0, .stall_at = 0, .stall_ms = 0, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &sweep); show(&sc, &R);
        CHECK(R.st.under == 0 && R.st.late == 0 && R.dac.starved == 0, "jitter");
        CHECK(R.min_buf >= TG - 15 && R.max_buf <= TG + 15, "level %d..%d", R.min_buf, R.max_buf);
    }
    /* 4. random 5 % loss + bursts of 3 every 200: every hole concealed once */
    {
        scen_t sc = { .name = "loss5%+bursts", .target_ms = TG, .no_drift = 0, .ppm = 0, .jitter_ms = 10, .loss = 0.05, .burst_every = 200, .burst_len = 3, .swap_every = 0, .stall_at = 0, .stall_ms = 0, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &tone); show(&sc, &R);
        CHECK(R.st.lost == (uint32_t)R.dropped, "lost %u dropped %d", R.st.lost, R.dropped);
        CHECK(R.st.under == 0 && R.dac.starved == 0 && R.st.late == 0, "loss");
        CHECK(R.max_step <= click, "click under loss %ld", R.max_step);
    }
    /* 5/6. clock drift +-500 ppm (5x a cheap crystal), 60 s */
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        scen_t sc = { .name = sgn > 0 ? "drift +500ppm (DAC fast)" : "drift -500ppm (DAC slow)", .target_ms = TG, .no_drift = 0, .ppm = 500.0 * sgn, .jitter_ms = 10, .loss = 0, .burst_every = 0, .burst_len = 0, .swap_every = 0, .stall_at = 0, .stall_ms = 0, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &tone60); show(&sc, &R);
        double want = 500e-6 * 48000 * R.dur_s;
        uint32_t got = sgn > 0 ? R.st.stretch : R.st.shrink;
        CHECK(R.st.under == 0 && R.dac.starved == 0, "drift underrun");
        CHECK(R.min_buf >= TG - 15 && R.max_buf <= TG + 15, "level %d..%d", R.min_buf, R.max_buf);
        /* the first seconds are absorbed by the dead band, not corrected */
        CHECK(got > want * 0.5 && got < want * 1.5 + 200, "corrected %u samples, drift %.0f", got, want);
        CHECK(R.st.splices == 0, "no splices for slow drift");
        CHECK(R.max_step <= click, "click %ld", R.max_step);
    }
    /* 7. 400 ms network stall then a burst: underrun once, splices back down */
    {
        scen_t sc = { .name = "stall 400ms + burst", .target_ms = TG, .no_drift = 0, .ppm = 0, .jitter_ms = 5, .loss = 0, .burst_every = 0, .burst_len = 0, .swap_every = 0, .stall_at = 10000, .stall_ms = 400, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &tone); show(&sc, &R);
        CHECK(R.st.under == 1 && R.st.lost == 10 && R.st.late == 10, "under %u lost %u late %u",
              R.st.under, R.st.lost, R.st.late);
        CHECK(R.max_buf <= TG + 15, "recovered level %d", R.max_buf);
        CHECK(R.max_step <= click, "click across stall %ld", R.max_step);
    }
    /* 8. duplicates */
    {
        scen_t sc = { .name = "duplicates", .target_ms = TG, .no_drift = 1, .ppm = 0, .jitter_ms = 5, .loss = 0, .burst_every = 0, .burst_len = 0, .swap_every = 0, .stall_at = 0, .stall_ms = 0, .dup_every = 13, .tail_ms = 0 };
        R = run(&sc, &tone); show(&sc, &R);
        CHECK(R.st.dup > 100 && R.st.lost == 0 && R.st.under == 0, "dup");
    }
    /* 9. stream stops, new session starts with an unrelated seq/ts */
    {
        stream_t a = encode_signal(300, 0, 1000), b = encode_signal(300, 0, 40000);
        stream_t ab = { malloc(600 * sizeof(spkt_t)), 600 };
        memcpy(ab.p, a.p, 300 * sizeof(spkt_t));
        memcpy(ab.p + 300, b.p, 300 * sizeof(spkt_t));
        scen_t sc = { .name = "stop 1s + new seq", .target_ms = TG, .no_drift = 0, .ppm = 0, .jitter_ms = 5, .loss = 0, .burst_every = 0, .burst_len = 0, .swap_every = 0, .stall_at = 0, .stall_ms = 0, .gap_idx = 300, .gap_ms = 1000, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &ab); show(&sc, &R);
        CHECK(R.st.under == 1 && R.st.resync == 1, "under %u resync %u", R.st.under, R.st.resync);
        CHECK(R.st.lost == 10, "PLC until the stop is detected (%u)", R.st.lost);
        CHECK(R.max_step <= click, "click at stop/start %ld", R.max_step);
    }
    /* 10. targets 40 and 60 */
    for (int tg = 40; tg <= 60; tg += 20) {
        scen_t sc = { .name = tg == 40 ? "target 40 jitter3" : "target 60 jitter25", .target_ms = tg, .no_drift = 0, .ppm = 100, .jitter_ms = tg == 40 ? 3 : 25, .loss = 0, .burst_every = 0, .burst_len = 0, .swap_every = 0, .stall_at = 0, .stall_ms = 0, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &sweep); show(&sc, &R);
        CHECK(R.st.under == 0 && R.st.late == 0, "target %d", tg);
        CHECK(R.min_buf >= tg - 15 && R.max_buf <= tg + 15, "level %d..%d", R.min_buf, R.max_buf);
    }
    /* 11. the real xCloud capture, and 12. the embedded tone stream */
    const char *names[2] = { "xcloud.pkts", "tone.pkts" };
    for (int v = 0; v < 2; v++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", vdir, names[v]);
        stream_t s = load_pkts(path);
        if (!s.n) { printf("  (skip %s: not found)\n", path); if (v == 1) { CHECK(0, "tone.pkts missing"); } continue; }
        scen_t sc = { .name = v ? "embedded tone stream" : "real xCloud capture", .target_ms = TG, .no_drift = 0, .ppm = 0, .jitter_ms = 15, .loss = 0.01, .burst_every = 0, .burst_len = 0, .swap_every = 0, .stall_at = 0, .stall_ms = 0, .dup_every = 0, .tail_ms = 0 };
        R = run(&sc, &s); show(&sc, &R);
        CHECK(R.st.dec_err == 0, "decode errors %u", R.st.dec_err);
        CHECK(R.st.lost == (uint32_t)R.dropped && R.st.under == 0, "real stream");
        if (v == 1) {   /* channel check: first 0.8 s left only, then right only */
            double el = 0, er = 0, fl = 0, fr = 0;
            long b0 = R.dac.first_pos;
            for (long i = b0 + 4800; i < b0 + 33600; i++) { el += abs(R.dac.out[i * 2]); er += abs(R.dac.out[i * 2 + 1]); }
            for (long i = b0 + 43200; i < b0 + 72000; i++) { fl += abs(R.dac.out[i * 2]); fr += abs(R.dac.out[i * 2 + 1]); }
            printf("    channels: part1 L/R %.0f/%.0f  part2 L/R %.0f/%.0f\n", el, er, fl, fr);
            CHECK(el > 20 * er && fr > 20 * fl, "left-then-right");
        }
    }
    printf("aud_test: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
