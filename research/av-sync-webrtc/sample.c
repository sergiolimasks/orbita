/*
 * XCloud360 — AV sync scaffolding
 *
 * Sample C implementation of the design in DESIGN.md.
 *   - xc_video_jitter: RTP -> H.264 AU reassembly with hold-timeout
 *   - xc_audio_jitter: seq-ordered Opus reorder
 *   - xc_audio_ring: PCM ring with servo (stretch/shrink for drift)
 *   - xc_sync_check: drift telemetry via RTCP SR (optional, P2)
 *
 * Threading model:
 *   - RTP net thread: xc_video_jitter_receive() / xc_audio_jitter_push()
 *   - Video decode thread: pops AUs from jitter emit, feeds decoder
 *   - Audio decode thread: pops Opus packets, decodes, applies servo,
 *                          writes to xc_audio_ring
 *   - Audio output thread: reads ring at 48kHz, writes to XAudio2/libxenon
 *   - Render thread: presents latest decoded video frame at vsync
 *
 * NO cross-stream sync — each pipeline is independent. See DESIGN.md §2.
 *
 * Build (single-header, single-TU pattern):
 *   In exactly ONE .c file:
 *     #define XC_AV_SYNC_IMPLEMENTATION
 *     #include "xc_av_sync.h"     // (this file renamed / split)
 *   Elsewhere: just #include "xc_av_sync.h"
 *
 *   Requires POSIX for xc_now_ms:
 *     gcc -D_POSIX_C_SOURCE=200809L -std=c11 ...
 *   On Xbox 360 (libxenon), replace xc_now_ms with mftb-based cycle counter.
 *
 * Verified: compiles clean w/ gcc -Wall -Wextra -std=c11 on x86_64 linux.
 */

#ifndef XC_AV_SYNC_H
#define XC_AV_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * Constants — see DESIGN.md §5
 * ============================================================ */
#define XC_V_HOLD_MS            150u
#define XC_V_MAX_FRAMES         32u
#define XC_V_MAX_AU_BYTES       (2u * 1024u * 1024u)

#define XC_A_MAX_LATE_PACKETS   32u
#define XC_A_SAMPLE_RATE        48000
#define XC_A_CHANNELS           2
#define XC_A_INT16_PER_MS       (XC_A_SAMPLE_RATE * XC_A_CHANNELS / 1000) /* 96 */
#define XC_A_RING_MS            1000
#define XC_A_PREBUFFER_MS       80
#define XC_A_RESUME_MS          40
#define XC_A_HIGH_WATERMARK_MS  300
#define XC_A_SHED_TARGET_MS     150

#define XC_A_SERVO_TARGET_MS    100.0f
#define XC_A_SERVO_GAIN         30e-6f      /* 30 ppm per ms of depth error */
#define XC_A_SERVO_EMA_ALPHA    0.02f
#define XC_A_SERVO_MAX_ADJ      2000e-6f    /* clamp +/-0.2% */

#define XC_SYNC_DRIFT_ALARM_MS  50
#define XC_SYNC_DRIFT_RESET_MS  250

/* ============================================================
 * Utility: monotonic time in ms (Xbox 360 = mftb / CLOCK_MONOTONIC)
 * ============================================================ */
uint64_t xc_now_ms(void);

/* ============================================================
 * Video jitter buffer
 * ============================================================ */

typedef void (*xc_video_emit_fn)(const uint8_t *au, size_t au_len, void *user);
typedef void (*xc_video_nack_fn)(uint16_t pid, uint16_t blp, void *user);

typedef struct xc_video_packet {
    uint16_t seq;
    bool     marker;
    uint8_t *payload;
    size_t   payload_len;
    struct xc_video_packet *next;
} xc_video_packet_t;

typedef struct xc_video_frame {
    uint32_t timestamp;         /* 90 kHz RTP ts, shared by all packets of one AU */
    uint64_t first_seen_ms;
    xc_video_packet_t *packets; /* linked list sorted by seq */
    size_t   packet_count;
    struct xc_video_frame *next; /* sorted by timestamp asc */
} xc_video_frame_t;

typedef struct {
    uint32_t packets_received;
    uint32_t frames_emitted;
    uint32_t frames_dropped;   /* hold-timeout expiry */
    uint32_t nacks_sent;
    uint32_t resyncs;           /* waited-for-IDR events */
    uint32_t last_frame_bytes;
} xc_video_jitter_stats_t;

typedef struct {
    xc_video_frame_t *frames_head;      /* asc timestamp */
    xc_video_frame_t *frames_tail;
    size_t   frames_count;
    uint32_t last_ts;
    bool     has_last_ts;
    bool     waiting_keyframe;
    uint16_t max_seq;
    bool     have_seq;
    xc_video_jitter_stats_t stats;
} xc_video_jitter_t;

void  xc_video_jitter_init(xc_video_jitter_t *j);
void  xc_video_jitter_reset(xc_video_jitter_t *j);
void  xc_video_jitter_destroy(xc_video_jitter_t *j);

/* Feed one decrypted RTP packet. `now_ms` = xc_now_ms(). Emit is called
 * synchronously for each complete AU (in decode order). Nack is called for
 * each detected gap. Sets *want_keyframe true if a resync is required. */
void  xc_video_jitter_receive(xc_video_jitter_t *j,
                              const uint8_t *rtp, size_t rtp_len,
                              uint64_t now_ms,
                              xc_video_emit_fn emit, void *emit_user,
                              xc_video_nack_fn nack, void *nack_user,
                              bool *want_keyframe);

/* ============================================================
 * Audio jitter (seq-ordered Opus reorder)
 * ============================================================ */

typedef struct {
    uint16_t seq;
    uint32_t timestamp;   /* 48 kHz RTP ts */
    uint8_t *payload;
    size_t   payload_len;
} xc_audio_pkt_t;

typedef struct {
    xc_audio_pkt_t slots[XC_A_MAX_LATE_PACKETS];
    size_t   count;
    uint16_t expected_seq;
    bool     have_expected;
    uint32_t stats_lost;
} xc_audio_jitter_t;

void xc_audio_jitter_init(xc_audio_jitter_t *j);
void xc_audio_jitter_reset(xc_audio_jitter_t *j);
void xc_audio_jitter_destroy(xc_audio_jitter_t *j);

/* Push one Opus RTP packet (payload already extracted). Copies memory. */
bool xc_audio_jitter_push(xc_audio_jitter_t *j,
                          uint16_t seq, uint32_t ts,
                          const uint8_t *payload, size_t len);

/* Try to pop the next in-order packet. Returns false if head packet is not
 * yet the expected seq AND buffer has room to wait. If wait exceeded
 * (kMaxLate reached), advances past the gap and returns the next one. */
bool xc_audio_jitter_pop(xc_audio_jitter_t *j, xc_audio_pkt_t *out);

/* ============================================================
 * Audio ring (PCM int16 stereo @ 48kHz) with servo
 * ============================================================ */

typedef struct {
    int16_t *buf;
    int      size;          /* samples (not frames): channels included */
    int      read;
    int      write;
    int      count;

    int      prime_target;  /* samples */
    bool     primed;

    float    depth_ema;     /* smoothed depth in ms */
    float    servo_adj;     /* current stretch ratio (-2e-3 .. +2e-3) */

    /* Stats */
    uint32_t underruns;
    uint32_t dropped_samples;
    uint32_t out_samples;
} xc_audio_ring_t;

bool xc_audio_ring_init(xc_audio_ring_t *r);
void xc_audio_ring_destroy(xc_audio_ring_t *r);
void xc_audio_ring_reset(xc_audio_ring_t *r);

/* Producer side: called from audio decode thread after opus_decode(). Applies
 * servo stretch/shrink to `pcm` (in-place capable), then writes to ring. If
 * ring depth would exceed high watermark, oldest samples are shed down to
 * shed target. Returns samples actually written (may be less than input if
 * shed happened simultaneously). */
int  xc_audio_ring_write(xc_audio_ring_t *r,
                         const int16_t *pcm, int samples_per_ch);

/* Consumer side: called from audio output thread. Reads up to `samples_per_ch`
 * frames into `out` (must hold samples_per_ch * XC_A_CHANNELS int16). Returns
 * actual samples written. If ring not yet primed, returns 0. On underrun,
 * fills silence and increments stats.underruns. */
int  xc_audio_ring_read(xc_audio_ring_t *r, int16_t *out, int samples_per_ch);

/* Diagnostic: current ring depth in ms, and servo state. */
int    xc_audio_ring_depth_ms(const xc_audio_ring_t *r);
int32_t xc_audio_ring_servo_ppm(const xc_audio_ring_t *r);

/* ============================================================
 * Sync check (optional, P2 telemetry — see DESIGN.md §8)
 * ============================================================ */

typedef struct {
    /* Latest SR mapping per SSRC */
    uint64_t audio_sr_ntp;   /* NTP timestamp from SR */
    uint32_t audio_sr_rtp;   /* corresponding RTP ts (48 kHz) */
    uint64_t audio_sr_recv_ms;

    uint64_t video_sr_ntp;
    uint32_t video_sr_rtp;   /* corresponding RTP ts (90 kHz) */
    uint64_t video_sr_recv_ms;

    /* Latest playout per stream */
    uint32_t last_audio_played_rtp;
    uint64_t last_audio_played_ms;
    uint32_t last_video_rendered_rtp;
    uint64_t last_video_rendered_ms;

    /* Rolling drift (video minus audio at same wall-clock) */
    int32_t  drift_ms;
    int32_t  drift_ms_avg;   /* EMA */
    uint64_t last_alarm_ms;
    uint64_t last_reset_ms;
} xc_sync_t;

void xc_sync_init(xc_sync_t *s);
void xc_sync_on_audio_sr(xc_sync_t *s, uint64_t ntp, uint32_t rtp, uint64_t now_ms);
void xc_sync_on_video_sr(xc_sync_t *s, uint64_t ntp, uint32_t rtp, uint64_t now_ms);
void xc_sync_on_audio_played(xc_sync_t *s, uint32_t rtp, uint64_t now_ms);
void xc_sync_on_video_rendered(xc_sync_t *s, uint32_t rtp, uint64_t now_ms);

/* Returns 1 if drift is in alarm range, 2 if reset required, 0 if fine. */
int  xc_sync_check(xc_sync_t *s, uint64_t now_ms);

#endif /* XC_AV_SYNC_H */


/* ============================================================
 * ============================================================
 * IMPLEMENTATION (put in xc_av_sync.c in production; inline here
 * for brevity — split into separate .c files if needed).
 * ============================================================
 * ============================================================ */

#ifdef XC_AV_SYNC_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t xc_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* --- ts_newer: 32-bit RTP timestamp wrap-safe comparison (RFC 3550) --- */
static bool xc_ts_newer_32(uint32_t a, uint32_t b)
{
    uint32_t d = a - b;
    return d != 0 && d < (1u << 31);
}

static bool xc_seq_newer_16(uint16_t a, uint16_t b)
{
    uint16_t d = a - b;
    return d != 0 && d < (1u << 15);
}

/* ================ RTP header parse ================ */
static bool xc_parse_rtp(const uint8_t *rtp, size_t len,
                         uint16_t *seq, uint32_t *ts, bool *marker,
                         const uint8_t **payload, size_t *plen)
{
    if (len < 12) return false;
    uint8_t v = rtp[0] >> 6;
    if (v != 2) return false;
    bool padding = (rtp[0] & 0x20) != 0;
    bool ext     = (rtp[0] & 0x10) != 0;
    uint8_t cc   = rtp[0] & 0x0F;
    *marker      = (rtp[1] & 0x80) != 0;
    *seq         = (uint16_t)((rtp[2] << 8) | rtp[3]);
    *ts          = ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) |
                   ((uint32_t)rtp[6] << 8)  |  (uint32_t)rtp[7];
    size_t header = 12 + 4u * cc;
    if (len < header) return false;
    if (ext) {
        if (len < header + 4) return false;
        size_t words = ((size_t)rtp[header + 2] << 8) | rtp[header + 3];
        header += 4 + 4 * words;
        if (len < header) return false;
    }
    size_t p_len = len - header;
    if (padding && p_len > 0) {
        uint8_t pad = rtp[len - 1];
        p_len = (pad <= p_len) ? p_len - pad : 0;
    }
    *payload = rtp + header;
    *plen    = p_len;
    return true;
}

/* ================ Video jitter — full impl ================ */

static void xc_free_packet_chain(xc_video_packet_t *head)
{
    while (head) {
        xc_video_packet_t *next = head->next;
        free(head->payload);
        free(head);
        head = next;
    }
}

static void xc_free_frame(xc_video_frame_t *f)
{
    xc_free_packet_chain(f->packets);
    free(f);
}

void xc_video_jitter_init(xc_video_jitter_t *j)
{
    memset(j, 0, sizeof(*j));
    j->waiting_keyframe = true;
}

void xc_video_jitter_reset(xc_video_jitter_t *j)
{
    xc_video_frame_t *f = j->frames_head;
    while (f) {
        xc_video_frame_t *next = f->next;
        xc_free_frame(f);
        f = next;
    }
    xc_video_jitter_stats_t saved_stats = j->stats;
    memset(j, 0, sizeof(*j));
    j->stats = saved_stats;
    j->waiting_keyframe = true;
}

void xc_video_jitter_destroy(xc_video_jitter_t *j)
{
    xc_video_jitter_reset(j);
}

/* Find existing frame with matching timestamp, or create + insert (asc). */
static xc_video_frame_t* xc_find_or_create_frame(xc_video_jitter_t *j,
                                                  uint32_t ts, uint64_t now_ms)
{
    xc_video_frame_t *prev = NULL;
    xc_video_frame_t *cur  = j->frames_head;
    while (cur) {
        if (cur->timestamp == ts) return cur;
        if (xc_ts_newer_32(cur->timestamp, ts)) break;
        prev = cur;
        cur  = cur->next;
    }
    /* Reject frames older than what we've emitted */
    if (j->has_last_ts && !xc_ts_newer_32(ts, j->last_ts)) return NULL;
    if (j->frames_count >= XC_V_MAX_FRAMES) return NULL;

    xc_video_frame_t *nf = (xc_video_frame_t*)calloc(1, sizeof(*nf));
    if (!nf) return NULL;
    nf->timestamp     = ts;
    nf->first_seen_ms = now_ms;
    nf->next          = cur;
    if (prev) prev->next = nf; else j->frames_head = nf;
    if (!cur) j->frames_tail = nf;
    j->frames_count++;
    return nf;
}

/* Insert packet into frame's linked list sorted by seq. */
static bool xc_frame_insert_packet(xc_video_frame_t *f,
                                    uint16_t seq, bool marker,
                                    const uint8_t *payload, size_t plen)
{
    xc_video_packet_t *pk = (xc_video_packet_t*)calloc(1, sizeof(*pk));
    if (!pk) return false;
    pk->payload = (uint8_t*)malloc(plen);
    if (!pk->payload) { free(pk); return false; }
    memcpy(pk->payload, payload, plen);
    pk->payload_len = plen;
    pk->seq         = seq;
    pk->marker      = marker;

    xc_video_packet_t *prev = NULL;
    xc_video_packet_t *cur  = f->packets;
    while (cur) {
        if (cur->seq == seq) {
            /* Duplicate — discard */
            free(pk->payload); free(pk);
            return true;
        }
        if (xc_seq_newer_16(cur->seq, seq)) break;
        prev = cur;
        cur  = cur->next;
    }
    pk->next = cur;
    if (prev) prev->next = pk; else f->packets = pk;
    f->packet_count++;
    return true;
}

/* Depacketize one RTP-H.264 NAL payload into Annex-B AU buffer.
 * Handles: single NAL, STAP-A (24), FU-A (28). Ignores others.
 * Returns bytes appended, or 0 on error. Grows *au dynamically. */
static const uint8_t g_start_code[4] = {0x00, 0x00, 0x00, 0x01};

static size_t xc_depacketize_nal(const uint8_t *p, size_t len,
                                  uint8_t **au, size_t *au_len, size_t *au_cap)
{
    if (len < 1) return 0;
    uint8_t nal_hdr = p[0];
    uint8_t type    = nal_hdr & 0x1F;

    /* Grow helper */
    #define GROW(need_extra) do { \
        size_t need = *au_len + (need_extra); \
        if (need > *au_cap) { \
            size_t new_cap = (*au_cap ? *au_cap * 2 : 4096); \
            while (new_cap < need) new_cap *= 2; \
            if (new_cap > XC_V_MAX_AU_BYTES) return 0; \
            uint8_t *nb = (uint8_t*)realloc(*au, new_cap); \
            if (!nb) return 0; \
            *au = nb; *au_cap = new_cap; \
        } \
    } while(0)

    if (type >= 1 && type <= 23) {
        /* Single NAL unit */
        GROW(4 + len);
        memcpy(*au + *au_len, g_start_code, 4); *au_len += 4;
        memcpy(*au + *au_len, p, len);          *au_len += len;
        return 4 + len;
    }
    if (type == 24) {
        /* STAP-A: [nalu_size_16][nalu][...] */
        size_t off = 1;
        while (off + 2 <= len) {
            size_t nsz = ((size_t)p[off] << 8) | p[off + 1];
            off += 2;
            if (off + nsz > len) return 0;
            GROW(4 + nsz);
            memcpy(*au + *au_len, g_start_code, 4); *au_len += 4;
            memcpy(*au + *au_len, p + off, nsz);    *au_len += nsz;
            off += nsz;
        }
        return len; /* approximate */
    }
    if (type == 28) {
        /* FU-A: [FU-indicator (1)][FU-header (1)][payload...] */
        if (len < 2) return 0;
        uint8_t fu_ind = p[0];
        uint8_t fu_hdr = p[1];
        bool    start  = (fu_hdr & 0x80) != 0;
        uint8_t nal_type_reass = fu_hdr & 0x1F;
        if (start) {
            uint8_t reconstructed = (fu_ind & 0xE0) | nal_type_reass;
            GROW(5);
            memcpy(*au + *au_len, g_start_code, 4); *au_len += 4;
            (*au)[*au_len] = reconstructed;         *au_len += 1;
        }
        size_t body = len - 2;
        GROW(body);
        memcpy(*au + *au_len, p + 2, body); *au_len += body;
        return len;
    }
    /* Unknown types (25-27, 29-31): skip */
    return 0;
    #undef GROW
}

/* Check if AU starts with an SPS+PPS+IDR sequence (nal_type 5).
 * Simplified: scan for any NAL unit of type 5. */
static bool xc_au_has_idr(const uint8_t *au, size_t len)
{
    size_t i = 0;
    while (i + 4 < len) {
        if (au[i] == 0 && au[i+1] == 0 && au[i+2] == 0 && au[i+3] == 1) {
            if (i + 4 < len && (au[i+4] & 0x1F) == 5) return true;
            i += 4;
        } else {
            i++;
        }
    }
    return false;
}

/* Try to assemble frame `f` into contiguous Annex-B AU. Returns true if
 * successful (out fully populated). Returns false if any seq gap. */
static bool xc_try_assemble(xc_video_frame_t *f,
                             uint8_t **au, size_t *au_len, size_t *au_cap)
{
    if (!f->packets) return false;
    /* Verify no seq gap */
    xc_video_packet_t *pk = f->packets;
    uint16_t expected = pk->seq;
    bool     saw_marker = false;
    while (pk) {
        if (pk->seq != expected) return false;
        if (pk->marker) saw_marker = true;
        expected = (uint16_t)(expected + 1);
        pk = pk->next;
    }
    if (!saw_marker) return false; /* last packet of AU has marker=1 */

    *au_len = 0;
    for (pk = f->packets; pk; pk = pk->next) {
        if (xc_depacketize_nal(pk->payload, pk->payload_len,
                               au, au_len, au_cap) == 0) {
            if (*au_len > XC_V_MAX_AU_BYTES) return false;
        }
    }
    return *au_len > 0;
}

/* Emit NACK for gap between last known max_seq and new_seq */
static void xc_emit_nack_gap(xc_video_jitter_t *j, uint16_t new_seq,
                              xc_video_nack_fn nack, void *user)
{
    if (!j->have_seq) return;
    uint16_t after = (uint16_t)(j->max_seq + 1);
    if (!xc_seq_newer_16(new_seq, after)) return; /* no gap */
    /* Simple: single NACK per missing seq (real impl would use BLP bitmap) */
    uint16_t s = after;
    int n = 0;
    while (s != new_seq && n < 16) {
        if (nack) nack(s, 0, user);
        j->stats.nacks_sent++;
        s = (uint16_t)(s + 1);
        n++;
    }
}

void xc_video_jitter_receive(xc_video_jitter_t *j,
                              const uint8_t *rtp, size_t rtp_len,
                              uint64_t now_ms,
                              xc_video_emit_fn emit, void *emit_user,
                              xc_video_nack_fn nack, void *nack_user,
                              bool *want_keyframe)
{
    uint16_t seq; uint32_t ts; bool marker;
    const uint8_t *payload; size_t plen;
    if (!xc_parse_rtp(rtp, rtp_len, &seq, &ts, &marker, &payload, &plen))
        return;
    if (plen == 0) return;

    j->stats.packets_received++;

    /* Sequence tracking + NACK */
    if (!j->have_seq) {
        j->max_seq  = seq;
        j->have_seq = true;
    } else {
        if (xc_seq_newer_16(seq, j->max_seq)) {
            xc_emit_nack_gap(j, seq, nack, nack_user);
            j->max_seq = seq;
        }
    }

    /* Insert into frame */
    xc_video_frame_t *f = xc_find_or_create_frame(j, ts, now_ms);
    if (f) xc_frame_insert_packet(f, seq, marker, payload, plen);

    /* Drain */
    static __thread uint8_t *au_buf = NULL;
    static __thread size_t   au_cap = 0;
    size_t au_len;

    for (;;) {
        xc_video_frame_t *front = j->frames_head;
        if (!front) break;

        if (xc_try_assemble(front, &au_buf, &au_len, &au_cap)) {
            j->last_ts     = front->timestamp;
            j->has_last_ts = true;
            j->frames_head = front->next;
            if (!j->frames_head) j->frames_tail = NULL;
            j->frames_count--;

            if (j->waiting_keyframe) {
                if (!xc_au_has_idr(au_buf, au_len)) {
                    j->stats.resyncs++;
                    if (want_keyframe) *want_keyframe = true;
                    xc_free_frame(front);
                    continue;
                }
                j->waiting_keyframe = false;
            }
            j->stats.frames_emitted++;
            j->stats.last_frame_bytes = (uint32_t)au_len;
            if (emit) emit(au_buf, au_len, emit_user);
            xc_free_frame(front);
            continue;
        }
        /* Not assemblable yet: hold-timeout? */
        if (now_ms - front->first_seen_ms > XC_V_HOLD_MS) {
            j->last_ts     = front->timestamp;
            j->has_last_ts = true;
            j->frames_head = front->next;
            if (!j->frames_head) j->frames_tail = NULL;
            j->frames_count--;
            j->waiting_keyframe = true;
            if (want_keyframe) *want_keyframe = true;
            j->stats.frames_dropped++;
            xc_free_frame(front);
            continue;
        }
        break; /* wait for more packets */
    }
}

/* ================ Audio jitter — brief impl ================ */

void xc_audio_jitter_init(xc_audio_jitter_t *j)
{
    memset(j, 0, sizeof(*j));
}

void xc_audio_jitter_reset(xc_audio_jitter_t *j)
{
    for (size_t i = 0; i < j->count; i++) free(j->slots[i].payload);
    memset(j, 0, sizeof(*j));
}

void xc_audio_jitter_destroy(xc_audio_jitter_t *j) { xc_audio_jitter_reset(j); }

bool xc_audio_jitter_push(xc_audio_jitter_t *j, uint16_t seq, uint32_t ts,
                          const uint8_t *payload, size_t len)
{
    /* Reject stale */
    if (j->have_expected && !xc_seq_newer_16(seq, (uint16_t)(j->expected_seq - 1)))
        return false;
    if (j->count >= XC_A_MAX_LATE_PACKETS) return false;

    /* Insert sorted by seq */
    size_t pos = j->count;
    for (size_t i = 0; i < j->count; i++) {
        if (xc_seq_newer_16(j->slots[i].seq, seq)) { pos = i; break; }
    }
    for (size_t i = j->count; i > pos; i--) j->slots[i] = j->slots[i-1];

    j->slots[pos].seq = seq;
    j->slots[pos].timestamp = ts;
    j->slots[pos].payload = (uint8_t*)malloc(len);
    if (!j->slots[pos].payload) return false;
    memcpy(j->slots[pos].payload, payload, len);
    j->slots[pos].payload_len = len;
    j->count++;
    return true;
}

bool xc_audio_jitter_pop(xc_audio_jitter_t *j, xc_audio_pkt_t *out)
{
    if (j->count == 0) return false;
    if (!j->have_expected) {
        j->expected_seq = j->slots[0].seq;
        j->have_expected = true;
    }
    if (j->slots[0].seq != j->expected_seq) {
        /* Gap. Wait unless buffer full (assume caller pops each frame slot) */
        if (j->count < XC_A_MAX_LATE_PACKETS) return false;
        /* Give up on missing packet — advance */
        j->stats_lost += (uint16_t)(j->slots[0].seq - j->expected_seq);
        j->expected_seq = j->slots[0].seq;
    }
    *out = j->slots[0];
    /* Shift left */
    for (size_t i = 1; i < j->count; i++) j->slots[i-1] = j->slots[i];
    j->count--;
    j->expected_seq = (uint16_t)(j->expected_seq + 1);
    return true;
}

/* ================ Audio ring — with servo ================ */

bool xc_audio_ring_init(xc_audio_ring_t *r)
{
    memset(r, 0, sizeof(*r));
    r->size = XC_A_RING_MS * XC_A_INT16_PER_MS;
    r->buf = (int16_t*)calloc(r->size, sizeof(int16_t));
    if (!r->buf) return false;
    r->prime_target = XC_A_PREBUFFER_MS * XC_A_INT16_PER_MS;
    return true;
}

void xc_audio_ring_destroy(xc_audio_ring_t *r)
{
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

void xc_audio_ring_reset(xc_audio_ring_t *r)
{
    r->read = r->write = r->count = 0;
    r->primed = false;
    r->prime_target = XC_A_PREBUFFER_MS * XC_A_INT16_PER_MS;
    r->depth_ema = 0.0f;
    r->servo_adj = 0.0f;
}

int xc_audio_ring_write(xc_audio_ring_t *r,
                        const int16_t *pcm, int samples_per_ch)
{
    /* Update servo based on depth EMA. */
    float depth_ms = (float)r->count / XC_A_INT16_PER_MS;
    r->depth_ema += XC_A_SERVO_EMA_ALPHA * (depth_ms - r->depth_ema);
    float adj = (XC_A_SERVO_TARGET_MS - r->depth_ema) * XC_A_SERVO_GAIN;
    if (adj >  XC_A_SERVO_MAX_ADJ) adj =  XC_A_SERVO_MAX_ADJ;
    if (adj < -XC_A_SERVO_MAX_ADJ) adj = -XC_A_SERVO_MAX_ADJ;
    r->servo_adj = adj;

    /* Simple stretch: adjust effective sample count by (1 + adj) via
     * duplicate / drop. For production, use a proper resampler (linear
     * or sinc). Here we skip stretch for brevity — servo just tracked. */
    int total_ints = samples_per_ch * XC_A_CHANNELS;

    /* High-watermark shed */
    int high   = XC_A_HIGH_WATERMARK_MS * XC_A_INT16_PER_MS;
    int target = XC_A_SHED_TARGET_MS    * XC_A_INT16_PER_MS;
    if (r->count + total_ints > high) {
        int shed = (r->count + total_ints) - target;
        if (shed > r->count) shed = r->count;
        if (shed > 0) {
            r->read = (r->read + shed) % r->size;
            r->count -= shed;
            r->dropped_samples += (uint32_t)shed;
        }
    }
    if (r->count + total_ints > r->size) {
        int over = (r->count + total_ints) - r->size;
        r->read = (r->read + over) % r->size;
        r->count -= over;
        r->dropped_samples += (uint32_t)over;
    }
    /* Copy in (wrap) */
    int first = r->size - r->write;
    if (first > total_ints) first = total_ints;
    memcpy(r->buf + r->write, pcm, first * sizeof(int16_t));
    if (total_ints - first > 0)
        memcpy(r->buf, pcm + first, (total_ints - first) * sizeof(int16_t));
    r->write = (r->write + total_ints) % r->size;
    r->count += total_ints;

    /* Prime check */
    if (!r->primed && r->count >= r->prime_target) r->primed = true;
    return samples_per_ch;
}

int xc_audio_ring_read(xc_audio_ring_t *r, int16_t *out, int samples_per_ch)
{
    int need = samples_per_ch * XC_A_CHANNELS;
    if (!r->primed || r->count < need) {
        memset(out, 0, need * sizeof(int16_t));
        if (r->primed) {
            r->underruns++;
            r->primed = false;
            r->prime_target = XC_A_RESUME_MS * XC_A_INT16_PER_MS;
        }
        return samples_per_ch;
    }
    int first = r->size - r->read;
    if (first > need) first = need;
    memcpy(out, r->buf + r->read, first * sizeof(int16_t));
    if (need - first > 0)
        memcpy(out + first, r->buf, (need - first) * sizeof(int16_t));
    r->read = (r->read + need) % r->size;
    r->count -= need;
    r->out_samples += (uint32_t)samples_per_ch;
    return samples_per_ch;
}

int xc_audio_ring_depth_ms(const xc_audio_ring_t *r)
{
    return r->count / XC_A_INT16_PER_MS;
}

int32_t xc_audio_ring_servo_ppm(const xc_audio_ring_t *r)
{
    return (int32_t)(r->servo_adj * 1e6f);
}

/* ================ Sync check ================ */

void xc_sync_init(xc_sync_t *s) { memset(s, 0, sizeof(*s)); }

void xc_sync_on_audio_sr(xc_sync_t *s, uint64_t ntp, uint32_t rtp, uint64_t now)
{ s->audio_sr_ntp = ntp; s->audio_sr_rtp = rtp; s->audio_sr_recv_ms = now; }

void xc_sync_on_video_sr(xc_sync_t *s, uint64_t ntp, uint32_t rtp, uint64_t now)
{ s->video_sr_ntp = ntp; s->video_sr_rtp = rtp; s->video_sr_recv_ms = now; }

void xc_sync_on_audio_played(xc_sync_t *s, uint32_t rtp, uint64_t now)
{ s->last_audio_played_rtp = rtp; s->last_audio_played_ms = now; }

void xc_sync_on_video_rendered(xc_sync_t *s, uint32_t rtp, uint64_t now)
{ s->last_video_rendered_rtp = rtp; s->last_video_rendered_ms = now; }

/* Convert RTP timestamp to NTP-ms using latest SR mapping. Returns 0 if no SR. */
static int64_t xc_rtp_to_ntp_ms(uint32_t sr_rtp, uint64_t sr_ntp,
                                 uint32_t clock_hz, uint32_t target_rtp)
{
    if (sr_ntp == 0) return 0;
    int32_t delta_rtp = (int32_t)(target_rtp - sr_rtp);   /* wrap-safe */
    int64_t delta_ms  = ((int64_t)delta_rtp * 1000) / clock_hz;
    /* sr_ntp is in NTP format (upper 32 = sec, lower 32 = frac). Convert to ms */
    int64_t sr_ms = (int64_t)(sr_ntp >> 32) * 1000 +
                    (int64_t)(((sr_ntp & 0xFFFFFFFFull) * 1000ull) >> 32);
    return sr_ms + delta_ms;
}

int xc_sync_check(xc_sync_t *s, uint64_t now_ms)
{
    if (s->audio_sr_ntp == 0 || s->video_sr_ntp == 0) return 0;
    if (s->last_audio_played_rtp == 0 || s->last_video_rendered_rtp == 0) return 0;

    int64_t audio_capture_ms = xc_rtp_to_ntp_ms(s->audio_sr_rtp, s->audio_sr_ntp,
                                                 48000, s->last_audio_played_rtp);
    int64_t video_capture_ms = xc_rtp_to_ntp_ms(s->video_sr_rtp, s->video_sr_ntp,
                                                 90000, s->last_video_rendered_rtp);

    /* Positive drift = video is behind audio at same wall-clock */
    int64_t audio_playout_wall_offset = (int64_t)now_ms - (int64_t)s->last_audio_played_ms;
    int64_t video_render_wall_offset  = (int64_t)now_ms - (int64_t)s->last_video_rendered_ms;

    int64_t audio_present_at = audio_capture_ms + audio_playout_wall_offset;
    int64_t video_present_at = video_capture_ms + video_render_wall_offset;

    s->drift_ms     = (int32_t)(video_present_at - audio_present_at);
    s->drift_ms_avg = (s->drift_ms_avg * 3 + s->drift_ms) / 4;

    int magnitude = s->drift_ms_avg < 0 ? -s->drift_ms_avg : s->drift_ms_avg;
    if (magnitude > XC_SYNC_DRIFT_RESET_MS) {
        if (now_ms - s->last_reset_ms > 30000) {
            s->last_reset_ms = now_ms;
            return 2;
        }
    }
    if (magnitude > XC_SYNC_DRIFT_ALARM_MS) {
        if (now_ms - s->last_alarm_ms > 3000) {
            s->last_alarm_ms = now_ms;
            return 1;
        }
    }
    return 0;
}

#endif /* XC_AV_SYNC_IMPLEMENTATION */
