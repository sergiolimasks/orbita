/*
 * XCloud360 stage 4 — H.264 RTP jitter buffer + NACK generator, in C with
 * fixed memory (no allocation per packet).
 *
 * Behaviour is a close port of tools/xcloud-capture/rtp/video_jitter.cpp,
 * itself ported from green-nx switch/stream/video_jitter.cpp (GPL-3.0):
 *   - frames keyed by RTP timestamp, kept in timestamp order (max 32);
 *   - a frame is emitted when it has its marker packet, a partition-head
 *     first packet and no sequence gap; RFC 6184 single NAL / STAP-A / FU-A
 *     are depacketized to Annex-B with 4-byte start codes;
 *   - a forward sequence gap of 1..255 packets sends RTCP Generic NACKs
 *     (pid + 16-bit blp bitmask), exactly as the C++ version;
 *   - the front frame is dropped after HOLD_MS (100 ms, "Fase 1") and the
 *     buffer then waits for the next keyframe (want_keyframe = 1);
 *   - until a keyframe passes, assembled non-IDR frames are discarded
 *     (resyncs++) and want_keyframe is raised;
 *   - RFC 3550 receiver statistics for the RTCP RR (report_stats).
 * Differences: packets live in a fixed slot pool (XCJ_POOL_SLOTS); a packet
 * that finds the pool full is dropped and counted (pool_full) — it then looks
 * like loss to the logic above (NACK / hold / keyframe), never corruption.
 */
#ifndef XC_JITTER_H
#define XC_JITTER_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XCJ_MAX_FRAMES      32
#define XCJ_HOLD_MS         100
#define XCJ_MAX_AU_BYTES    (2 * 1024 * 1024)
#define XCJ_SLOT_PAYLOAD    1500
#ifndef XCJ_POOL_SLOTS
#define XCJ_POOL_SLOTS      1024     /* ~1.5 MB: a 3 Mbps IDR is ~100-300 packets */
#endif
#define XCJ_MAX_FRAME_PKTS  1024

typedef struct xcj_stats {
    uint32_t packets, frames, dropped, nacks, resyncs;
    uint32_t last_frame_bytes;
    uint32_t pool_full, late, duplicates;
} xcj_stats_t;

typedef void (*xcj_emit_fn)(void *user, const uint8_t *au, size_t n, uint32_t rtp_ts);
typedef void (*xcj_nack_fn)(void *user, uint16_t pid, uint16_t blp);

typedef struct xcj_frame {
    uint32_t timestamp;
    uint64_t first_seen_ms;
    uint16_t npk;
    uint16_t *pk;          /* slot indices, sorted by sequence (XCJ_MAX_FRAME_PKTS) */
} xcj_frame_t;

typedef struct xcj {
    /* packet pool */
    uint8_t  *slot_data;   /* XCJ_POOL_SLOTS * XCJ_SLOT_PAYLOAD */
    uint16_t *slot_len;
    uint16_t *slot_seq;
    uint8_t  *slot_marker;
    uint16_t *free_list;
    int       nfree;
    /* frames, ring in timestamp order: frames[(head + i) % MAX] */
    xcj_frame_t frames[XCJ_MAX_FRAMES];
    int head, count;
    int have_last_ts;
    uint32_t last_ts;
    int waiting_keyframe;
    /* RFC 3550 stats */
    int have_seq;
    uint16_t max_seq;
    uint32_t cycles, base_seq, received, received_prior, expected_prior;
    xcj_stats_t st;
    uint8_t *au;           /* XCJ_MAX_AU_BYTES assembly buffer */
    uint16_t *pk_block;    /* backing store of frames[].pk */
} xcj_t;

/* Allocates the pool (~1.6 MB + 2 MB AU buffer). 0 = ok. */
int  xcj_init(xcj_t *j);
void xcj_free(xcj_t *j);
void xcj_reset(xcj_t *j);
/* Feed one decrypted RTP packet. emit() gets each completed AU in order (the
 * buffer is reused: copy it before returning). */
void xcj_receive(xcj_t *j, const uint8_t *rtp, size_t len, uint64_t now_ms,
                 xcj_emit_fn emit, xcj_nack_fn nack, void *user, int *want_keyframe);
int  xcj_report_stats(xcj_t *j, uint8_t *fraction_lost, uint32_t *cumulative_lost, uint32_t *highest_seq_ext);
static inline int xcj_waiting_keyframe(const xcj_t *j) { return j->waiting_keyframe; }

#ifdef __cplusplus
}
#endif
#endif
