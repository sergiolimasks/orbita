/*
 * XCloud360 v0.18 — portable audio core (see aud_core.h).
 *
 * Playout is pulled by the sound device: whenever its queue drops below
 * cfg.hw_low we need 512 more frames; if the PCM FIFO can't supply them we
 * produce one more 20 ms frame, and THAT is the moment a packet is due:
 *   - next_seq in the jitter buffer      -> decode it
 *   - missing                            -> wait while the device still has
 *                                           > hw_crit frames (it may be late),
 *                                           then conceal it: in-band FEC from
 *                                           next_seq+1 if present, else PLC
 * Concealment keeps the clock running: stragglers that arrive after their
 * slot are dropped as late, so a network hiccup never adds latency. Dry
 * (nothing buffered) for 4 frames in a row = one underrun; for 10 (200 ms:
 * the stream stopped) fade to 0, stop feeding the device, re-buffer.
 *
 * Buffer level = device queue + FIFO + packets waiting. A slow EMA of it is
 * held at cfg.target_ms by resampling each decoded frame by 1..4 samples
 * (0.1..0.4 % for 20 ms, endpoints preserved -> no discontinuity, inaudible),
 * which absorbs clock drift between xCloud's 48 kHz and the Xbox DAC. Only
 * far above target (>20 ms over, e.g. after a network stall dumped a burst)
 * a whole frame is dropped, cross-faded over 2 ms. Every start fades in
 * over 2 ms; every stop/underrun fades the unsubmitted tail to zero.
 */
#include "aud_core.h"

#include <stdlib.h>
#include <string.h>
#include <opus.h>

#if defined(__powerpc__) || defined(__PPC__) || defined(__ppc__)
#define AUD_BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#else
#define AUD_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

#define FIFO_CAP     16384      /* frames */
#define FADE_FRAMES  96         /* 2 ms */
#define TAIL_FADE    480        /* 10 ms */
#define UNDER_DRY    4          /* dry frames in a row = 1 underrun (80 ms) */
#define IDLE_DRY     10         /* dry frames in a row = stream stopped (200 ms) */
#define DRIFT_TOL    240        /* 5 ms dead band */
#define SPLICE_OVER  960        /* 20 ms over target: drop a frame */
#define STEP_FRAMES  240        /* +1 sample/frame per 5 ms of error */
#define MAX_ADJ      4          /* samples per 20 ms frame */

/* ------------------------------------------------------------- ring */

void aud_ring_init(aud_ring_t *r) {
    r->head = r->tail = 0;
    r->overflow = 0;
}

int aud_ring_push(aud_ring_t *r, uint16_t seq, uint32_t ts, const uint8_t *p, int len) {
    if (len <= 0 || len > AUD_MAX_PAYLOAD) { r->overflow++; return -1; }
    uint32_t h = r->head;
    if (h - r->tail >= AUD_RING_SLOTS) { r->overflow++; return -1; }
    AUD_BARRIER();                     /* consumer is done with this slot */
    aud_pkt_t *s = &r->slot[h & (AUD_RING_SLOTS - 1)];
    s->len = (uint16_t)len;
    s->seq = seq;
    s->ts = ts;
    memcpy(s->data, p, (size_t)len);
    AUD_BARRIER();                     /* slot visible before the new head */
    r->head = h + 1;
    return 0;
}

static const aud_pkt_t *ring_peek(aud_ring_t *r) {
    uint32_t t = r->tail;
    if (t == r->head) return NULL;
    AUD_BARRIER();                     /* head read before slot data */
    return &r->slot[t & (AUD_RING_SLOTS - 1)];
}

static void ring_pop(aud_ring_t *r) {
    AUD_BARRIER();                     /* finished reading before release */
    r->tail = r->tail + 1;
}

/* ----------------------------------------------------------- engine */

typedef struct {
    int      valid;
    uint16_t seq, len;
    uint8_t  data[AUD_MAX_PAYLOAD];
} jb_slot_t;

enum { ST_IDLE = 0, ST_PLAY };

struct aud_engine {
    aud_cfg_t   cfg;
    aud_out_t   out;
    uint64_t  (*now)(void);
    OpusDecoder *dec;

    jb_slot_t   jb[AUD_JB_SLOTS];
    int         nbuf;
    int         state;
    uint16_t    next_seq;
    int         have_next;     /* next_seq means something (played before) */
    uint16_t    last_rx;
    int         have_rx;
    int         frame;         /* size of the last real frame (PLC uses it) */
    int         plc_run;
    int         fade_in;
    int         ema, ema_ok;

    int         f_rd, f_wr;
    int16_t     fifo[FIFO_CAP * AUD_CH];
    int16_t     a[AUD_MAX_FRAME * AUD_CH];
    int16_t     b[AUD_MAX_FRAME * AUD_CH];
    int16_t     rs[(AUD_MAX_FRAME + MAX_ADJ) * AUD_CH];

    aud_stats_t st;
};

aud_engine_t *aud_create(const aud_cfg_t *cfg, const aud_out_t *out, uint64_t (*now_us)(void)) {
    aud_engine_t *e = (aud_engine_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cfg = *cfg;
    if (e->cfg.target_ms <= 0) e->cfg.target_ms = 50;
    if (e->cfg.hw_low <= 0) e->cfg.hw_low = AUD_HW_LOW_DEFAULT;
    if (e->cfg.hw_crit <= 0) e->cfg.hw_crit = AUD_HW_CRIT_DEFAULT;
    if (e->cfg.hw_crit > e->cfg.hw_low) e->cfg.hw_crit = e->cfg.hw_low;
    e->out = *out;
    e->now = now_us;
    int err = 0;
    e->dec = (OpusDecoder *)malloc((size_t)opus_decoder_get_size(AUD_CH));
    if (!e->dec || (err = opus_decoder_init(e->dec, AUD_RATE, AUD_CH)) != OPUS_OK) {
        free(e->dec);
        free(e);
        return NULL;
    }
    e->frame = AUD_FRAME;
    return e;
}

static void jb_clear(aud_engine_t *e) {
    for (int i = 0; i < AUD_JB_SLOTS; i++) e->jb[i].valid = 0;
    e->nbuf = 0;
}

void aud_reset(aud_engine_t *e, aud_ring_t *r) {
    opus_decoder_ctl(e->dec, OPUS_RESET_STATE);
    jb_clear(e);
    e->state = ST_IDLE;
    e->have_next = e->have_rx = 0;
    e->frame = AUD_FRAME;
    e->plc_run = e->fade_in = 0;
    e->ema = e->ema_ok = 0;
    e->f_rd = e->f_wr = 0;
    memset(&e->st, 0, sizeof(e->st));
    if (r) { AUD_BARRIER(); r->tail = r->head; }
}

void aud_get_stats(const aud_engine_t *e, aud_stats_t *s) { *s = e->st; }

/* ------------------------------------------------------------- PCM */

int aud_resample(const int16_t *in, int n_in, int16_t *out, int n_out) {
    if (n_in <= 0 || n_out <= 0) return 0;
    if (n_in == 1 || n_out == 1) {
        for (int i = 0; i < n_out; i++) { out[2 * i] = in[0]; out[2 * i + 1] = in[1]; }
        return n_out;
    }
    /* Linear, Q16 position; first and last samples land exactly on the
     * input's, so consecutive frames join without a step. */
    uint32_t step = (uint32_t)(((uint64_t)(n_in - 1) << 16) / (uint32_t)(n_out - 1));
    uint32_t pos = 0;
    for (int i = 0; i < n_out; i++, pos += step) {
        int idx = (int)(pos >> 16);
        int fr = (int)((pos & 0xffff) >> 1);          /* Q15 */
        if (idx >= n_in - 1) { idx = n_in - 1; fr = 0; }
        for (int c = 0; c < AUD_CH; c++) {
            int x0 = in[idx * AUD_CH + c];
            int x1 = fr ? in[(idx + 1) * AUD_CH + c] : x0;
            out[i * AUD_CH + c] = (int16_t)(x0 + (((x1 - x0) * fr) >> 15));
        }
    }
    out[(n_out - 1) * AUD_CH] = in[(n_in - 1) * AUD_CH];
    out[(n_out - 1) * AUD_CH + 1] = in[(n_in - 1) * AUD_CH + 1];
    return n_out;
}

static inline int fifo_frames(const aud_engine_t *e) { return e->f_wr - e->f_rd; }

static void fifo_append(aud_engine_t *e, const int16_t *pcm, int n) {
    if (e->f_wr + n > FIFO_CAP) {
        int have = fifo_frames(e);
        memmove(e->fifo, e->fifo + e->f_rd * AUD_CH, (size_t)have * AUD_CH * sizeof(int16_t));
        e->f_rd = 0;
        e->f_wr = have;
        if (e->f_wr + n > FIFO_CAP) n = FIFO_CAP - e->f_wr;   /* can't happen */
    }
    memcpy(e->fifo + e->f_wr * AUD_CH, pcm, (size_t)n * AUD_CH * sizeof(int16_t));
    e->f_wr += n;
}

static void fifo_submit_block(aud_engine_t *e) {
    e->out.submit(e->out.u, e->fifo + e->f_rd * AUD_CH, AUD_BLOCK);
    e->f_rd += AUD_BLOCK;
    if (e->f_rd == e->f_wr) e->f_rd = e->f_wr = 0;
}

/* Fade the unsubmitted tail to zero, pad to whole blocks, hand it all to the
 * device. The device queue then ends on silence instead of mid-waveform. */
static void flush_fade(aud_engine_t *e) {
    int n = fifo_frames(e);
    if (n > 0) {
        int f = n < TAIL_FADE ? n : TAIL_FADE;
        int16_t *t = e->fifo + (e->f_wr - f) * AUD_CH;
        for (int i = 0; i < f; i++) {
            int g = f - 1 - i;                    /* f-1 .. 0 */
            t[i * 2]     = (int16_t)(t[i * 2] * g / f);
            t[i * 2 + 1] = (int16_t)(t[i * 2 + 1] * g / f);
        }
        int pad = (AUD_BLOCK - n % AUD_BLOCK) % AUD_BLOCK;
        static const int16_t zero[AUD_BLOCK * AUD_CH];
        if (pad) fifo_append(e, zero, pad);
        while (fifo_frames(e) >= AUD_BLOCK) fifo_submit_block(e);
    }
    e->f_rd = e->f_wr = 0;
}

/* ---------------------------------------------------------- decode */

static int decode(aud_engine_t *e, const uint8_t *d, int len, int fec, int16_t *out) {
    uint64_t t0 = e->now();
    int max = (d && !fec) ? AUD_MAX_FRAME : e->frame;
    int n = opus_decode(e->dec, d, len, out, max, fec);
    if (n < 0) {
        e->st.dec_err++;
        n = opus_decode(e->dec, NULL, 0, out, e->frame, 0);
        if (n < 0) { memset(out, 0, (size_t)e->frame * AUD_CH * sizeof(int16_t)); n = e->frame; }
    } else if (d && !fec && n > 0) {
        e->frame = n;
    }
    uint32_t dt = (uint32_t)(e->now() - t0);
    e->st.dec_us += dt;
    if (dt > e->st.dec_us_max) e->st.dec_us_max = dt;
    e->st.dec_frames++;
    return n;
}

static jb_slot_t *jb_get(aud_engine_t *e, uint16_t seq) {
    jb_slot_t *s = &e->jb[seq & (AUD_JB_SLOTS - 1)];
    return (s->valid && s->seq == seq) ? s : NULL;
}

static void jb_take(aud_engine_t *e, jb_slot_t *s) { s->valid = 0; e->nbuf--; }

static void go_idle(aud_engine_t *e) {
    flush_fade(e);
    e->state = ST_IDLE;
    e->st.playing = 0;
    e->ema_ok = 0;
}

static void jb_insert(aud_engine_t *e, const aud_pkt_t *p) {
    e->st.pkts++;
    if (e->have_next) {
        int d = (int16_t)(uint16_t)(p->seq - e->next_seq);
        if (d < 0 && d >= -AUD_JB_SLOTS) { e->st.late++; return; }
        if (d < 0 || d >= AUD_JB_SLOTS) {
            /* seq jumped (new stream, or >1.2 s behind): start over */
            e->st.resync++;
            if (e->state == ST_PLAY) go_idle(e);
            jb_clear(e);
            e->have_next = 0;
        }
    } else if (e->have_rx) {
        int d = (int16_t)(uint16_t)(p->seq - e->last_rx);
        if (d > AUD_JB_SLOTS || d < -AUD_JB_SLOTS) jb_clear(e);
    }
    jb_slot_t *s = &e->jb[p->seq & (AUD_JB_SLOTS - 1)];
    if (s->valid) {
        if (s->seq == p->seq) { e->st.dup++; return; }
        jb_take(e, s);                         /* stale leftover */
    }
    s->valid = 1;
    s->seq = p->seq;
    s->len = p->len;
    memcpy(s->data, p->data, p->len);
    e->nbuf++;
    e->last_rx = p->seq;
    e->have_rx = 1;
}

static int level_frames(aud_engine_t *e) {
    return e->out.queued_frames(e->out.u) + fifo_frames(e) + e->nbuf * e->frame;
}

/* One 20 ms (or whatever the stream uses) frame into the FIFO. */
static void produce_frame(aud_engine_t *e) {
    int16_t *x = e->a;
    int n, real = 0;
    jb_slot_t *s = jb_get(e, e->next_seq);
    if (s) {
        n = decode(e, s->data, s->len, 0, x);
        jb_take(e, s);
        e->plc_run = 0;
        real = 1;
    } else if (e->nbuf > 0) {
        jb_slot_t *f = jb_get(e, (uint16_t)(e->next_seq + 1));
        if (f) { n = decode(e, f->data, f->len, 1, x); e->st.fec++; }
        else     n = decode(e, NULL, 0, 0, x);
        e->st.lost++;
        e->plc_run = 0;
    } else {
        n = decode(e, NULL, 0, 0, x);
        e->st.lost++;
        if (++e->plc_run == UNDER_DRY) e->st.under++;
    }
    e->next_seq++;

    if (real && !e->cfg.no_drift) {
        int lvl = level_frames(e) + n;
        if (!e->ema_ok) { e->ema = lvl; e->ema_ok = 1; }
        else e->ema += (lvl - e->ema) / 16;
        int err = e->ema - e->cfg.target_ms * (AUD_RATE / 1000);
        jb_slot_t *y;
        if (err > SPLICE_OVER && (y = jb_get(e, e->next_seq)) != NULL) {
            /* Way over: drop this frame, keep the next one, and cross-fade
             * from the dropped frame (the natural continuation of what is
             * playing) into it so the splice has no step. */
            int ny = decode(e, y->data, y->len, 0, e->b);
            jb_take(e, y);
            e->next_seq++;
            int L = FADE_FRAMES;
            if (L > n) L = n;
            if (L > ny) L = ny;
            for (int i = 0; i < L; i++)
                for (int c = 0; c < AUD_CH; c++)
                    e->b[i * 2 + c] = (int16_t)((x[i * 2 + c] * (L - i) + e->b[i * 2 + c] * i) / L);
            x = e->b;
            n = ny;
            e->ema -= n;
            e->st.splices++;
        } else if (err > DRIFT_TOL || err < -DRIFT_TOL) {
            int k = 1 + ((err > 0 ? err : -err) - DRIFT_TOL) / STEP_FRAMES;
            if (k > MAX_ADJ) k = MAX_ADJ;
            if (k > n / 8) k = n / 8;
            if (k > 0) {
                int m = err > 0 ? n - k : n + k;
                aud_resample(x, n, e->rs, m);
                x = e->rs;
                n = m;
                if (err > 0) e->st.shrink += (uint32_t)k; else e->st.stretch += (uint32_t)k;
            }
        }
        e->st.buf_ms = e->ema / (AUD_RATE / 1000);
    }

    if (e->fade_in) {
        int L = n < FADE_FRAMES ? n : FADE_FRAMES;
        for (int i = 0; i < L; i++) {
            x[i * 2]     = (int16_t)(x[i * 2] * i / L);
            x[i * 2 + 1] = (int16_t)(x[i * 2 + 1] * i / L);
        }
        e->fade_in = 0;
    }
    fifo_append(e, x, n);

    if (e->plc_run >= IDLE_DRY) {
        go_idle(e);
        e->plc_run = 0;
    }
}

static void try_start(aud_engine_t *e) {
    /* Pre-buffer the whole target: most of it moves into the device queue
     * at once, and what stays queued as packets is the jitter margin. */
    int need = (e->cfg.target_ms + 19) / 20;
    if (need < 1) need = 1;
    if (e->nbuf < need) return;
    int best = 0x7fffffff;
    for (int i = 0; i < AUD_JB_SLOTS; i++) {
        if (!e->jb[i].valid) continue;
        int d = (int16_t)(uint16_t)(e->jb[i].seq - e->last_rx);
        if (d < best) { best = d; e->next_seq = e->jb[i].seq; }
    }
    e->have_next = 1;
    e->state = ST_PLAY;
    e->st.playing = 1;
    e->fade_in = 1;
    e->plc_run = 0;
    e->ema_ok = 0;
}

int aud_service(aud_engine_t *e, aud_ring_t *r) {
    const aud_pkt_t *p;
    while ((p = ring_peek(r)) != NULL) {
        jb_insert(e, p);
        ring_pop(r);
    }
    if (e->state == ST_IDLE) try_start(e);
    if (e->state != ST_PLAY) {
        if (!e->st.playing) e->st.buf_ms = e->nbuf * e->frame / (AUD_RATE / 1000);
        return 0;
    }
    int worked = 0;
    while (e->out.queued_frames(e->out.u) < e->cfg.hw_low) {
        if (fifo_frames(e) < AUD_BLOCK) {
            if (worked) break;               /* one decode per call */
            if (!jb_get(e, e->next_seq) && e->out.queued_frames(e->out.u) >= e->cfg.hw_crit)
                break;                       /* due packet may still come */
            produce_frame(e);
            worked = 1;
            if (e->state != ST_PLAY) break;
            continue;
        }
        fifo_submit_block(e);
    }
    return worked;
}

void aud_stop(aud_engine_t *e) {
    if (e->state == ST_PLAY) {
        if (fifo_frames(e) < TAIL_FADE) produce_frame(e);   /* something to fade */
        if (e->state == ST_PLAY) go_idle(e);
    }
    jb_clear(e);
    e->have_next = 0;
    e->st.playing = 0;
}
