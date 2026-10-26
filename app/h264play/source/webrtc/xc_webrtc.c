/*
 * XCloud360 stage 4 — see xc_webrtc.h.
 *
 * Mapping to tools/xcloud-capture/rtp/capture-rtp.cpp (kept 1:1 on purpose,
 * each block says which part it mirrors):
 *   on_video_track / jitter emit        -> on_video_track + TcpOutServer::push_au
 *   request_keyframe_throttled          -> request_keyframe_throttled
 *   poll_timers (RR+REMB, kf flags)     -> pc_thread body
 *   input_step                          -> input_send_loop
 *   on_channel_open / on_channel_message-> same names
 *   munge_offer                         -> the two SDP edits in main()
 * The xCloud JSON/binary messages come from xc_proto.c (port of green-nx
 * xcloud_protocol.cpp, GPL-3.0).
 */
#include "xc_webrtc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "peer.h"
#include "peer_connection.h"
#include "xc_jitter.h"
#include "xcw_port.h"

#define XCW_MAX_CANDS 8
#define XCW_OFFER_CAP 12288
#define XCW_PARAM_CAP 256

static const struct {
    const char *label, *proto;
    uint16_t sid;
} k_chans[XCW_CH_COUNT] = {
    {"control", "controlV1", 0},
    {"input", "1.0", 2},
    {"message", "messageV1", 4},
    {"chat", "chatV1", 6},
};

struct xcw {
    xcw_cfg_t cfg;
    char stun_url[160];
    char alias[16];
    PeerConnection *pc;

    char *offer;
    char ufrag[64];
    char cands[XCW_MAX_CANDS][192];
    int ncands;

    xcj_t jit;
    uint8_t *aubuf;               /* AU + prepended SPS/PPS */

    xcw_state_t st;
    const char *fail_reason;
    int answer_set, channels_opened, handshake_done, sctp_closed;
    uint64_t t_answer, t_ice_abs, last_step_ms, last_sctp_ms, last_kf_ms, last_rr_ms, last_consent_ms;

    /* any-thread flags */
    volatile int kf_wanted, kf_force, resync_req, video_kbps;

    /* gamepads (stage 5: up to XCW_MAX_PADS, like capture-rtp kMaxPads):
     * one seqlock per index, single writer per index. present = indices
     * that ever got a frame (pad 0 always: player 1 heartbeat); announced =
     * told to xCloud with gamepadChanged (0 goes in the handshake). */
    volatile uint32_t pad_seq[XCW_MAX_PADS];
    xcp_gamepad_t pad_shared[XCW_MAX_PADS];
    volatile uint32_t pad_present;
    uint32_t pad_announced;

    /* input sender (input_send_loop) */
    int input_run;
    xcp_input_ser_t ser;
    xcp_gamepad_t last_sent[XCW_MAX_PADS];
    uint64_t last_send_ms_pad[XCW_MAX_PADS];
    uint64_t t_input_start_us, last_send_ms, next_input_ms;
    int consec_fails;
    int logged_first_press;

    /* AU gating / parameter-set cache (TcpOutServer::push_au) */
    int wait_idr;
    int video_paused;   /* v0.19.3: xcw_set_video_paused */
    uint8_t sps[XCW_PARAM_CAP], pps[XCW_PARAM_CAP];
    size_t sps_n, pps_n;
    int log_sps_pending, log_pps_pending;

    /* stage 5: RTT estimate = NACK sent -> that sequence number arrives */
    struct { uint16_t seq; uint8_t used; uint64_t t_us; } nack_t[16];
    int nack_w;

    xcw_stats_t s;
};

static void xlog(xcw_t *x, const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (x && x->cfg.on_log)
        x->cfg.on_log(x->cfg.user, line);
    else
        xcw_log(line);
}

static void set_state(xcw_t *x, xcw_state_t st, const char *reason) {
    if (x->st == st) return;
    if (x->st == XCW_FAILED || x->st == XCW_CLOSED) return;   /* terminal */
    x->st = st;
    if (reason) x->fail_reason = reason;
    xlog(x, "[xcw] state -> %s%s%s", xcw_state_name(st), reason ? " : " : "", reason ? reason : "");
    if (x->cfg.on_state) x->cfg.on_state(x->cfg.user, st);
}

static uint32_t since_answer(xcw_t *x, uint64_t now) {
    if (!x->t_answer) return 0;
    uint32_t v = (uint32_t)(now - x->t_answer);
    return v ? v : 1;
}

const char *xcw_state_name(xcw_state_t st) {
    switch (st) {
        case XCW_NEW: return "new";
        case XCW_CHECKING: return "checking";
        case XCW_ICE_CONNECTED: return "ice-connected";
        case XCW_DTLS_CONNECTED: return "dtls-connected";
        case XCW_CHANNELS_OPEN: return "channels-open";
        case XCW_STREAMING: return "streaming";
        case XCW_FAILED: return "failed";
        case XCW_CLOSED: return "closed";
    }
    return "?";
}

/* ------------------------------------------------------- datachannels */

static xcw_channel_t channel_of_label(const char *label) {
    if (!label) return XCW_CH_UNKNOWN;
    for (int i = 0; i < XCW_CH_COUNT; i++)
        if (strcmp(label, k_chans[i].label) == 0) return (xcw_channel_t)i;
    return XCW_CH_UNKNOWN;
}

int xcw_send(xcw_t *x, xcw_channel_t ch, const void *data, size_t n, int text) {
    if (!x->pc || ch < 0 || ch >= XCW_CH_COUNT) return -1;
    uint16_t sid = 0;
    if (peer_connection_lookup_sid(x->pc, k_chans[ch].label, &sid) != 0) {
        xlog(x, "[dc] send FAILED: no channel '%s'", k_chans[ch].label);
        x->s.dc_tx_fail++;
        return -1;
    }
    int rc = text ? peer_connection_datachannel_send_text_sid(x->pc, (char *)data, n, sid)
                  : peer_connection_datachannel_send_sid(x->pc, (char *)data, n, sid);
    if (rc < 0) {
        x->s.dc_tx_fail++;
    } else {
        x->s.dc_tx[ch]++;
    }
    /* Input goes out at up to 250 Hz: only log the other channels. */
    if (ch != XCW_CH_INPUT) {
        if (text)
            xlog(x, "[dc] send [%s sid=%u len=%u] %.180s%s", k_chans[ch].label, sid, (unsigned)n,
                 (const char *)data, n > 180 ? "..." : "");
        else
            xlog(x, "[dc] send-bin [%s sid=%u len=%u] rc=%d", k_chans[ch].label, sid, (unsigned)n, rc);
    } else if (rc < 0) {
        xlog(x, "[dc] send-bin [input sid=%u len=%u] rc=%d", sid, (unsigned)n, rc);
    }
    return rc < 0 ? -1 : 0;
}

static int send_text(xcw_t *x, xcw_channel_t ch, const char *s) { return xcw_send(x, ch, s, strlen(s), 1); }

/* capture-rtp request_keyframe_throttled(): PLI + "videoKeyframeRequested",
 * at most once per second unless forced. */
static void request_keyframe_throttled(xcw_t *x, int force) {
    if (!x->pc || !x->handshake_done) return;
    uint64_t now = xcw_now_ms();
    if (!force && now - x->last_kf_ms < 1000) return;
    x->last_kf_ms = now;
    if (peer_connection_request_keyframe(x->pc) >= 0) x->s.pli_sent++;
    send_text(x, XCW_CH_CONTROL, xcp_video_keyframe_requested());
    x->s.keyframe_requests++;
}

static void on_channel_open(void *user) {
    xcw_t *x = (xcw_t *)user;
    if (x->channels_opened) return;
    x->channels_opened = 1;
    x->s.t_sctp_ms = since_answer(x, xcw_now_ms());
    xlog(x, "[dc] SCTP up — creating data channels + sending handshake");
    /* libpeer only maps label->sid once the DCEP OPEN went out over SCTP, so
     * channels are created here, not at setup time (green-nx does the same). */
    for (int i = 0; i < XCW_CH_COUNT; i++)
        peer_connection_create_datachannel_sid(x->pc, DATA_CHANNEL_RELIABLE, 0, 0, (char *)k_chans[i].label,
                                               (char *)k_chans[i].proto, k_chans[i].sid);
    set_state(x, XCW_CHANNELS_OPEN, NULL);
    send_text(x, XCW_CH_MESSAGE, xcp_message_handshake());
}

static void on_channel_close(void *user) {
    xcw_t *x = (xcw_t *)user;
    x->sctp_closed = 1;
    set_state(x, XCW_CLOSED, "SCTP association closed");
}

static void startup_sequence(xcw_t *x) {
    char msg[1024];
    xlog(x, "[dc] handshakeAck received — running startup sequence");
    x->handshake_done = 1;
    x->s.t_handshake_ms = since_answer(x, xcw_now_ms());
    /* Authorize control + announce a plugged-in gamepad (mandatory). */
    send_text(x, XCW_CH_CONTROL, xcp_authorization_request());
    send_text(x, XCW_CH_CONTROL, xcp_gamepad_changed(0, 1));
    send_text(x, XCW_CH_CONTROL, xcp_user_requested_resolution(x->alias));
    /* ClientDeviceCapabilities + quality knobs: this makes xCloud start RTP. */
    for (int i = 0; i < XCP_STARTUP_MESSAGES; i++) {
        if (xcp_startup_message(i, x->cfg.video_w, x->cfg.video_h, x->video_kbps, x->cfg.video_fps, msg,
                                sizeof(msg)) > 0)
            send_text(x, XCW_CH_MESSAGE, msg);
    }
    /* Empty client_metadata so the server marks the input channel open.
     * Like capture-rtp, from a FRESH serializer (sequence 1); the gamepad
     * stream below then also starts at sequence 1. */
    {
        xcp_input_ser_t fresh = {0};
        uint8_t pkt[XCP_CLIENT_METADATA_LEN];
        size_t n = xcp_client_metadata(&fresh, 0, pkt);
        xcw_send(x, XCW_CH_INPUT, pkt, n, 0);
    }
    /* input stream (start_input_threads) */
    x->input_run = 1;
    x->ser.sequence = 0;
    memset(x->last_sent, 0, sizeof(x->last_sent));
    x->pad_announced = 1;   /* gamepadChanged(0) just went out above */
    x->t_input_start_us = xcw_now_us();
    x->last_send_ms = xcw_now_ms();
    for (int i = 0; i < XCW_MAX_PADS; i++) x->last_send_ms_pad[i] = x->last_send_ms;
    x->next_input_ms = 0;
    x->consec_fails = 0;
    xlog(x, "[input] on-change (>=4 ms) + 50 ms heartbeat started");
    /* Ask for an IDR right away (else the next periodic one, 5-10 s). */
    send_text(x, XCW_CH_CONTROL, xcp_video_keyframe_requested());
    if (peer_connection_request_keyframe(x->pc) >= 0) x->s.pli_sent++;
    set_state(x, XCW_STREAMING, NULL);
    xlog(x, "[dc] startup sequence sent, awaiting first video NAL");
}

static void on_channel_message(char *data, size_t size, void *user, uint16_t sid) {
    xcw_t *x = (xcw_t *)user;
    if (!x->pc) return;
    char *label = peer_connection_lookup_sid_label(x->pc, sid);
    xcw_channel_t ch = channel_of_label(label);
    if (ch != XCW_CH_INPUT)
        xlog(x, "[dc] recv [%s sid=%u len=%u] %.*s", label ? label : "sid?", sid, (unsigned)size,
             (int)(size < 240 ? size : 240), data);
    if (ch >= 0) x->s.dc_rx[ch]++;
    if (ch == XCW_CH_MESSAGE && !x->handshake_done && xcp_is_handshake_ack(data, size)) startup_sequence(x);
    if (x->cfg.on_channel_msg) x->cfg.on_channel_msg(x->cfg.user, ch, (const uint8_t *)data, size);
}

/* ---------------------------------------------------------------- video */

static void log_sps(xcw_t *x, const uint8_t *nal, size_t n) {
    /* profile_idc/constraint/level are bytes 1..3 of the RBSP; emulation
     * prevention cannot occur that early. */
    if (n < 4) {
        xlog(x, "[h264] SPS too short (%u bytes)", (unsigned)n);
        return;
    }
    xlog(x, "[h264] SPS profile_idc=%u constraint=0x%02x level_idc=%u (%u bytes)", nal[1], nal[2], nal[3],
         (unsigned)n);
}

typedef struct {
    const uint8_t *b;
    size_t nbits, pos;
    int err;
} bitrd_t;

static unsigned rd_bit(bitrd_t *r) {
    if (r->pos >= r->nbits) {
        r->err = 1;
        return 0;
    }
    unsigned v = (r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
    r->pos++;
    return v;
}

static uint32_t rd_ue(bitrd_t *r) {
    int lz = 0;
    while (!r->err && rd_bit(r) == 0)
        if (++lz > 31) {
            r->err = 1;
            return 0;
        }
    if (r->err) return 0;
    uint32_t v = 0;
    for (int k = 0; k < lz; k++) v = (v << 1) | rd_bit(r);
    return (uint32_t)(((1ull << lz) - 1) + v);
}

static void log_pps(xcw_t *x, const uint8_t *nal, size_t n) {
    uint8_t r[32];
    size_t rn = 0, zeros = 0;
    for (size_t k = 1; k < n && rn < sizeof(r); k++) {   /* RBSP, skip header */
        if (zeros >= 2 && nal[k] == 0x03) {
            zeros = 0;
            continue;
        }
        r[rn++] = nal[k];
        zeros = nal[k] == 0 ? zeros + 1 : 0;
    }
    bitrd_t br = {r, rn * 8, 0, 0};
    uint32_t pps_id = rd_ue(&br);
    uint32_t sps_id = rd_ue(&br);
    unsigned ecm = rd_bit(&br);
    if (br.err) {
        xlog(x, "[h264] PPS parse error (%u bytes)", (unsigned)n);
        return;
    }
    xlog(x, "[h264] PPS entropy_coding_mode=%u (%s) pps_id=%u sps_id=%u", ecm, ecm ? "CABAC" : "CAVLC",
         (unsigned)pps_id, (unsigned)sps_id);
}

/* TcpOutServer::push_au minus the TCP: scan NALs, cache SPS/PPS, gate on IDR,
 * prepend parameter sets to IDRs that lack them, optional non-ref drop. */
static void deliver_au(xcw_t *x, const uint8_t *au, size_t n, uint32_t ts) {
    int has_idr = 0, has_sps = 0, has_pps = 0, has_slice = 0, any_ref = 0;
    size_t i = 0;
#define IS_SC3(p) ((p) + 3 <= n && au[(p)] == 0 && au[(p) + 1] == 0 && au[(p) + 2] == 1)
    while (i + 3 <= n) {
        if (IS_SC3(i)) {
            if (i + 3 >= n) break;
            size_t s = i + 3, e = s;
            while (e + 3 <= n && !IS_SC3(e)) e++;
            if (e + 3 > n) e = n;
            size_t next = e;
            while (e > s && e < n && au[e - 1] == 0) e--;
            uint8_t t = au[s] & 0x1f;
            if (t == 7) {
                if (e - s <= sizeof(x->sps)) { memcpy(x->sps, au + s, e - s); x->sps_n = e - s; }
                has_sps = 1;
                if (x->log_sps_pending) { x->log_sps_pending = 0; log_sps(x, au + s, e - s); }
            } else if (t == 8) {
                if (e - s <= sizeof(x->pps)) { memcpy(x->pps, au + s, e - s); x->pps_n = e - s; }
                has_pps = 1;
                if (x->log_pps_pending) { x->log_pps_pending = 0; log_pps(x, au + s, e - s); }
            } else if (t == 5) {
                has_idr = 1;
            }
            if (t == 1 || t == 5) {
                has_slice = 1;
                if (au[s] & 0x60) any_ref = 1;
            }
            i = next;
        } else {
            i++;
        }
    }
#undef IS_SC3
    int nonref = has_slice && !any_ref;
    if (nonref && x->cfg.drop_nonref) {
        x->s.nonref_dropped++;
        return;
    }
    if (has_idr) {
        x->wait_idr = 0;
    } else if (x->wait_idr) {
        x->s.gated_dropped++;
        return;
    }
    xcw_au_info_t info;
    memset(&info, 0, sizeof(info));
    info.rtp_ts = ts;
    info.idr = (uint8_t)has_idr;
    info.nonref = (uint8_t)nonref;
    info.done_ms = xcw_now_ms();
    const uint8_t *out = au;
    size_t on = n;
    if (has_idr && ((!has_sps && x->sps_n) || (!has_pps && x->pps_n))) {
        static const uint8_t sc[4] = {0, 0, 0, 1};
        size_t o = 0;
        if (!has_sps && x->sps_n) {
            memcpy(x->aubuf + o, sc, 4); o += 4;
            memcpy(x->aubuf + o, x->sps, x->sps_n); o += x->sps_n;
        }
        if (!has_pps && x->pps_n) {
            memcpy(x->aubuf + o, sc, 4); o += 4;
            memcpy(x->aubuf + o, x->pps, x->pps_n); o += x->pps_n;
        }
        memcpy(x->aubuf + o, au, n);
        out = x->aubuf;
        on = o + n;
        info.params_prepended = 1;
    }
    if (has_idr) x->s.idrs++;
    x->s.aus++;
    x->s.au_bytes_last = (uint32_t)on;
    x->s.video_bytes += on;
    if (!x->s.t_first_au_ms) x->s.t_first_au_ms = since_answer(x, info.done_ms);
    if (x->video_paused) {   /* v0.19.3: no menu ninguém decodifica */
        x->s.paused_dropped++;
        return;
    }
    if (x->cfg.on_video_au) x->cfg.on_video_au(x->cfg.user, out, on, &info);
}

static void jit_emit(void *user, const uint8_t *au, size_t n, uint32_t ts) { deliver_au((xcw_t *)user, au, n, ts); }

static void jit_nack(void *user, uint16_t pid, uint16_t blp) {
    xcw_t *x = (xcw_t *)user;
    if (x->pc && peer_connection_send_nack(x->pc, pid, blp) >= 0) {
        x->s.nacks_sent++;
        int k = x->nack_w++ & 15;   /* remember the first lost seq of this NACK */
        x->nack_t[k].seq = pid;
        x->nack_t[k].used = 1;
        x->nack_t[k].t_us = xcw_now_us();
    }
}

/* A retransmission (or a late original) of a NACKed seq: one RTT sample. */
static void rtt_check(xcw_t *x, const uint8_t *rtp, size_t n) {
    if (n < 4) return;
    uint16_t seq = (uint16_t)((rtp[2] << 8) | rtp[3]);
    for (int k = 0; k < 16; k++) {
        if (!x->nack_t[k].used || x->nack_t[k].seq != seq) continue;
        x->nack_t[k].used = 0;
        uint32_t us = (uint32_t)(xcw_now_us() - x->nack_t[k].t_us);
        if (us > 2000000) return;
        x->s.rtt_samples++;
        x->s.rtt_last_us = us;
        x->s.rtt_avg_us = x->s.rtt_avg_us ? (x->s.rtt_avg_us * 7 + us) / 8 : us;
        return;
    }
}

/* libpeer (green-nx patch) hands over the whole decrypted RTP packet. */
static void on_video_track(uint8_t *data, size_t size, void *user) {
    xcw_t *x = (xcw_t *)user;
    if (size == 0) return;
    x->s.rtp_packets++;
    if (x->s.nacks_sent) rtt_check(x, data, size);
    int want_kf = 0;
    xcj_receive(&x->jit, data, size, xcw_now_ms(), jit_emit, jit_nack, x, &want_kf);
    if (want_kf && !x->video_paused) request_keyframe_throttled(x, 0);   /* v0.19.3: pausado não pede */
}

/* capture-rtp on_audio_track: strip the RTP header (CSRCs, extension,
 * padding); hand over seq + timestamp + Opus payload. */
static void on_audio_track(uint8_t *data, size_t size, void *user) {
    xcw_t *x = (xcw_t *)user;
    if (size < 12 || (data[0] >> 6) != 2) return;
    size_t off = 12 + 4u * (data[0] & 0x0f);
    if ((data[0] & 0x10) && off + 4 <= size) off += 4 + 4u * (((size_t)data[off + 2] << 8) | data[off + 3]);
    size_t end = size;
    if ((data[0] & 0x20) && end > off) {
        size_t pad = data[end - 1];
        end = pad <= end - off ? end - pad : off;
    }
    if (off >= end) return;
    size_t n = end - off;
    x->s.audio_packets++;
    x->s.audio_bytes += n;
    uint16_t seq = (uint16_t)((data[2] << 8) | data[3]);
    uint32_t ts = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
    if (x->cfg.on_audio) x->cfg.on_audio(x->cfg.user, seq, ts, data + off, n);
}

static void on_request_keyframe(void *user) {
    xcw_t *x = (xcw_t *)user;
    x->s.remote_pli++;
}

static void on_pc_state(PeerConnectionState state, void *user) {
    xcw_t *x = (xcw_t *)user;
    uint64_t now = xcw_now_ms();
    xlog(x, "[pc] state -> %s", peer_connection_state_to_string(state));
    switch (state) {
        case PEER_CONNECTION_CHECKING: set_state(x, XCW_CHECKING, NULL); break;
        case PEER_CONNECTION_CONNECTED:
            if (!x->t_ice_abs) {
                x->t_ice_abs = now;
                x->s.t_ice_ms = since_answer(x, now);
            }
            set_state(x, XCW_ICE_CONNECTED, NULL);
            break;
        case PEER_CONNECTION_COMPLETED:
            x->s.t_dtls_ms = since_answer(x, now);
            x->last_sctp_ms = now;
            set_state(x, XCW_DTLS_CONNECTED, NULL);
            break;
        case PEER_CONNECTION_FAILED: set_state(x, XCW_FAILED, "ICE: all candidate pairs failed"); break;
        case PEER_CONNECTION_CLOSED: set_state(x, XCW_CLOSED, "ICE consent / binding request timeout"); break;
        case PEER_CONNECTION_DISCONNECTED: set_state(x, XCW_CLOSED, "disconnected"); break;
        default: break;
    }
}

/* ---------------------------------------------------------------- input */

static void read_pad(xcw_t *x, int i, xcp_gamepad_t *out) {
    for (;;) {
        uint32_t s1 = x->pad_seq[i];
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (s1 & 1) continue;
        memcpy(out, &x->pad_shared[i], sizeof(*out));
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (x->pad_seq[i] == s1) return;
    }
}

void xcw_set_gamepad(xcw_t *x, const xcp_gamepad_t *pad) {
    /* capture-rtp input_udp_loop: an index out of range is player 1 */
    int i = pad->index < XCW_MAX_PADS ? pad->index : 0;
    x->pad_seq[i]++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(&x->pad_shared[i], pad, sizeof(*pad));
    x->pad_shared[i].index = (uint8_t)i;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    x->pad_seq[i]++;
    if (!(x->pad_present & (1u << i))) __atomic_fetch_or(&x->pad_present, 1u << i, __ATOMIC_SEQ_CST);
}

/* capture-rtp input_send_loop (v0.19, per pad) as a non-blocking step: a pad
 * seen for the first time is announced with gamepadChanged(i, true) on
 * "control" (pad 0 went in the handshake); then every present pad is sent on
 * state change and at least every 50 ms, one shared sequence counter, one
 * pass at most every 4 ms. On failure: give the sequence number back, skip
 * the rest of the pass and back off 8 -> 20 -> 100 ms. */
static void input_step(xcw_t *x, uint64_t now) {
    if (!x->input_run || !x->handshake_done) return;
    PeerConnectionState st = peer_connection_get_state(x->pc);
    if (st != PEER_CONNECTION_CONNECTED && st != PEER_CONNECTION_COMPLETED) {
        x->consec_fails = 0;
        return;
    }
    if (now < x->next_input_ms) return;
    uint32_t present = x->pad_present;
    for (int i = 1; i < XCW_MAX_PADS; i++) {
        uint32_t bit = 1u << i;
        if ((present & bit) && !(x->pad_announced & bit)) {
            send_text(x, XCW_CH_CONTROL, xcp_gamepad_changed((uint8_t)i, 1));
            x->pad_announced |= bit;
            x->s.pads_announced++;
            xlog(x, "[input] controle %d do Xbox -> gamepadChanged(%d, added)", i, i);
        }
    }
    int failed = 0;
    for (int i = 0; i < XCW_MAX_PADS && !failed; i++) {
        if (!(present & (1u << i))) continue;
        xcp_gamepad_t snap;
        read_pad(x, i, &snap);
        int changed = memcmp(&snap, &x->last_sent[i], sizeof(snap)) != 0;
        if (!changed && now - x->last_send_ms_pad[i] < 50) continue;
        int press = snap.a || snap.b || snap.x || snap.y || snap.dpad_up || snap.dpad_down || snap.dpad_left ||
                    snap.dpad_right || snap.menu || snap.view;
        uint8_t pkt[XCP_GAMEPAD_PACKET_LEN];
        double elapsed_ms = (double)(xcw_now_us() - x->t_input_start_us) / 1000.0;
        size_t n = xcp_gamepad_packet(&x->ser, &snap, elapsed_ms, pkt);
        if (press && !x->logged_first_press) {
            x->logged_first_press = 1;
            xlog(x, "[input-send] first frame with button press queued (pkt %uB, pad %d)", (unsigned)n, i);
        }
        if (xcw_send(x, XCW_CH_INPUT, pkt, n, 0) == 0) {
            x->last_sent[i] = snap;
            x->last_send_ms_pad[i] = now;
            x->last_send_ms = now;
            x->consec_fails = 0;
            x->s.input_sent++;
        } else {
            xcp_rollback_sequence(&x->ser);
            x->consec_fails++;
            x->s.input_fail++;
            failed = 1;
        }
    }
    x->next_input_ms = now + (!failed ? 4 : x->consec_fails < 3 ? 8 : x->consec_fails < 10 ? 20 : 100);
    x->s.input_seq = x->ser.sequence;
}

/* ------------------------------------------------------------ lifecycle */

int xcw_global_init(void) { return peer_init(); }

void xcw_global_deinit(void) { peer_deinit(); }

void xcw_default_cfg(xcw_cfg_t *c) {
    memset(c, 0, sizeof(*c));
    c->video_kbps = 3000;
    c->video_w = 1280;
    c->video_h = 720;
    c->video_fps = 30;
    c->resolution_alias = "720";
    c->stun_url = NULL;
    c->dead_media_ms = 12000;
    c->max_drain = 32;
}

xcw_t *xcw_create(const xcw_cfg_t *cfg) {
    xcw_t *x = (xcw_t *)calloc(1, sizeof(*x));
    if (!x) return NULL;
    x->cfg = *cfg;
    if (x->cfg.dead_media_ms <= 0) x->cfg.dead_media_ms = 12000;
    if (x->cfg.max_drain <= 0) x->cfg.max_drain = 32;
    if (x->cfg.video_kbps <= 0) x->cfg.video_kbps = 3000;
    snprintf(x->alias, sizeof(x->alias), "%s", cfg->resolution_alias ? cfg->resolution_alias : "720");
    x->video_kbps = x->cfg.video_kbps;
    x->wait_idr = 1;
    x->log_sps_pending = x->log_pps_pending = 1;
    x->pad_present = 1;     /* player 1 always (zero frames until a pad talks) */
    x->pad_announced = 1;
    x->offer = (char *)malloc(XCW_OFFER_CAP);
    x->aubuf = (uint8_t *)malloc(XCJ_MAX_AU_BYTES + 2 * XCW_PARAM_CAP + 16);
    if (!x->offer || !x->aubuf || xcj_init(&x->jit) != 0) {
        free(x->offer);
        free(x->aubuf);
        free(x);
        return NULL;
    }
    PeerConfiguration pc_cfg;
    memset(&pc_cfg, 0, sizeof(pc_cfg));
    pc_cfg.video_codec = CODEC_H264;
    pc_cfg.audio_codec = CODEC_OPUS;
    pc_cfg.datachannel = DATA_CHANNEL_BINARY;
    pc_cfg.onvideotrack = on_video_track;
    pc_cfg.onaudiotrack = on_audio_track;
    pc_cfg.on_request_keyframe = on_request_keyframe;
    pc_cfg.user_data = x;
    if (cfg->stun_url && cfg->stun_url[0]) {
        snprintf(x->stun_url, sizeof(x->stun_url), "%s", cfg->stun_url);
        pc_cfg.ice_servers[0].urls = x->stun_url;
    }
    x->pc = peer_connection_create(&pc_cfg);
    if (!x->pc) {
        xcj_free(&x->jit);
        free(x->offer);
        free(x->aubuf);
        free(x);
        return NULL;
    }
    peer_connection_oniceconnectionstatechange(x->pc, on_pc_state);
    /* handlers BEFORE the channels open: the first inbound SCTP message
     * (xCloud's HandshakeAck) must find them */
    peer_connection_ondatachannel(x->pc, on_channel_message, on_channel_open, on_channel_close);
    x->st = XCW_NEW;
    return x;
}

void xcw_destroy(xcw_t *x) {
    if (!x) return;
    if (x->pc) {
        peer_connection_close(x->pc);
        peer_connection_destroy(x->pc);
    }
    xcj_free(&x->jit);
    free(x->offer);
    free(x->aubuf);
    free(x);
}

/* capture-rtp main(): a=setup:passive -> a=setup:actpass (a no-op with the
 * current libpeer, whose offer says a=setup:active — kept for fidelity), and
 * b=AS:<kbps> + b=TIAS:<bps> right after the m=video section's c= line. */
static void munge_offer(xcw_t *x, const char *raw) {
    char *o = x->offer;
    size_t cap = XCW_OFFER_CAP;
    size_t n = 0;
    const char *p = raw;
    while (*p && n + 1 < cap) {
        if (strncmp(p, "a=setup:passive", 15) == 0 && n + 15 < cap) {
            memcpy(o + n, "a=setup:actpass", 15);
            n += 15;
            p += 15;
            continue;
        }
        o[n++] = *p++;
    }
    o[n] = 0;
    int kbps = x->video_kbps;
    char *mv = strstr(o, "m=video");
    char *c = mv ? strstr(mv, "\nc=") : NULL;
    char *nx = mv ? strstr(mv + 1, "\nm=") : NULL;
    if (kbps > 0 && c && (!nx || c < nx)) {
        char *eol = strchr(c + 1, '\n');
        if (eol) {
            int crlf = eol > o && eol[-1] == '\r';
            char ins[96];
            int il = snprintf(ins, sizeof(ins), "b=AS:%d%sb=TIAS:%lld%s", kbps, crlf ? "\r\n" : "\n",
                              (long long)kbps * 1000, crlf ? "\r\n" : "\n");
            size_t at = (size_t)(eol + 1 - o);
            if (n + (size_t)il + 1 < cap) {
                memmove(o + at + il, o + at, n - at + 1);
                memcpy(o + at, ins, (size_t)il);
                n += (size_t)il;
                xlog(x, "[sdp] video bandwidth cap: b=AS:%d b=TIAS:%lld (m=video)", kbps, (long long)kbps * 1000);
            }
        }
    } else {
        xlog(x, "[sdp] WARN: m=video/c= not found, no b=AS cap");
    }
}

static void extract_from_offer(xcw_t *x) {
    const char *u = strstr(x->offer, "a=ice-ufrag:");
    x->ufrag[0] = 0;
    if (u) {
        u += 12;
        size_t l = strcspn(u, "\r\n");
        if (l >= sizeof(x->ufrag)) l = sizeof(x->ufrag) - 1;
        memcpy(x->ufrag, u, l);
        x->ufrag[l] = 0;
    }
    x->ncands = 0;
    const char *p = x->offer;
    while ((p = strstr(p, "a=candidate:")) != NULL && x->ncands < XCW_MAX_CANDS) {
        size_t l = strcspn(p + 2, "\r\n");
        if (l >= sizeof(x->cands[0])) l = sizeof(x->cands[0]) - 1;
        memcpy(x->cands[x->ncands], p + 2, l);
        x->cands[x->ncands][l] = 0;
        x->ncands++;
        p += 2;
    }
}

const char *xcw_create_offer(xcw_t *x) {
    const char *raw = peer_connection_create_offer(x->pc);
    if (!raw) return NULL;
    munge_offer(x, raw);
    extract_from_offer(x);
    xlog(x, "[sdp] offer size=%u ice-ufrag(local)=%s candidates=%d", (unsigned)strlen(x->offer), x->ufrag,
         x->ncands);
    return x->offer;
}

const char *xcw_local_ufrag(xcw_t *x) { return x->ufrag; }
int xcw_local_candidate_count(xcw_t *x) { return x->ncands; }
const char *xcw_local_candidate(xcw_t *x, int i) { return (i >= 0 && i < x->ncands) ? x->cands[i] : NULL; }

int xcw_add_remote_candidate(xcw_t *x, const char *cand) {
    char buf[256];
    const char *c = cand;
    if (strncmp(c, "a=", 2) == 0) c += 2;
    if (strncmp(c, "candidate:", 10) != 0) return -1;
    size_t l = strcspn(c, "\r\n");
    if (l >= sizeof(buf)) return -1;
    memcpy(buf, c, l);
    buf[l] = 0;
    int r = peer_connection_add_ice_candidate(x->pc, buf);
    xlog(x, "[ice] +remote%s: %.80s", r == 0 ? "" : " (REJECTED)", buf);
    return r == 0 ? 0 : -1;
}

int xcw_set_answer(xcw_t *x, const char *sdp) {
    x->t_answer = xcw_now_ms();
    x->answer_set = 1;
    peer_connection_set_remote_description(x->pc, sdp, SDP_TYPE_ANSWER);
    return 0;
}

void xcw_request_keyframe(xcw_t *x, int force) {
    if (force)
        x->kf_force = 1;
    else
        x->kf_wanted = 1;
}

void xcw_resync_video(xcw_t *x) { x->resync_req = 1; }

void xcw_set_video_paused(xcw_t *x, int on) {
    on = on ? 1 : 0;
    if (!x || x->video_paused == on) return;
    x->video_paused = on;
    if (!on) {   /* volta do menu: decoder novo, só a partir de um IDR, pedido já */
        x->wait_idr = 1;
        x->kf_force = 1;
    }
}

void xcw_set_video_kbps(xcw_t *x, int kbps) {
    if (kbps > 0) x->video_kbps = kbps;
}

static int xchg0(volatile int *p) {
    int v = *p;
    if (v) *p = 0;
    return v;
}

/* capture-rtp pc_thread: keyframe flags, RR + REMB every second. */
static void poll_timers(xcw_t *x, uint64_t now) {
    if (xchg0(&x->resync_req)) {
        x->wait_idr = 1;
        x->kf_wanted = 1;
    }
    if (!x->handshake_done) return;
    int force = xchg0(&x->kf_force);
    int wanted = xchg0(&x->kf_wanted);
    int starving = x->wait_idr && !x->video_paused;   /* consumer waiting for an IDR: keep asking */
    if (wanted || force || starving) request_keyframe_throttled(x, force);
    if (now - x->last_rr_ms >= 1000) {
        x->last_rr_ms = now;
        uint8_t fraction;
        uint32_t cumulative, highest_ext;
        if (xcj_report_stats(&x->jit, &fraction, &cumulative, &highest_ext) &&
            peer_connection_send_receiver_report(x->pc, fraction, cumulative, highest_ext, 0) >= 0)
            x->s.rr_sent++;
        if (peer_connection_send_remb(x->pc, (uint32_t)x->video_kbps * 1000u) >= 0) x->s.remb_sent++;
    }
    input_step(x, now);
}

void xcw_poll(xcw_t *x) {
    if (!x || !x->pc) return;
    uint64_t now = xcw_now_ms();
    PeerConnectionState st = peer_connection_get_state(x->pc);
    switch (st) {
        case PEER_CONNECTION_CHECKING:
        case PEER_CONNECTION_CONNECTED:
            /* One step per millisecond: libpeer's ICE counters
             * (AGENT_CONNCHECK_PERIOD/MAX) assume the upstream 1 ms select()
             * timeout per loop. */
            if (now != x->last_step_ms) {
                x->last_step_ms = now;
                int r = peer_connection_loop(x->pc);
                if (r > 0) x->s.rx_datagrams++;
            }
            break;
        case PEER_CONNECTION_COMPLETED:
            for (int i = 0; i < x->cfg.max_drain; i++) {
                int r = peer_connection_loop(x->pc);
                if (r <= 0) break;
                x->s.rx_datagrams++;
            }
            break;
        default:
            break;
    }
    /* usrsctp timers (no timer thread): every >= 10 ms like its own thread */
    if (x->last_sctp_ms && now - x->last_sctp_ms >= 10) {
        sctp_usrsctp_handle_timers((uint32_t)(now - x->last_sctp_ms));
        x->last_sctp_ms = now;
    }
    if (st == PEER_CONNECTION_COMPLETED && x->cfg.consent_interval_ms > 0 &&
        now - x->last_consent_ms >= (uint64_t)x->cfg.consent_interval_ms) {
        x->last_consent_ms = now;
        if (peer_connection_send_consent(x->pc) >= 0) x->s.consent_sent++;
    }
    /* green-nx dead-media rule: ICE up but DTLS silent for dead_media_ms */
    if (st == PEER_CONNECTION_CONNECTED && x->t_ice_abs && now - x->t_ice_abs > (uint64_t)x->cfg.dead_media_ms)
        set_state(x, XCW_FAILED, "dead media: ICE up but DTLS never completed");
    poll_timers(x, now);
}

xcw_state_t xcw_state(xcw_t *x) { return x->st; }

void xcw_get_stats(xcw_t *x, xcw_stats_t *out) {
    x->s.state = x->st;
    x->s.fail_reason = x->fail_reason;
    xcj_stats_t js = x->jit.st;
    x->s.jit_dropped = js.dropped;
    x->s.jit_resyncs = js.resyncs;
    x->s.jit_late = js.late;
    x->s.jit_dup = js.duplicates;
    x->s.jit_pool_full = js.pool_full;
    x->s.waiting_keyframe = (uint8_t)(xcj_waiting_keyframe(&x->jit) || x->wait_idr);
    x->s.srtp_errors = x->pc ? peer_connection_get_srtp_errors(x->pc) : 0;
    if (x->jit.have_seq) {   /* RFC 3550 cumulative loss, read-only (the RR keeps its intervals) */
        uint32_t expected = x->jit.cycles + x->jit.max_seq - x->jit.base_seq + 1;
        x->s.rtp_expected = expected;
        x->s.rtp_lost = expected > x->jit.received ? expected - x->jit.received : 0;
    }
    *out = x->s;
}
