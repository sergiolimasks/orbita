// XCloud360 stage 4 — the same dump as proto_test.c, produced by the ORIGINAL
// green-nx C++ (src/core/xcloud_protocol.cpp) + capture-rtp's 24-byte wire
// decode (input_udp_loop), so proto_golden.sh can diff byte for byte.
#include <cstdio>
#include <cstring>
#include <string>
#include "xcloud_protocol.hpp"
#include "proto_cases.inc"
using namespace gnx::xcloud;

static void hex(const char *name, const std::vector<uint8_t> &p) {
    std::printf("%s:", name);
    for (uint8_t b : p) std::printf("%02x", b);
    std::printf("\n");
}

static GamepadFrame decode(const uint8_t *buf) {   // verbatim capture-rtp input_udp_loop
    auto rd_u16 = [](const uint8_t *p) -> uint16_t { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); };
    auto rd_i16 = [](const uint8_t *p) -> int16_t { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); };
    GamepadFrame f{};
    f.index = buf[0];
    uint8_t b0 = buf[1], b1 = buf[2];
    f.a = b0 & 1;  f.b = (b0 >> 1) & 1;  f.x = (b0 >> 2) & 1;  f.y = (b0 >> 3) & 1;
    f.dpad_up    = (b0 >> 4) & 1;  f.dpad_down  = (b0 >> 5) & 1;
    f.dpad_left  = (b0 >> 6) & 1;  f.dpad_right = (b0 >> 7) & 1;
    f.menu = b1 & 1;   f.view = (b1 >> 1) & 1;   f.nexus = (b1 >> 2) & 1;
    f.left_shoulder  = (b1 >> 3) & 1;  f.right_shoulder = (b1 >> 4) & 1;
    f.left_thumb     = (b1 >> 5) & 1;  f.right_thumb    = (b1 >> 6) & 1;
    f.left_trigger  = rd_u16(buf + 3) / 65535.0f;
    f.right_trigger = rd_u16(buf + 5) / 65535.0f;
    f.left_x  = rd_i16(buf + 7)  / 32767.0f;
    f.left_y  = rd_i16(buf + 9)  / 32767.0f;
    f.right_x = rd_i16(buf + 11) / 32767.0f;
    f.right_y = rd_i16(buf + 13) / 32767.0f;
    return f;
}

int main() {
    std::printf("auth:%s\n", authorization_request().c_str());
    std::printf("gpc1:%s\n", gamepad_changed(0, true).c_str());
    std::printf("gpc2:%s\n", gamepad_changed(3, false).c_str());
    std::printf("vkr:%s\n", video_keyframe_requested().c_str());
    std::printf("res:%s\n", user_requested_resolution("720").c_str());
    std::printf("res2:%s\n", user_requested_resolution("1080HQ").c_str());
    std::printf("hs:%s\n", message_handshake().c_str());
    auto a = startup_messages(1280, 720, 3000, 30);
    auto b = startup_messages(1920, 1080, 8000, 60);
    for (size_t i = 0; i < a.size(); i++) {
        std::printf("start720_%zu:%s\n", i, a[i].c_str());
        std::printf("start1080_%zu:%s\n", i, b[i].c_str());
    }
    for (size_t i = 0; i < sizeof(k_ack_cases) / sizeof(k_ack_cases[0]); i++)
        std::printf("ack%zu:%d\n", i, is_handshake_ack(k_ack_cases[i]) ? 1 : 0);
    InputSerializer s;
    hex("meta", s.client_metadata());
    for (size_t w = 0; w < sizeof(k_wire) / sizeof(k_wire[0]); w++) {
        uint8_t wire[24] = {0};
        const int *v = k_wire[w];
        wire[0] = (uint8_t)v[0]; wire[1] = (uint8_t)v[1]; wire[2] = (uint8_t)v[2];
        for (int k = 0; k < 6; k++) {
            int x = v[3 + k];
            wire[3 + 2 * k] = (uint8_t)(x & 0xff);
            wire[4 + 2 * k] = (uint8_t)((x >> 8) & 0xff);
        }
        GamepadFrame g = decode(wire);
        for (size_t e = 0; e < sizeof(k_elapsed) / sizeof(k_elapsed[0]); e++) {
            char name[32];
            std::snprintf(name, sizeof(name), "pad%zu_%zu", w, e);
            hex(name, s.gamepad_packet(g, k_elapsed[e]));
        }
    }
    GamepadFrame c{};
    c.left_x = 1.5f; c.left_y = -1.5f; c.right_x = -2.0f; c.right_y = 0.5f;
    c.left_trigger = -0.2f; c.right_trigger = 1.2f; c.nexus = true; c.right_thumb = true;
    hex("clamp", s.gamepad_packet(c, 1.0));
    s.rollback_sequence();
    hex("rollback", s.gamepad_packet(c, 2.0));
    return 0;
}
