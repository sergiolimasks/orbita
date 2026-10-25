/*
 * XCloud360 stage 4 — glue between the stage 3 session machine
 * (source/session/xs_machine.h, its xsm_rtc_* mailbox) and xc_webrtc.h.
 * Runs on the NETWORK THREAD (hw1): call xcws_step() from net_step().
 *
 * Implements the contract in SESSION-STAGE3.md "Contrato com o WebRTC":
 *   1. gen changed            -> destroy the old PeerConnection
 *   2. xsm_rtc_want_offer     -> xcw_create + xcw_create_offer (munged) ->
 *                                xsm_rtc_set_offer + one xsm_rtc_add_local per
 *                                "candidate:..." line of the offer
 *   3. xsm_rtc_take_answer    -> drain xsm_rtc_take_remote, add them, THEN
 *                                xcw_set_answer (capture-rtp "pre-SRD" order:
 *                                libpeer builds the pairs at SRD); later remotes
 *                                keep flowing in (patched libpeer pairs them)
 *   4. reports ICE_UP / MEDIA_UP / VIDEO (first IDR-gated AU) / DEAD / DROPPED
 *
 * Local candidates go one per line ("candidate:..."), like green-nx
 * engine.cpp. capture-rtp passed libpeer's onicecandidate value, which is the
 * WHOLE offer SDP, and xsm_rtc_add_local keeps only its first line ("v=0"),
 * so that variant cannot be reproduced through the mailbox anyway.
 */
#ifndef XC_WEBRTC_XSM_H
#define XC_WEBRTC_XSM_H
#include "xc_webrtc.h"
#include "xs_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xcws {
    xsm_t *m;
    xcw_cfg_t cfg;          /* template for every PeerConnection (callbacks...) */
    xcw_t *x;
    unsigned gen;
    int answered, rep_ice, rep_media, rep_video, rep_end;
} xcws_t;

/* cfg: callbacks + knobs (the PC launcher used --kbps 1500; stun_url must be
 * an IP literal or NULL on the network thread, see WEBRTC-STAGE4.md). */
void xcws_init(xcws_t *g, xsm_t *m, const xcw_cfg_t *cfg);
/* network thread, every net_step (~1 ms) */
void xcws_step(xcws_t *g);
/* network thread: the live PeerConnection or NULL (for xcw_send etc.). The
 * [any thread] calls of xc_webrtc.h may use it from elsewhere only while it
 * cannot be destroyed (e.g. guarded by the app's own session state). */
static inline xcw_t *xcws_pc(xcws_t *g) { return g->x; }
void xcws_shutdown(xcws_t *g);

#ifdef __cplusplus
}
#endif
#endif
