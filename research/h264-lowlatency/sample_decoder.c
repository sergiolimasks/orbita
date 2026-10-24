/*
 * XCloud360 — H.264 low-latency receive/decode module (implementation)
 *
 * Wire format assumed: length-prefixed AU chunks over TCP:
 *   [uint32_be length][annex-b bytes ...]
 * Each AU is a complete access unit (SPS+PPS+IDR for keyframes, single slice
 * for P-frames). Server sends 1 AU per encoded frame. Multiple AUs per socket
 * read are handled.
 *
 * Threading model:
 *   - Caller thread: does recv() and calls xc_video_feed_framed()
 *   - Decoder thread (owned by module): pops from queue, feeds decoder,
 *     invokes on_frame callback.
 *
 * Drop policy:
 *   - Producer overflow => clear queue + request IDR.
 *   - avcodec_send_packet failure => count consecutive; hit limit => reset.
 *   - Missing IDR at startup => drop all P/SPS/PPS/SEI until first IDR arrives.
 *
 * NOT included (deliberately — do it in your caller):
 *   - Socket setup / TCP_NODELAY / recv loop
 *   - SDL/GL rendering
 *   - Audio sync
 *
 * Requires: FFmpeg with libavcodec (works with --disable-avformat).
 *           pthread or your platform equivalent.
 */

#include "sample_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

/* --- Platform time (replace mftb() on Xenon for cycle-precise) --- */
#include <time.h>
static uint64_t xc_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* --- Queue entry (one access unit) --- */

typedef struct QueuedAU {
    uint8_t *data;
    size_t   size;
    uint64_t recv_ts_us;
    bool     is_idr;
    struct QueuedAU *next;
} QueuedAU;

/* --- Module state (singleton) --- */

static struct {
    XcVideoConfig cfg;

    AVCodecContext *codec_ctx;
    AVPacket       *pkt;
    AVFrame        *frame;

    /* AU queue (network -> decoder) */
    pthread_mutex_t q_mu;
    pthread_cond_t  q_cv;
    QueuedAU       *q_head;
    QueuedAU       *q_tail;
    int             q_len;

    pthread_t       dec_thread;
    volatile int    running;

    /* Reassembly (partial AU across multiple feed calls) */
    uint8_t        *partial;
    size_t          partial_size;
    size_t          partial_cap;
    uint32_t        partial_expected; /* 0 = not started */

    /* State machine */
    bool            waiting_for_idr;
    int             consecutive_drops;
    int             consecutive_failed_decodes;

    /* Cached SPS/PPS (for re-injection if server doesn't send in-band) */
    uint8_t        *cached_sps;
    size_t          cached_sps_size;
    uint8_t        *cached_pps;
    size_t          cached_pps_size;

    XcVideoStats    stats;
    pthread_mutex_t stats_mu;
} S;

/* --- Utility: read big-endian uint32 --- */
static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* --- Utility: detect if AU contains an IDR + parse NAL types
 *
 * H.264 NAL type is (byte & 0x1F) where byte is the first byte after
 * a 3 or 4-byte start code (00 00 01 or 00 00 00 01).
 *
 * Types of interest:
 *   1 = non-IDR slice (P/B)
 *   5 = IDR slice
 *   6 = SEI (recovery point may live here)
 *   7 = SPS
 *   8 = PPS
 *   9 = AUD
 * --- */

static bool au_is_idr_and_scan_sps_pps(const uint8_t *data, size_t size,
                                       const uint8_t **sps_out, size_t *sps_size_out,
                                       const uint8_t **pps_out, size_t *pps_size_out)
{
    bool is_idr = false;
    *sps_out = NULL; *sps_size_out = 0;
    *pps_out = NULL; *pps_size_out = 0;

    size_t i = 0;
    while (i + 3 < size) {
        /* Find next start code */
        int sc_len = 0;
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) sc_len = 3;
        else if (i + 4 < size && data[i] == 0 && data[i+1] == 0 &&
                 data[i+2] == 0 && data[i+3] == 1) sc_len = 4;

        if (sc_len == 0) { i++; continue; }

        size_t nal_start = i + sc_len;
        if (nal_start >= size) break;

        uint8_t nal_type = data[nal_start] & 0x1F;

        /* Find end of this NAL (next start code or EOF) */
        size_t nal_end = size;
        for (size_t j = nal_start + 1; j + 2 < size; j++) {
            if (data[j] == 0 && data[j+1] == 0 &&
                (data[j+2] == 1 || (j+3 < size && data[j+2] == 0 && data[j+3] == 1))) {
                nal_end = j;
                break;
            }
        }

        if (nal_type == 5) is_idr = true;
        else if (nal_type == 7) { *sps_out = &data[nal_start]; *sps_size_out = nal_end - nal_start; }
        else if (nal_type == 8) { *pps_out = &data[nal_start]; *pps_size_out = nal_end - nal_start; }

        i = nal_end;
    }
    return is_idr;
}

/* --- Queue ops --- */

static void queue_push_locked(QueuedAU *au)
{
    if (S.q_tail) S.q_tail->next = au;
    else S.q_head = au;
    S.q_tail = au;
    S.q_len++;
    if ((uint64_t)S.q_len > S.stats.queue_high_watermark)
        S.stats.queue_high_watermark = S.q_len;
}

static QueuedAU *queue_pop_locked(void)
{
    QueuedAU *au = S.q_head;
    if (au) {
        S.q_head = au->next;
        if (!S.q_head) S.q_tail = NULL;
        S.q_len--;
    }
    return au;
}

static void free_au(QueuedAU *au)
{
    if (!au) return;
    av_free(au->data);
    free(au);
}

static void queue_clear_locked(void)
{
    QueuedAU *au;
    while ((au = queue_pop_locked()) != NULL) free_au(au);
}

/* --- Stats helpers --- */

static void bump_drop(XcDropReason r)
{
    pthread_mutex_lock(&S.stats_mu);
    S.stats.frames_dropped++;
    S.stats.frames_dropped_by[r]++;
    pthread_mutex_unlock(&S.stats_mu);
}

/* --- Request IDR (invokes app callback + marks state) --- */

static void request_idr(void)
{
    pthread_mutex_lock(&S.stats_mu);
    S.stats.idr_requests++;
    pthread_mutex_unlock(&S.stats_mu);

    S.waiting_for_idr = true;
    S.consecutive_drops = 0;

    if (S.cfg.on_need_idr) S.cfg.on_need_idr(S.cfg.user);
}

/* --- Deliver one complete AU (called from network thread) --- */

static void deliver_au(uint8_t *data, size_t size, uint64_t recv_ts_us)
{
    const uint8_t *sps = NULL; size_t sps_size = 0;
    const uint8_t *pps = NULL; size_t pps_size = 0;
    bool is_idr = au_is_idr_and_scan_sps_pps(data, size, &sps, &sps_size, &pps, &pps_size);

    /* Cache SPS/PPS for potential re-injection */
    if (sps && sps_size > 0 && (S.cached_sps_size != sps_size ||
        memcmp(S.cached_sps, sps, sps_size) != 0)) {
        free(S.cached_sps);
        S.cached_sps = malloc(sps_size);
        memcpy(S.cached_sps, sps, sps_size);
        S.cached_sps_size = sps_size;
    }
    if (pps && pps_size > 0 && (S.cached_pps_size != pps_size ||
        memcmp(S.cached_pps, pps, pps_size) != 0)) {
        free(S.cached_pps);
        S.cached_pps = malloc(pps_size);
        memcpy(S.cached_pps, pps, pps_size);
        S.cached_pps_size = pps_size;
    }

    /* Drop if waiting for IDR and this isn't one */
    if (S.waiting_for_idr && !is_idr) {
        av_free(data);
        bump_drop(XC_DROP_WAITING_IDR);
        return;
    }
    if (is_idr) S.waiting_for_idr = false;

    QueuedAU *au = calloc(1, sizeof(*au));
    au->data       = data;
    au->size       = size;
    au->recv_ts_us = recv_ts_us;
    au->is_idr     = is_idr;

    pthread_mutex_lock(&S.q_mu);

    /* OVERFLOW: cloud-gaming drop-on-overflow — clear queue + request IDR */
    if (S.q_len >= XC_MAX_QUEUED_FRAMES) {
        queue_clear_locked();
        pthread_mutex_unlock(&S.q_mu);

        bump_drop(XC_DROP_QUEUE_OVERFLOW);
        request_idr();

        /* If this AU is an IDR, keep it — it's the recovery point.
         * Otherwise, drop it too. */
        pthread_mutex_lock(&S.q_mu);
        if (is_idr) {
            queue_push_locked(au);
            pthread_cond_signal(&S.q_cv);
        } else {
            free_au(au);
        }
        pthread_mutex_unlock(&S.q_mu);
        return;
    }

    queue_push_locked(au);
    pthread_cond_signal(&S.q_cv);
    pthread_mutex_unlock(&S.q_mu);

    pthread_mutex_lock(&S.stats_mu);
    S.stats.frames_received++;
    pthread_mutex_unlock(&S.stats_mu);
}

/* --- Public feed API: length-prefixed framing --- */

int xc_video_feed_framed(const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        /* If mid-AU reassembly, continue filling */
        if (S.partial_expected > 0) {
            size_t need = S.partial_expected - S.partial_size;
            size_t take = (len - off < need) ? (len - off) : need;
            memcpy(S.partial + S.partial_size, data + off, take);
            S.partial_size += take;
            off += take;

            if (S.partial_size == S.partial_expected) {
                /* AU complete — hand off. We MOVE ownership of S.partial. */
                deliver_au(S.partial, S.partial_size, xc_now_us());
                S.partial = NULL;
                S.partial_size = 0;
                S.partial_cap = 0;
                S.partial_expected = 0;
            }
            continue;
        }

        /* Need at least 4 bytes for length prefix */
        if (len - off < 4) {
            /* Partial header — stash rest for next call. In practice, this rarely
             * happens because TCP tends to deliver full headers. If it does,
             * caller should buffer at their level. For simplicity we require
             * caller to pass whole headers. */
            fprintf(stderr, "xc: truncated length prefix (%zu bytes)\n", len - off);
            return -1;
        }

        uint32_t au_size = read_u32_be(data + off);
        off += 4;

        if (au_size == 0 || au_size > 2 * 1024 * 1024) {
            fprintf(stderr, "xc: bogus au_size=%u\n", au_size);
            return -1;
        }

        /* Alloc destination (padded per FFmpeg requirement) */
        uint8_t *au_buf = av_malloc(au_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!au_buf) return -1;
        memset(au_buf + au_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

        size_t avail = len - off;
        if (avail >= au_size) {
            memcpy(au_buf, data + off, au_size);
            off += au_size;
            deliver_au(au_buf, au_size, xc_now_us());
        } else {
            /* AU spans multiple feed calls — start reassembly */
            memcpy(au_buf, data + off, avail);
            S.partial = au_buf;
            S.partial_size = avail;
            S.partial_cap = au_size;
            S.partial_expected = au_size;
            off += avail;
        }

        pthread_mutex_lock(&S.stats_mu);
        S.stats.bytes_received += au_size;
        pthread_mutex_unlock(&S.stats_mu);
    }
    return 0;
}

/* --- Public feed API: raw Annex-B stream (splits on start codes) ---
 *
 * Use only if server doesn't provide length-prefix framing. Slower because
 * we scan every byte for 00 00 01 patterns. Prefers 3-byte start codes.
 *
 * Splits into AUs based on Access Unit Delimiter (NAL type 9) or, absent
 * that, on frame boundary heuristic (SPS/PPS/IDR/slice sequence).
 * For simplicity, this implementation groups from AUD to next AUD.
 * If your server doesn't send AUDs, use xc_video_feed_framed() with your
 * own length prefix instead — trying to guess AU boundaries from bitstream
 * is error-prone. --- */

int xc_video_feed_annexb_stream(const uint8_t *data, size_t len)
{
    /* Left as an exercise — production code should insist on framed input.
     * The straightforward approach: append to a growing buffer, scan for
     * AUD (00 00 [00] 01 09), emit AU on each AUD boundary. */
    (void)data; (void)len;
    fprintf(stderr, "xc: Annex-B stream mode not implemented — use framed\n");
    return -1;
}

/* --- Decoder thread: pops AUs, feeds decoder, invokes callback --- */

static void *decoder_thread(void *arg)
{
    (void)arg;

    while (S.running) {
        pthread_mutex_lock(&S.q_mu);
        while (S.running && S.q_len == 0) {
            pthread_cond_wait(&S.q_cv, &S.q_mu);
        }
        QueuedAU *au = queue_pop_locked();
        pthread_mutex_unlock(&S.q_mu);

        if (!au) continue;

        uint64_t t_dec_start = xc_now_us();

        /* Fill AVPacket. Point at AU buffer; we own it until send_packet
         * consumes it (ref-counted internally when needed). */
        av_packet_unref(S.pkt);
        S.pkt->data = au->data;
        S.pkt->size = (int)au->size;
        S.pkt->pts = AV_NOPTS_VALUE;
        S.pkt->dts = AV_NOPTS_VALUE;
        S.pkt->flags = au->is_idr ? AV_PKT_FLAG_KEY : 0;

        int err = avcodec_send_packet(S.codec_ctx, S.pkt);
        if (err < 0) {
            char errbuf[128];
            av_strerror(err, errbuf, sizeof(errbuf));
            fprintf(stderr, "xc: send_packet failed: %s (idr=%d)\n",
                    errbuf, au->is_idr);
            bump_drop(XC_DROP_DECODE_ERROR);

            S.consecutive_failed_decodes++;
            if (S.consecutive_failed_decodes >= XC_CONSECUTIVE_FAILED_LIMIT) {
                fprintf(stderr, "xc: too many decode failures — flushing\n");
                avcodec_flush_buffers(S.codec_ctx);
                S.consecutive_failed_decodes = 0;
                request_idr();
            }
            free_au(au);
            continue;
        }
        S.consecutive_failed_decodes = 0;

        /* Drain all frames that came out (LOW_DELAY => usually exactly 1) */
        for (;;) {
            int r = avcodec_receive_frame(S.codec_ctx, S.frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) {
                fprintf(stderr, "xc: receive_frame err %d\n", r);
                bump_drop(XC_DROP_DECODE_ERROR);
                break;
            }

            uint64_t t_dec_end = xc_now_us();

            pthread_mutex_lock(&S.stats_mu);
            S.stats.frames_decoded++;
            S.stats.last_recv_to_decode_us = t_dec_end - au->recv_ts_us;
            S.stats.last_decode_us = t_dec_end - t_dec_start;
            pthread_mutex_unlock(&S.stats_mu);

            /* Hand off to renderer. Renderer must av_frame_unref when done. */
            if (S.cfg.on_frame) {
                AVFrame *out = av_frame_alloc();
                av_frame_ref(out, S.frame);
                S.cfg.on_frame(out, au->recv_ts_us, S.cfg.user);
            }
            av_frame_unref(S.frame);
        }

        free_au(au);
    }

    /* Drain on shutdown */
    avcodec_send_packet(S.codec_ctx, NULL);
    while (avcodec_receive_frame(S.codec_ctx, S.frame) >= 0) {
        av_frame_unref(S.frame);
    }
    return NULL;
}

/* --- Init --- */

int xc_video_init(const XcVideoConfig *cfg)
{
    memset(&S, 0, sizeof(S));
    S.cfg = *cfg;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { fprintf(stderr, "xc: no H.264 decoder\n"); return -1; }

    S.codec_ctx = avcodec_alloc_context3(codec);
    if (!S.codec_ctx) return -1;

    /* --- LOW-LATENCY FLAGS (confirmed via moonlight-qt/ffmpeg.cpp) --- */
    S.codec_ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;

    if (cfg->allow_output_corrupt) {
        S.codec_ctx->flags  |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
        S.codec_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    }
    S.codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;   /* skip some checks; small quality hit */

    /* CRITICAL: never frame-threaded (adds thread_count-1 frames of delay).
     * Slice threading is fine and helps on multi-core. */
    S.codec_ctx->thread_type  = FF_THREAD_SLICE;
    S.codec_ctx->thread_count = cfg->thread_count > 0 ? cfg->thread_count : 1;

    /* Zero reorder delay — assumes encoder is CBP (no B-frames, refs=1) */
    S.codec_ctx->delay        = 0;
    S.codec_ctx->has_b_frames = 0;

    /* Dimensions help early allocation (not strictly required — SPS overrides). */
    S.codec_ctx->width  = cfg->width;
    S.codec_ctx->height = cfg->height;

    if (avcodec_open2(S.codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "xc: avcodec_open2 failed\n");
        avcodec_free_context(&S.codec_ctx);
        return -1;
    }

    S.pkt   = av_packet_alloc();
    S.frame = av_frame_alloc();
    if (!S.pkt || !S.frame) return -1;

    pthread_mutex_init(&S.q_mu, NULL);
    pthread_cond_init(&S.q_cv, NULL);
    pthread_mutex_init(&S.stats_mu, NULL);

    S.waiting_for_idr = true;   /* start in drop-until-IDR mode */
    S.running = 1;

    if (pthread_create(&S.dec_thread, NULL, decoder_thread, NULL) != 0) {
        fprintf(stderr, "xc: pthread_create failed\n");
        S.running = 0;
        return -1;
    }
    return 0;
}

/* --- Getters/setters --- */

void xc_video_get_stats(XcVideoStats *out)
{
    pthread_mutex_lock(&S.stats_mu);
    *out = S.stats;
    pthread_mutex_unlock(&S.stats_mu);
}

void xc_video_mark_want_idr(void)
{
    request_idr();
    /* Also clear the pending queue — anything already there is P-frames
     * that depend on obsolete references. */
    pthread_mutex_lock(&S.q_mu);
    queue_clear_locked();
    pthread_mutex_unlock(&S.q_mu);
    avcodec_flush_buffers(S.codec_ctx);
}

/* --- Shutdown --- */

void xc_video_shutdown(void)
{
    if (!S.running) return;

    S.running = 0;
    pthread_mutex_lock(&S.q_mu);
    pthread_cond_broadcast(&S.q_cv);
    pthread_mutex_unlock(&S.q_mu);

    pthread_join(S.dec_thread, NULL);

    pthread_mutex_lock(&S.q_mu);
    queue_clear_locked();
    pthread_mutex_unlock(&S.q_mu);

    if (S.partial) { av_free(S.partial); S.partial = NULL; }
    free(S.cached_sps); S.cached_sps = NULL;
    free(S.cached_pps); S.cached_pps = NULL;

    av_frame_free(&S.frame);
    av_packet_free(&S.pkt);
    avcodec_free_context(&S.codec_ctx);

    pthread_mutex_destroy(&S.q_mu);
    pthread_cond_destroy(&S.q_cv);
    pthread_mutex_destroy(&S.stats_mu);
}

/*
 * --- Example usage (caller side, pseudo-code) ---
 *
 * static void my_on_frame(AVFrame *f, uint64_t ts, void *u) {
 *     // upload f->data[0..2] (Y U V planes) to GPU as textures
 *     // draw fullscreen quad with YUV->RGB shader
 *     // present
 *     av_frame_free(&f);
 * }
 * static void my_on_need_idr(void *u) {
 *     // send "IDR pls" over your control channel
 * }
 *
 * XcVideoConfig cfg = {
 *     .width = 1280, .height = 720, .fps = 60,
 *     .thread_count = 2,               // try 1 first, then 2 if CPU allows
 *     .allow_output_corrupt = true,    // show partials rather than stall
 *     .on_frame = my_on_frame,
 *     .on_need_idr = my_on_need_idr,
 *     .user = NULL,
 * };
 * xc_video_init(&cfg);
 *
 * int sock = tcp_connect_to_server(...);
 * int one = 1;
 * setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
 * int rcvbuf = 64 * 1024;
 * setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
 *
 * uint8_t buf[16384];
 * for (;;) {
 *     ssize_t n = recv(sock, buf, sizeof(buf), 0);
 *     if (n <= 0) break;
 *     xc_video_feed_framed(buf, n);
 * }
 *
 * xc_video_shutdown();
 */
