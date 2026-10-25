/*
 * XCloud360 stage 4 — host stand-in for the stage 3 session mailbox, so the
 * E2E can drive source/webrtc/xc_webrtc_xsm.c (the hw1 glue) exactly as the
 * Xbox will, without HTTPS/xCloud. The xsm_rtc_* bodies follow
 * source/session/xs_machine.c (same fields, same gen/answer_taken rules); the
 * fx_* functions play the session thread's side.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xs_machine.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static void lk(void) { pthread_mutex_lock(&g_mu); }
static void ul(void) { pthread_mutex_unlock(&g_mu); }

unsigned xsm_rtc_gen(xsm_t *m) { lk(); unsigned g = m->gen; ul(); return g; }

int xsm_rtc_want_offer(xsm_t *m, unsigned *gen) {
    lk();
    int w = m->want_offer && !m->offer;
    if (gen) *gen = m->gen;
    ul();
    return w;
}

void xsm_rtc_set_offer(xsm_t *m, unsigned gen, const char *sdp) {
    size_t n = strlen(sdp) + 1;
    char *c = (char *)malloc(n);
    if (!c) return;
    memcpy(c, sdp, n);
    lk();
    if (gen == m->gen && m->want_offer && !m->offer) { m->offer = c; c = NULL; }
    ul();
    free(c);
}

void xsm_rtc_add_local(xsm_t *m, unsigned gen, const char *cand) {
    char t[XS_CAND_LEN];
    size_t l = strcspn(cand, "\r\n");
    if (l == 0 || l >= sizeof(t)) return;
    memcpy(t, cand, l);
    t[l] = 0;
    lk();
    if (gen == m->gen && m->nlocal < XSM_MAX_LOCAL) snprintf(m->locals[m->nlocal++], XS_CAND_LEN, "%s", t);
    ul();
}

char *xsm_rtc_take_answer(xsm_t *m, unsigned gen) {
    char *a = NULL;
    lk();
    if (gen == m->gen && m->answer) { a = m->answer; m->answer = NULL; m->answer_taken = 1; }
    ul();
    return a;
}

int xsm_rtc_take_remote(xsm_t *m, unsigned gen, char (*out)[XS_CAND_LEN], int max) {
    int k = 0;
    lk();
    if (gen == m->gen && m->answer_taken)
        while (m->remote_taken < m->nremote && k < max) memcpy(out[k++], m->remotes[m->remote_taken++], XS_CAND_LEN);
    ul();
    return k;
}

void xsm_rtc_report(xsm_t *m, unsigned gen, int ev) {
    lk();
    if (gen == m->gen) {
        if (ev == XSM_RTC_ICE_UP) m->ev_ice_up = 1;
        else if (ev == XSM_RTC_MEDIA_UP) m->ev_media_up = 1;
        else if (ev == XSM_RTC_VIDEO) m->ev_video = 1;
        else if (ev == XSM_RTC_DEAD) m->ev_dead = 1;
        else if (ev == XSM_RTC_DROPPED) m->ev_dropped = 1;
    }
    ul();
}

/* ---- session-thread side (what xs_machine's WAIT_OFFER / ICE_BRIDGE do) */

void fx_request_offer(xsm_t *m) {
    lk();
    m->gen++;
    m->want_offer = 1;
    free(m->offer);
    m->offer = NULL;
    m->nlocal = m->nremote = m->remote_taken = m->answer_taken = 0;
    ul();
}

/* returns malloc'd offer or NULL; copies locals */
char *fx_take_offer(xsm_t *m, char (*locals)[XS_CAND_LEN], int *nlocal) {
    char *o = NULL;
    lk();
    if (m->offer) {
        o = strdup(m->offer);
        *nlocal = m->nlocal;
        memcpy(locals, m->locals, sizeof(m->locals[0]) * (size_t)m->nlocal);
    }
    ul();
    return o;
}

/* pre-SRD bridge done: remotes first, then the answer (xs_machine order) */
void fx_publish(xsm_t *m, const char *answer, char (*remotes)[XS_CAND_LEN], int n) {
    lk();
    for (int i = 0; i < n && m->nremote < XSM_MAX_REMOTE; i++) memcpy(m->remotes[m->nremote++], remotes[i], XS_CAND_LEN);
    m->answer = strdup(answer);
    ul();
}

void fx_events(xsm_t *m, int ev[5]) {
    lk();
    ev[0] = m->ev_ice_up;
    ev[1] = m->ev_media_up;
    ev[2] = m->ev_video;
    ev[3] = m->ev_dead;
    ev[4] = m->ev_dropped;
    ul();
}
