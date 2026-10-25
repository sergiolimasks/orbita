/*
 * XCloud360 stage 4 — see xc_webrtc_mt.h. Static buffers only; a tiny
 * spinlock (GCC atomics: lwarx/stwcx. + lwsync on Xenon) guards the shared
 * state, which is only ever touched for a few hundred bytes of copying.
 */
#include "xc_webrtc_mt.h"

#include <string.h>

#include "xcw_port.h"

#define CMDQ 32

enum { C_NONE = 0, C_CAND, C_ANSWER, C_STOP };

static volatile int g_lock;
static void lk(void) {
    while (__atomic_test_and_set((void *)&g_lock, __ATOMIC_ACQUIRE)) {
    }
}
static void ul(void) { __atomic_clear((void *)&g_lock, __ATOMIC_RELEASE); }

/* shared (under g_lock) */
static int g_want_start, g_active, g_offer_ready, g_offer_failed;
static xcw_cfg_t g_cfg;
static char g_offer[XCWT_SDP_MAX], g_ufrag[64];
static char g_cands[XCWT_CAND_MAX][192];
static int g_ncands;
static struct {
    int type;
    char cand[256];
} g_q[CMDQ];
static int g_qr, g_qw;
static char g_answer[XCWT_SDP_MAX];
static int g_answer_pending;
static xcw_stats_t g_snap;
static xcw_t *volatile g_x;   /* owned by the network thread */

int xcwt_start(const xcw_cfg_t *cfg) {
    int r = -1;
    lk();
    if (!g_active && !g_want_start) {
        g_cfg = *cfg;
        g_want_start = 1;
        g_offer_ready = g_offer_failed = 0;
        g_qr = g_qw = 0;
        g_answer_pending = 0;
        memset(&g_snap, 0, sizeof(g_snap));
        r = 0;
    }
    ul();
    return r;
}

int xcwt_wait_offer(char *sdp, size_t sdp_cap, char *ufrag, size_t ufrag_cap, char cands[][192], int max_cands,
                    int timeout_ms) {
    uint64_t end = xcw_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        lk();
        if (g_offer_failed) {
            ul();
            return -1;
        }
        if (g_offer_ready) {
            if (sdp && sdp_cap) {
                strncpy(sdp, g_offer, sdp_cap - 1);
                sdp[sdp_cap - 1] = 0;
            }
            if (ufrag && ufrag_cap) {
                strncpy(ufrag, g_ufrag, ufrag_cap - 1);
                ufrag[ufrag_cap - 1] = 0;
            }
            int n = g_ncands < max_cands ? g_ncands : max_cands;
            for (int i = 0; i < n; i++) memcpy(cands[i], g_cands[i], 192);
            ul();
            return n;
        }
        ul();
        if (xcw_now_ms() > end) return -1;
        xcw_idle(1);
    }
}

static int push_cmd(int type, const char *cand) {
    int r = -1;
    lk();
    if (g_qw - g_qr < CMDQ) {
        int k = g_qw % CMDQ;
        g_q[k].type = type;
        g_q[k].cand[0] = 0;
        if (cand) {
            strncpy(g_q[k].cand, cand, sizeof(g_q[k].cand) - 1);
            g_q[k].cand[sizeof(g_q[k].cand) - 1] = 0;
        }
        g_qw++;
        r = 0;
    }
    ul();
    return r;
}

int xcwt_add_remote_candidate(const char *cand) { return push_cmd(C_CAND, cand); }

int xcwt_set_answer(const char *sdp) {
    if (strlen(sdp) >= sizeof(g_answer)) return -1;
    lk();
    strcpy(g_answer, sdp);
    g_answer_pending = 1;
    ul();
    /* ordering: candidates queued before this call are applied first (they
     * are drained before the answer in xcwt_poll) */
    return push_cmd(C_ANSWER, NULL);
}

void xcwt_stop(void) { push_cmd(C_STOP, NULL); }

xcw_state_t xcwt_state(void) {
    lk();
    xcw_state_t s = g_snap.state;
    ul();
    return s;
}

void xcwt_stats(xcw_stats_t *out) {
    lk();
    *out = g_snap;
    ul();
}

void xcwt_set_gamepad(const xcp_gamepad_t *pad) {
    xcw_t *x = g_x;
    if (x) xcw_set_gamepad(x, pad);
}

void xcwt_request_keyframe(int force) {
    xcw_t *x = g_x;
    if (x) xcw_request_keyframe(x, force);
}

void xcwt_resync_video(void) {
    xcw_t *x = g_x;
    if (x) xcw_resync_video(x);
}

/* ------------------------------------------------- network thread side */

xcw_t *xcwt_net_handle(void) { return g_x; }

void xcwt_poll(void) {
    int start = 0;
    xcw_cfg_t cfg;
    lk();
    if (g_want_start && !g_active) {
        cfg = g_cfg;
        g_want_start = 0;
        g_active = 1;
        start = 1;
    }
    ul();
    if (start) {
        xcw_t *x = xcw_create(&cfg);
        const char *offer = x ? xcw_create_offer(x) : NULL;
        lk();
        if (!offer) {
            g_offer_failed = 1;
            g_active = 0;
        } else {
            strncpy(g_offer, offer, sizeof(g_offer) - 1);
            g_offer[sizeof(g_offer) - 1] = 0;
            strncpy(g_ufrag, xcw_local_ufrag(x), sizeof(g_ufrag) - 1);
            g_ncands = 0;
            for (int i = 0; i < xcw_local_candidate_count(x) && i < XCWT_CAND_MAX; i++, g_ncands++)
                strncpy(g_cands[i], xcw_local_candidate(x, i), 191);
            g_offer_ready = 1;
        }
        ul();
        if (!offer && x) xcw_destroy(x);
        if (offer) g_x = x;
    }
    xcw_t *x = g_x;
    if (!x) return;
    /* commands, in order */
    for (;;) {
        int type = C_NONE;
        char cand[256];
        lk();
        if (g_qr != g_qw) {
            int k = g_qr % CMDQ;
            type = g_q[k].type;
            memcpy(cand, g_q[k].cand, sizeof(cand));
            g_qr++;
        }
        ul();
        if (type == C_NONE) break;
        if (type == C_CAND) {
            xcw_add_remote_candidate(x, cand);
        } else if (type == C_ANSWER) {
            static char ans[XCWT_SDP_MAX];
            int have = 0;
            lk();
            if (g_answer_pending) {
                memcpy(ans, g_answer, sizeof(ans));
                g_answer_pending = 0;
                have = 1;
            }
            ul();
            if (have) xcw_set_answer(x, ans);
        } else if (type == C_STOP) {
            g_x = NULL;
            xcw_destroy(x);
            lk();
            g_active = 0;
            g_snap.state = XCW_CLOSED;
            ul();
            return;
        }
    }
    xcw_poll(x);
    xcw_stats_t s;
    xcw_get_stats(x, &s);
    lk();
    g_snap = s;
    ul();
}
