/*
 * XCloud360 stage 4 — link check (not shipped): pulls in the WHOLE
 * xc_webrtc API with the Xbox port so every symbol of libpeer / usrsctp /
 * libsrtp / mbedTLS the app will need resolves against libxenon + newlib +
 * deps/xenon-pthread, exactly like the future app link.
 */
#include <stdio.h>
#include "xc_udp_xenon.h"
#include "xc_webrtc.h"

static void pump(void) { xcw_net_step(); }

int main(void) {
    xcw_net_set_pump(pump);
    xcw_global_init();
    xcw_cfg_t cfg;
    xcw_default_cfg(&cfg);
    xcw_t *x = xcw_create(&cfg);
    if (!x) return 1;
    const char *offer = xcw_create_offer(x);
    xcw_add_remote_candidate(x, "candidate:1 1 UDP 1 10.0.0.1 9 typ host");
    xcw_set_answer(x, offer ? offer : "");
    xcp_gamepad_t pad = {0};
    xcw_set_gamepad(x, &pad);
    xcw_request_keyframe(x, 1);
    xcw_resync_video(x);
    xcw_poll(x);
    xcw_send(x, XCW_CH_CHAT, "x", 1, 1);
    xcw_stats_t st;
    xcw_get_stats(x, &st);
    printf("%s %d\n", xcw_state_name(st.state), xcw_local_candidate_count(x));
    xcw_destroy(x);
    xcw_global_deinit();
    return 0;
}
