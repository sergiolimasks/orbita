/* XCloud360 stage 4 — see xc_webrtc_xsm.h. */
#include "xc_webrtc_xsm.h"

#include <stdlib.h>
#include <string.h>

#include "xcw_port.h"

void xcws_init(xcws_t *g, xsm_t *m, const xcw_cfg_t *cfg) {
    memset(g, 0, sizeof(*g));
    g->m = m;
    g->cfg = *cfg;
}

static void drop_pc(xcws_t *g) {
    if (g->x) xcw_destroy(g->x);
    g->x = NULL;
    g->answered = g->rep_ice = g->rep_media = g->rep_video = g->rep_end = 0;
}

void xcws_shutdown(xcws_t *g) { drop_pc(g); }

static void add_remotes(xcws_t *g) {
    static char c[8][XS_CAND_LEN];
    int n;
    while ((n = xsm_rtc_take_remote(g->m, g->gen, c, 8)) > 0)
        for (int i = 0; i < n; i++) xcw_add_remote_candidate(g->x, c[i]);
}

void xcws_step(xcws_t *g) {
    if (!g->m) return;
    unsigned gen = xsm_rtc_gen(g->m);
    if (g->x && gen != g->gen) drop_pc(g);   /* new attempt / re-signaling */

    if (!g->x && xsm_rtc_want_offer(g->m, &gen)) {
        g->x = xcw_create(&g->cfg);
        const char *offer = g->x ? xcw_create_offer(g->x) : NULL;
        if (!offer) {
            drop_pc(g);
            xsm_rtc_report(g->m, gen, XSM_RTC_DEAD);
            return;
        }
        g->gen = gen;
        xsm_rtc_set_offer(g->m, gen, offer);
        for (int i = 0; i < xcw_local_candidate_count(g->x); i++)
            xsm_rtc_add_local(g->m, gen, xcw_local_candidate(g->x, i));
    }
    if (!g->x) return;

    if (!g->answered) {
        char *answer = xsm_rtc_take_answer(g->m, g->gen);
        if (answer) {
            add_remotes(g);                 /* pre-SRD: pairs are built at SRD */
            xcw_set_answer(g->x, answer);   /* verbatim (CRLF intact) */
            free(answer);
            g->answered = 1;
        }
    } else {
        add_remotes(g);                     /* trickled later */
    }

    xcw_poll(g->x);

    xcw_state_t st = xcw_state(g->x);
    if (!g->rep_ice && st >= XCW_ICE_CONNECTED && st <= XCW_STREAMING) {
        g->rep_ice = 1;
        xsm_rtc_report(g->m, g->gen, XSM_RTC_ICE_UP);
    }
    if (!g->rep_media && st >= XCW_DTLS_CONNECTED && st <= XCW_STREAMING) {
        g->rep_media = 1;
        xsm_rtc_report(g->m, g->gen, XSM_RTC_MEDIA_UP);
    }
    if (!g->rep_video && g->rep_media) {
        xcw_stats_t s;
        xcw_get_stats(g->x, &s);
        if (s.aus > 0) {   /* first AU out of the IDR gate = decodable */
            g->rep_video = 1;
            xsm_rtc_report(g->m, g->gen, XSM_RTC_VIDEO);
        }
    }
    if (!g->rep_end && (st == XCW_FAILED || st == XCW_CLOSED)) {
        g->rep_end = 1;
        xsm_rtc_report(g->m, g->gen, g->rep_media ? XSM_RTC_DROPPED : XSM_RTC_DEAD);
        /* keep the dead PC until the session bumps gen (cheap: poll is a no-op
         * in FAILED/CLOSED) so a late want_offer of the same gen cannot
         * spawn a twin */
    }
}
