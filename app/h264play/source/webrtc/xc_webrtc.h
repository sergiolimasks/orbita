/*
 * XCloud360 stage 4 — on-Xbox xCloud WebRTC receiver (C API).
 *
 * Replaces what tools/xcloud-capture/rtp/capture-rtp.cpp does on the PC on
 * top of libpeer, so the Xbox no longer needs the PC for media:
 *   - SDP offer (libpeer, green-nx native-client template) + munging
 *     (a=setup:actpass, b=AS/b=TIAS video cap), answer, ICE candidates;
 *   - DTLS-SRTP, SRTP decrypt, video jitter buffer + RTCP NACK, keyframe
 *     requests (PLI + "videoKeyframeRequested", throttled 1/s), RR + REMB
 *     every second;
 *   - datachannels control/input/message/chat (sids 0/2/4/6) and the xCloud
 *     message/control handshake + startup messages;
 *   - the gamepad input stream (on change, >= 4 ms apart, 50 ms heartbeat).
 * Outputs: complete Annex-B H.264 access units (in order, IDR-gated, SPS/PPS
 * prepended on IDRs that lack them) and Opus packets (seq, ts, payload).
 *
 * Signaling (session/SDP/ICE HTTP to xCloud) is NOT here: the session layer
 * (source/session/) moves the strings this API produces/consumes.
 *
 * THREADING: every function runs on ONE thread (the "WebRTC thread": the
 * hw1 network thread, see WEBRTC-STAGE4.md), EXCEPT the ones marked
 * [any thread]. Callbacks fire on the WebRTC thread, from inside xcw_poll().
 *
 * Typical flow (mirrors capture-rtp main()):
 *   xcw_global_init();
 *   x = xcw_create(&cfg);
 *   offer = xcw_create_offer(x);            -> session: POST /sdp
 *   for each local candidate (xcw_local_candidate)  -> session: POST /ice
 *   for each remote candidate from GET /ice -> xcw_add_remote_candidate(x, c)
 *   xcw_set_answer(x, answer_sdp);          (remote candidates BEFORE this)
 *   loop: xcw_poll(x) every <= 1 ms; xcw_set_gamepad() from the input code;
 *         on XCW_EV_FAILED / dead media -> destroy, new xCloud session.
 *   xcw_destroy(x);
 */
#ifndef XC_WEBRTC_H
#define XC_WEBRTC_H
#include <stddef.h>
#include <stdint.h>

#include "xc_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xcw xcw_t;

typedef enum xcw_state {
    XCW_NEW = 0,          /* created / offer made */
    XCW_CHECKING,         /* answer set, ICE connectivity checks */
    XCW_ICE_CONNECTED,    /* ICE pair succeeded, DTLS handshake running */
    XCW_DTLS_CONNECTED,   /* DTLS-SRTP up, SCTP association starting */
    XCW_CHANNELS_OPEN,    /* SCTP up, datachannels opened, Handshake sent */
    XCW_STREAMING,        /* HandshakeAck received, startup sequence sent */
    XCW_FAILED,           /* ICE failed, or dead media (see stats.fail_reason) */
    XCW_CLOSED,           /* ICE consent timeout / SCTP closed / destroyed */
} xcw_state_t;

typedef enum xcw_channel {
    XCW_CH_CONTROL = 0,   /* "control"  protocol "controlV1" sid 0 */
    XCW_CH_INPUT,         /* "input"    protocol "1.0"       sid 2 */
    XCW_CH_MESSAGE,       /* "message"  protocol "messageV1" sid 4 */
    XCW_CH_CHAT,          /* "chat"     protocol "chatV1"    sid 6 */
    XCW_CH_COUNT,
    XCW_CH_UNKNOWN = -1,
} xcw_channel_t;

typedef struct xcw_au_info {
    uint32_t rtp_ts;      /* 90 kHz */
    uint8_t  idr;         /* contains an IDR slice */
    uint8_t  nonref;      /* every slice has nal_ref_idc == 0 (droppable) */
    uint8_t  params_prepended; /* SPS/PPS were inserted from the cache */
    uint64_t done_ms;     /* xcw_now_ms() when it completed */
} xcw_au_info_t;

typedef struct xcw_cfg {
    /* quality knobs (capture-rtp defaults: 3000 kbps, 1280x720, 30 fps, "720") */
    int video_kbps;               /* b=AS/b=TIAS in the offer, REMB, maxBitrateKbps */
    int video_w, video_h, video_fps;
    const char *resolution_alias; /* userRequestedResolutionUpdate */
    /* "stun:host:port" or NULL. On the Xbox use an IP literal or a name the
     * session thread resolved already (see WEBRTC-STAGE4.md). capture-rtp:
     * "stun:stun.l.google.com:19302". */
    const char *stun_url;
    int dead_media_ms;            /* ICE up but no DTLS (default 12000, green-nx) */
    int drop_nonref;              /* 1 = never deliver non-reference frames */
    int max_drain;                /* datagrams processed per xcw_poll (default 32) */
    /* Send our own ICE consent checks (RFC 7675) every N ms once connected;
     * an answer also refreshes libpeer's 10 s keepalive timer. 0 = off, which
     * is capture-rtp's behaviour (it relies on xCloud's own binding requests). */
    int consent_interval_ms;

    /* callbacks (WebRTC thread). The AU / payload buffers are only valid
     * during the call. */
    void (*on_video_au)(void *user, const uint8_t *au, size_t n, const xcw_au_info_t *info);
    void (*on_audio)(void *user, uint16_t seq, uint32_t rtp_ts, const uint8_t *opus, size_t n);
    void (*on_state)(void *user, xcw_state_t st);
    /* every datachannel message received (after the built-in handshake logic) */
    void (*on_channel_msg)(void *user, xcw_channel_t ch, const uint8_t *data, size_t n);
    void (*on_log)(void *user, const char *line);   /* NULL = xcw_log() */
    void *user;
} xcw_cfg_t;

typedef struct xcw_stats {
    xcw_state_t state;
    const char *fail_reason;      /* static string or NULL */
    /* timings, ms since xcw_set_answer (0 = not reached) */
    uint32_t t_ice_ms, t_dtls_ms, t_sctp_ms, t_handshake_ms, t_first_au_ms;
    /* video */
    uint32_t rtp_packets, aus, au_bytes_last, idrs, nonref_dropped, gated_dropped;
    uint64_t video_bytes;
    uint32_t jit_dropped, jit_resyncs, jit_late, jit_dup, jit_pool_full;
    uint32_t nacks_sent, pli_sent, keyframe_requests, remb_sent, rr_sent;
    uint8_t  waiting_keyframe;
    /* audio */
    uint32_t audio_packets;
    uint64_t audio_bytes;
    /* transport */
    uint32_t srtp_errors, rx_datagrams, remote_pli, consent_sent;
    /* datachannels */
    uint32_t dc_rx[XCW_CH_COUNT], dc_tx[XCW_CH_COUNT], dc_tx_fail;
    uint32_t input_sent, input_fail, input_seq;
    uint32_t pads_announced;      /* gamepadChanged(i>0) sent */
    /* stage 5: RTT from NACK -> arrival of that seq (0 samples = no loss) */
    uint32_t rtt_samples, rtt_last_us, rtt_avg_us;
    uint32_t rtp_expected, rtp_lost;   /* video, RFC 3550 cumulative (before NACK repair) */
    uint32_t paused_dropped;      /* v0.19.3: AUs thrown away while paused */
} xcw_stats_t;

/* Once per process: libsrtp + usrsctp (no threads). 0 = ok. */
int  xcw_global_init(void);
void xcw_global_deinit(void);

/* Fills cfg with the capture-rtp defaults (callbacks NULL). */
void xcw_default_cfg(xcw_cfg_t *cfg);

xcw_t *xcw_create(const xcw_cfg_t *cfg);
void   xcw_destroy(xcw_t *x);

/* Builds the local offer (gathers the host candidate; STUN if configured —
 * may wait up to ~1 s for the STUN answer). The returned SDP is already
 * munged (actpass + bandwidth) and owned by x. NULL on failure. */
const char *xcw_create_offer(xcw_t *x);
/* Local ICE username fragment (for the /ice POST's usernameFragment). */
const char *xcw_local_ufrag(xcw_t *x);
/* Local candidates from the offer, without the "a=" prefix
 * ("candidate:1 1 UDP 2130706431 192.168.0.10 50000 typ host").
 * i = 0..n-1; NULL past the end. */
int         xcw_local_candidate_count(xcw_t *x);
const char *xcw_local_candidate(xcw_t *x, int i);

/* Remote candidate, "candidate:..." or "a=candidate:..." (IPv4; the session
 * layer decodes xCloud's Teredo IPv6 like green-nx session.cpp). Best BEFORE
 * xcw_set_answer (that is when libpeer builds the pairs); later ones are
 * added to the running checks. 0 = ok. */
int xcw_add_remote_candidate(xcw_t *x, const char *candidate);
/* The answer, VERBATIM (CRLF). 0 = ok. */
int xcw_set_answer(xcw_t *x, const char *sdp);

/* Drive everything; call at least every 1 ms. Non-blocking except the DTLS
 * crypto steps (ECDHE/ECDSA, a few times per session). */
void xcw_poll(xcw_t *x);

/* [any thread] Current state of gamepad pad->index (0..XCW_MAX_PADS-1; out of
 * range = 0), sent on change + 50 ms heartbeat once STREAMING. Pad 0 is
 * always sent (player 1, zero frames until set); an index seen for the first
 * time is announced with gamepadChanged(i, true) first (capture-rtp v0.19).
 * Single writer per index. */
#define XCW_MAX_PADS 4
void xcw_set_gamepad(xcw_t *x, const xcp_gamepad_t *pad);
/* [any thread] Ask for an IDR: PLI + videoKeyframeRequested, at most 1/s
 * unless force. */
void xcw_request_keyframe(xcw_t *x, int force);
/* [any thread] Consumer lost video (ring overflow, decoder error...): drop
 * AUs until the next IDR and keep asking for one (1/s) until it arrives —
 * capture-rtp's TCP-client gating. */
void xcw_resync_video(xcw_t *x);
/* [any thread] New REMB / b=AS target (kbps), takes effect on the next REMB. */
void xcw_set_video_kbps(xcw_t *x, int kbps);
/* v0.19.3 (WebRTC thread): 1 = jogo pausado no menu com a sessão aberta.
 * Os AUs completos são jogados fora antes do on_video_au (o cache de
 * SPS/PPS continua sendo atualizado) e NENHUM pedido de keyframe sai
 * sozinho: nem o do jitter (perda sem reparo) nem o "starving" do IDR
 * gate. Pedidos explícitos (xcw_request_keyframe) continuam valendo.
 * 0 = volta: liga o IDR gate (só sai AU a partir do próximo IDR) e pede um
 * keyframe já, sem o limite de 1/s. */
void xcw_set_video_paused(xcw_t *x, int on);

/* Send on a channel (WebRTC thread). text=1 -> WebRTC string PPID (JSON on
 * control/message), 0 -> binary. Returns 0, or -1 (channel not open / SCTP
 * buffer full). */
int xcw_send(xcw_t *x, xcw_channel_t ch, const void *data, size_t n, int text);

xcw_state_t xcw_state(xcw_t *x);
void xcw_get_stats(xcw_t *x, xcw_stats_t *out);
const char *xcw_state_name(xcw_state_t st);

#ifdef __cplusplus
}
#endif
#endif
