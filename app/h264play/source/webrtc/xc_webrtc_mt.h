/*
 * XCloud360 stage 4 — cross-thread front end for xc_webrtc.h.
 *
 * libpeer/usrsctp are single-threaded and must live on the WebRTC thread
 * (hw1, the lwIP owner), but the session layer (source/session/, HTTP over
 * net_pipe) runs on another thread. This wrapper owns ONE session and turns
 * the signaling calls into commands executed inside xcwt_poll() on hw1.
 *
 *   session thread                      network thread (hw1, every ~1 ms)
 *   --------------                      ---------------------------------
 *   xcwt_start(&cfg)            ---->   xcw_create + xcw_create_offer
 *   xcwt_wait_offer(buf, ...)   <----   offer + ufrag + candidates published
 *   xcwt_add_remote_candidate() ---->   xcw_add_remote_candidate
 *   xcwt_set_answer(sdp)        ---->   xcw_set_answer
 *   xcwt_state()/xcwt_stats()   <----   snapshot refreshed every poll
 *   xcwt_stop()                 ---->   xcw_destroy
 *
 * Gamepad / keyframe / resync go straight to xc_webrtc's [any thread]
 * functions through xcwt_set_gamepad() etc. The callbacks in cfg fire on
 * the network thread.
 */
#ifndef XC_WEBRTC_MT_H
#define XC_WEBRTC_MT_H
#include "xc_webrtc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XCWT_SDP_MAX  16384
#define XCWT_CAND_MAX 8

/* ---- any thread (the session thread) ---- */
int  xcwt_start(const xcw_cfg_t *cfg);   /* -1 if a session is already active */
/* Waits for the offer (STUN gathering may take ~1 s). Copies the munged
 * offer SDP, the local ufrag and up to XCWT_CAND_MAX local candidates.
 * Returns the number of candidates, or -1 on timeout/failure. */
int  xcwt_wait_offer(char *sdp, size_t sdp_cap, char *ufrag, size_t ufrag_cap,
                     char cands[][192], int max_cands, int timeout_ms);
int  xcwt_add_remote_candidate(const char *cand);   /* queued; -1 if queue full */
int  xcwt_set_answer(const char *sdp);              /* queued; -1 if too long */
xcw_state_t xcwt_state(void);
void xcwt_stats(xcw_stats_t *out);
void xcwt_stop(void);                                /* queued; returns at once */
void xcwt_set_gamepad(const xcp_gamepad_t *pad);
void xcwt_request_keyframe(int force);
void xcwt_resync_video(void);

/* ---- network thread only ---- */
void xcwt_poll(void);
/* The live session (NULL if none) for app code that also runs on the network
 * thread and needs the full xc_webrtc.h API (e.g. xcw_send). */
xcw_t *xcwt_net_handle(void);

#ifdef __cplusplus
}
#endif
#endif
