/* XCloud360 stage 4 — dumps everything xc_proto.c produces for the shared
 * vectors (proto_cases.inc); proto_golden.sh diffs it against green-nx C++. */
#include <stdio.h>
#include <string.h>
#include "xc_proto.h"
#include "proto_cases.inc"

static void hex(const char *name, const uint8_t *p, size_t n) {
    printf("%s:", name);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

int main(void) {
    printf("auth:%s\n", xcp_authorization_request());
    printf("gpc1:%s\n", xcp_gamepad_changed(0, 1));
    printf("gpc2:%s\n", xcp_gamepad_changed(3, 0));
    printf("vkr:%s\n", xcp_video_keyframe_requested());
    printf("res:%s\n", xcp_user_requested_resolution("720"));
    printf("res2:%s\n", xcp_user_requested_resolution("1080HQ"));
    printf("hs:%s\n", xcp_message_handshake());
    char buf[2048];
    for (int i = 0; i < XCP_STARTUP_MESSAGES; i++) {
        xcp_startup_message(i, 1280, 720, 3000, 30, buf, sizeof(buf));
        printf("start720_%d:%s\n", i, buf);
        xcp_startup_message(i, 1920, 1080, 8000, 60, buf, sizeof(buf));
        printf("start1080_%d:%s\n", i, buf);
    }
    for (size_t i = 0; i < sizeof(k_ack_cases) / sizeof(k_ack_cases[0]); i++)
        printf("ack%zu:%d\n", i, xcp_is_handshake_ack(k_ack_cases[i], strlen(k_ack_cases[i])));
    xcp_input_ser_t s = {0};
    uint8_t pkt[64];
    size_t n = xcp_client_metadata(&s, 0, pkt);
    hex("meta", pkt, n);
    for (size_t w = 0; w < sizeof(k_wire) / sizeof(k_wire[0]); w++) {
        uint8_t wire[24] = {0};
        const int *v = k_wire[w];
        wire[0] = (uint8_t)v[0];
        wire[1] = (uint8_t)v[1];
        wire[2] = (uint8_t)v[2];
        for (int k = 0; k < 6; k++) {
            int x = v[3 + k];
            wire[3 + 2 * k] = (uint8_t)(x & 0xff);
            wire[4 + 2 * k] = (uint8_t)((x >> 8) & 0xff);
        }
        xcp_gamepad_t g;
        xcp_gamepad_from_wire24(wire, 24, &g);
        for (size_t e = 0; e < sizeof(k_elapsed) / sizeof(k_elapsed[0]); e++) {
            n = xcp_gamepad_packet(&s, &g, k_elapsed[e], pkt);
            char name[32];
            snprintf(name, sizeof(name), "pad%zu_%zu", w, e);
            hex(name, pkt, n);
        }
    }
    /* clamping */
    xcp_gamepad_t c;
    memset(&c, 0, sizeof(c));
    c.left_x = 1.5f;
    c.left_y = -1.5f;
    c.right_x = -2.0f;
    c.right_y = 0.5f;
    c.left_trigger = -0.2f;
    c.right_trigger = 1.2f;
    c.nexus = 1;
    c.right_thumb = 1;
    n = xcp_gamepad_packet(&s, &c, 1.0, pkt);
    hex("clamp", pkt, n);
    xcp_rollback_sequence(&s);
    n = xcp_gamepad_packet(&s, &c, 2.0, pkt);
    hex("rollback", pkt, n);
    return 0;
}
