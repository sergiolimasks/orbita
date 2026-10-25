/*
 * XCloud360 v0.18 — portable audio core: packet ring + jitter buffer + Opus
 * decode/PLC + drift control + output scheduling.
 *
 * Knows nothing about where packets come from (today: UDP :9004 from the PC
 * bridge; later: the Xbox's own WebRTC stack) nor about the sound hardware
 * (aud_out_t). Builds on the Xbox and on the Linux host (hosttest/audio).
 *
 * Threads: exactly one PRODUCER calls aud_ring_push(); exactly one CONSUMER
 * owns the aud_engine_t (aud_service and everything else that takes the
 * engine). They may be the same thread. Lock-free: head/tail with a
 * store barrier (lwsync on PPC).
 */
#ifndef XCLOUD360_AUD_CORE_H
#define XCLOUD360_AUD_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUD_RATE        48000
#define AUD_CH          2
#define AUD_FRAME       960     /* 20 ms: what xCloud sends (CELT FB stereo) */
#define AUD_MAX_FRAME   5760    /* 120 ms: biggest Opus packet */
#define AUD_BLOCK       512     /* frames per device submit (2048 bytes) */
#define AUD_RING_SLOTS  64      /* power of two; 1.28 s of 20 ms packets */
#define AUD_JB_SLOTS    64      /* power of two */
#define AUD_MAX_PAYLOAD 1500
/* Device queue refill threshold. A packet is due when queue + FIFO falls to
 * about hw_low + AUD_BLOCK/2, so jitter tolerance ~= target - that floor
 * (~ target - 21 ms). 768 = 16 ms: the consumer runs every ~1 ms. */
#define AUD_HW_LOW_DEFAULT 768
/* If the packet due next is missing, wait for it until the device queue is
 * down to this before concealing (late packets get ~8 ms more). */
#define AUD_HW_CRIT_DEFAULT 384

/* ---------------------------------------------------------- packet ring */
typedef struct {
    uint16_t len, seq;
    uint32_t ts;
    uint8_t  data[AUD_MAX_PAYLOAD];
} aud_pkt_t;

typedef struct {
    aud_pkt_t slot[AUD_RING_SLOTS];
    volatile uint32_t head;      /* producer only */
    volatile uint32_t tail;      /* consumer only */
    volatile uint32_t overflow;  /* producer only: dropped, ring full */
} aud_ring_t;

void aud_ring_init(aud_ring_t *r);
/* Producer. 0 = queued, -1 = full or bad length (counted in overflow). */
int  aud_ring_push(aud_ring_t *r, uint16_t seq, uint32_t ts, const uint8_t *p, int len);

/* --------------------------------------------------------- sound device */
typedef struct {
    /* frames submitted and not yet played */
    int  (*queued_frames)(void *u);
    /* native-endian interleaved stereo int16; always AUD_BLOCK frames */
    void (*submit)(void *u, const int16_t *pcm, int frames);
    void *u;
} aud_out_t;

typedef struct {
    int target_ms;    /* total buffered (jitter + device queue) to aim for */
    int hw_low;       /* frames: refill the device queue below this (0 = default) */
    int hw_crit;      /* frames: conceal a missing packet only below this (0 = default) */
    int no_drift;     /* 1 = never stretch/shrink/splice (tests) */
} aud_cfg_t;

typedef struct {
    uint32_t pkts;      /* packets taken from the ring this session */
    uint32_t lost;      /* frames synthesised for missing seqs (PLC/FEC) */
    uint32_t fec;       /* ...of which recovered with in-band FEC */
    uint32_t late;      /* arrived after their slot was played */
    uint32_t dup;
    uint32_t under;     /* underruns: no packet at all for >= 80 ms while playing */
    uint32_t resync;    /* seq jumped too far: buffer flushed */
    uint32_t dec_err;   /* opus_decode < 0 */
    uint32_t stretch;   /* samples inserted by drift control */
    uint32_t shrink;    /* samples removed by drift control */
    uint32_t splices;   /* whole frames dropped (cross-faded) when far over */
    uint32_t dec_frames;
    uint64_t dec_us;    /* time inside opus_decode, all calls */
    uint32_t dec_us_max;
    int      buf_ms;    /* smoothed buffer level */
    int      playing;
} aud_stats_t;

typedef struct aud_engine aud_engine_t;

/* Allocates (malloc, once). now_us: monotonic clock (decode timing and
 * arrival gaps). NULL on failure. */
aud_engine_t *aud_create(const aud_cfg_t *cfg, const aud_out_t *out, uint64_t (*now_us)(void));
/* New session: decoder state, jitter buffer, PCM, stats all zero. Drops
 * whatever is still in the ring. The device queue is left to drain. */
void aud_reset(aud_engine_t *e, aud_ring_t *r);
/* Consumer. Moves packets ring -> jitter buffer and keeps the device fed.
 * At most one Opus decode per call (two on a splice). Returns 1 if it decoded. */
int  aud_service(aud_engine_t *e, aud_ring_t *r);
/* End of session: fade the not-yet-submitted PCM to zero, submit it, idle. */
void aud_stop(aud_engine_t *e);
void aud_get_stats(const aud_engine_t *e, aud_stats_t *s);

/* Exposed for tests. */
int  aud_resample(const int16_t *in, int n_in, int16_t *out, int n_out);

#ifdef __cplusplus
}
#endif

#endif
