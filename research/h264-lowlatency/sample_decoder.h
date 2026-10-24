/*
 * XCloud360 — H.264 low-latency receive/decode module (API)
 *
 * Standalone module for receiving H.264 Annex-B over TCP and feeding
 * libavcodec (with --disable-avformat). Cloud-gaming drop-on-overflow
 * policy, ~1-2 frame jitter buffer, no re-buffering.
 *
 * Based on patterns from:
 *   - moonlight-common-c/src/VideoDepacketizer.c (reassembly + drop)
 *   - moonlight-qt/app/streaming/video/ffmpeg.cpp (decoder flags)
 *   - green-nx/src/switch/stream/video_jitter.hpp (kHoldMs + kMaxQueued)
 */

#ifndef XCLOUD_H264_DECODER_H
#define XCLOUD_H264_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Tunable constants (defaults calibrated for LAN wired 720p60) --- */

/* Max frames queued from network thread to decoder thread.
 * Overflow => clear queue + request IDR. green-nx uses 2. */
#define XC_MAX_QUEUED_FRAMES        2

/* Ring buffer size for raw socket bytes (must hold >= 1 IDR worth).
 * 720p60 CBP @ 4.5 Mbps => avg ~9KB/frame, IDR up to ~60KB.
 * 128KB = ~14 P-frames buffered when decoder stalls. */
#define XC_RECV_RING_BYTES          (128 * 1024)

/* Consecutive decode failures before nuclear reset (recreate codec ctx). */
#define XC_CONSECUTIVE_FAILED_LIMIT 5

/* Consecutive frame drops (P-refs with missing reference) before forcing IDR. */
#define XC_CONSECUTIVE_DROP_LIMIT   2

/* --- Frame drop reasons (for stats/logs) --- */

typedef enum {
    XC_DROP_NONE = 0,
    XC_DROP_QUEUE_OVERFLOW,     /* consumer too slow */
    XC_DROP_MISSING_REFERENCE,  /* previous frame lost, this P-frame invalid */
    XC_DROP_WAITING_IDR,        /* no IDR seen yet since (re)start */
    XC_DROP_DECODE_ERROR,       /* avcodec_send_packet returned < 0 */
    XC_DROP_INCOMPLETE_AU,      /* fragment timeout */
} XcDropReason;

/* --- Stats (poll via xc_get_stats) --- */

typedef struct {
    uint64_t bytes_received;
    uint64_t frames_received;      /* complete AUs delivered to decoder */
    uint64_t frames_decoded;       /* successful avcodec_receive_frame */
    uint64_t frames_dropped;
    uint64_t frames_dropped_by[8]; /* indexed by XcDropReason */
    uint64_t idr_requests;
    uint64_t queue_high_watermark;
    /* Rolling latency (microseconds) */
    uint64_t last_recv_to_decode_us;
    uint64_t last_decode_us;
} XcVideoStats;

/* --- Callbacks --- */

/* Called on decoded AVFrame (holds a reference — caller must
 * av_frame_unref() when done). May be called from decoder thread. */
typedef void (*XcFrameCallback)(AVFrame *frame, uint64_t capture_ts_us,
                                void *user);

/* Called when receiver decides an IDR is needed (queue overflow, missing ref,
 * decode error). Application must send IDR request to server over its own
 * signalling channel. If no such channel exists, decoder will simply wait
 * for the next natural IDR from the encoder's GOP cadence. */
typedef void (*XcRequestIdrCallback)(void *user);

/* --- Config --- */

typedef struct {
    int width;                 /* expected, e.g. 1280 */
    int height;                /* expected, e.g. 720 */
    int fps;                   /* expected, e.g. 60 */
    int thread_count;          /* 1 = min latency, 2 = better throughput on Xenon */
    bool allow_output_corrupt; /* true = show partial frames instead of stalling */
    XcFrameCallback on_frame;
    XcRequestIdrCallback on_need_idr;
    void *user;
} XcVideoConfig;

/* --- Lifecycle --- */

/* Init decoder + spawn decoder thread. Returns 0 on success, < 0 on error.
 * Does NOT open any socket — call xc_video_feed_bytes() from your own recv loop. */
int  xc_video_init(const XcVideoConfig *cfg);

/* Feed raw bytes as they arrive from socket. Thread-safe.
 * Framing: expects length-prefixed AU chunks: [uint32_be length][annex-b bytes].
 * If your wire format is raw Annex-B stream, call xc_video_feed_annexb_stream()
 * instead (uses start-code splitter, slightly slower). */
int  xc_video_feed_framed(const uint8_t *data, size_t len);
int  xc_video_feed_annexb_stream(const uint8_t *data, size_t len);

/* Poll stats snapshot (non-blocking). */
void xc_video_get_stats(XcVideoStats *out);

/* Manual IDR request (call after network reconnect etc). Marks current state
 * as waiting-for-IDR — all P-frames until next IDR are dropped. */
void xc_video_mark_want_idr(void);

/* Shutdown decoder thread + free codec ctx. */
void xc_video_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* XCLOUD_H264_DECODER_H */
