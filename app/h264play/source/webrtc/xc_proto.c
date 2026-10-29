/*
 * XCloud360 stage 4 — see xc_proto.h. Close port of green-nx
 * src/core/xcloud_protocol.cpp (GPL-3.0). Byte-exactness against the C++
 * original is checked by hosttest/webrtc/proto_golden.sh.
 */
#include "xc_proto.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- input */

#define REPORT_GAMEPAD         2
#define REPORT_CLIENT_METADATA 8

static uint8_t *put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static uint8_t *put_f64le(uint8_t *p, double d) {
    uint64_t v;
    memcpy(&v, &d, 8);   /* IEEE-754 bits in host integer order */
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
    return p + 8;
}

static int16_t axis(float value) {
    float scaled = value * 32767.0f;
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32767.0f) scaled = -32767.0f;
    return (int16_t)scaled;
}

static uint16_t trigger(float value) {
    if (value < 0) return 0;
    float scaled = value * 65535.0f;
    if (scaled > 65535.0f) scaled = 65535.0f;
    return (uint16_t)scaled;
}

size_t xcp_client_metadata(xcp_input_ser_t *s, uint8_t max_touchpoints, uint8_t *out) {
    uint8_t *p = out;
    p = put_u16le(p, REPORT_CLIENT_METADATA);
    p = put_u32le(p, ++s->sequence);
    p = put_f64le(p, 0.0);
    *p++ = max_touchpoints;
    return (size_t)(p - out);
}

static uint32_t gamepad_physicality(const xcp_gamepad_t *pad) {
    uint32_t m = 0;
    if (pad->dpad_up) m |= 1u;
    if (pad->dpad_down) m |= 2u;
    if (pad->dpad_left) m |= 4u;
    if (pad->dpad_right) m |= 8u;
    if (pad->menu) m |= 16u;
    if (pad->view) m |= 32u;
    if (pad->left_thumb) m |= 64u;
    if (pad->right_thumb) m |= 128u;
    if (pad->left_shoulder) m |= 256u;
    if (pad->right_shoulder) m |= 512u;
    if (pad->nexus) m |= 1024u;
    if (pad->a) m |= 4096u;
    if (pad->b) m |= 8192u;
    if (pad->x) m |= 16384u;
    if (pad->y) m |= 32768u;
    if (pad->left_trigger > 0) m |= 65536u;
    if (pad->right_trigger > 0) m |= 131072u;
    if (pad->left_x != 0 || pad->left_y != 0) m |= 262144u | 524288u;
    if (pad->right_x != 0 || pad->right_y != 0) m |= 1048576u | 2097152u;
    return m;
}

size_t xcp_gamepad_packet(xcp_input_ser_t *s, const xcp_gamepad_t *pad, double elapsed_ms, uint8_t *out) {
    uint8_t *p = out;
    p = put_u16le(p, REPORT_GAMEPAD);
    p = put_u32le(p, ++s->sequence);
    p = put_f64le(p, elapsed_ms);
    *p++ = 1;            /* one gamepad frame */
    *p++ = pad->index;

    uint16_t mask = 0;
    if (pad->nexus) mask |= 2;
    if (pad->menu) mask |= 4;
    if (pad->view) mask |= 8;
    if (pad->a) mask |= 16;
    if (pad->b) mask |= 32;
    if (pad->x) mask |= 64;
    if (pad->y) mask |= 128;
    if (pad->dpad_up) mask |= 256;
    if (pad->dpad_down) mask |= 512;
    if (pad->dpad_left) mask |= 1024;
    if (pad->dpad_right) mask |= 2048;
    if (pad->left_shoulder) mask |= 4096;
    if (pad->right_shoulder) mask |= 8192;
    if (pad->left_thumb) mask |= 16384;
    if (pad->right_thumb) mask |= 32768;
    p = put_u16le(p, mask);

    p = put_u16le(p, (uint16_t)axis(pad->left_x));
    p = put_u16le(p, (uint16_t)axis(-pad->left_y));
    p = put_u16le(p, (uint16_t)axis(pad->right_x));
    p = put_u16le(p, (uint16_t)axis(-pad->right_y));
    p = put_u16le(p, trigger(pad->left_trigger));
    p = put_u16le(p, trigger(pad->right_trigger));
    /* v0.21.39: PhysicalPhysicality = máscara do que está apertado de verdade
     * (GamepadInputPhysicality do cliente web: DPadUp=1 ... A=4096 ... eixos);
     * VirtualPhysicality = 0 (não é toque). Antes ia 1 fixo (só "DPadUp") e o
     * teclado do sistema do Xbox ignorava o A (os jogos não ligam). */
    p = put_u32le(p, gamepad_physicality(pad));
    p = put_u32le(p, 0);
    return (size_t)(p - out);
}

static uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t rd_i16le(const uint8_t *p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

int xcp_gamepad_from_wire24(const uint8_t *buf, size_t n, xcp_gamepad_t *f) {
    if (n < 15) return -1;
    memset(f, 0, sizeof(*f));
    f->index = buf[0];
    uint8_t b0 = buf[1], b1 = buf[2];
    f->a = b0 & 1;
    f->b = (b0 >> 1) & 1;
    f->x = (b0 >> 2) & 1;
    f->y = (b0 >> 3) & 1;
    f->dpad_up = (b0 >> 4) & 1;
    f->dpad_down = (b0 >> 5) & 1;
    f->dpad_left = (b0 >> 6) & 1;
    f->dpad_right = (b0 >> 7) & 1;
    f->menu = b1 & 1;
    f->view = (b1 >> 1) & 1;
    f->nexus = (b1 >> 2) & 1;
    f->left_shoulder = (b1 >> 3) & 1;
    f->right_shoulder = (b1 >> 4) & 1;
    f->left_thumb = (b1 >> 5) & 1;
    f->right_thumb = (b1 >> 6) & 1;
    f->left_trigger = rd_u16le(buf + 3) / 65535.0f;
    f->right_trigger = rd_u16le(buf + 5) / 65535.0f;
    f->left_x = rd_i16le(buf + 7) / 32767.0f;
    f->left_y = rd_i16le(buf + 9) / 32767.0f;
    f->right_x = rd_i16le(buf + 11) / 32767.0f;
    f->right_y = rd_i16le(buf + 13) / 32767.0f;
    return 0;
}

/* ----------------------------------------------------------------- JSON */

const char *xcp_authorization_request(void) {
    return "{\"accessKey\":\"4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E\",\"message\":\"authorizationRequest\"}";
}

const char *xcp_gamepad_changed(uint8_t index, int was_added) {
    static char buf[96];
    snprintf(buf, sizeof(buf), "{\"gamepadIndex\":%u,\"message\":\"gamepadChanged\",\"wasAdded\":%s}",
             (unsigned)index, was_added ? "true" : "false");
    return buf;
}

const char *xcp_video_keyframe_requested(void) {
    return "{\"ifrRequested\":true,\"message\":\"videoKeyframeRequested\"}";
}

const char *xcp_user_requested_resolution(const char *alias) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "{\"message\":\"userRequestedResolutionUpdate\",\"resolutionAlias\":\"%s\"}", alias);
    return buf;
}

const char *xcp_message_handshake(void) {
    return "{\"cv\":\"0\",\"id\":\"be0bfc6d-1e83-4c8a-90ed-fa8601c5a179\",\"type\":\"Handshake\",\"version\":\"messageV1\"}";
}

/* Minimal scanner: top-level key "type" with string value "HandshakeAck".
 * (green-nx parses the full JSON with nlohmann; the server's ack is a flat
 * object, e.g. {"type":"HandshakeAck","version":"messageV1",...}.) */
int xcp_is_handshake_ack(const char *s, size_t n) {
    size_t i = 0;
    int depth = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    if (i >= n || s[i] != '{') return 0;
    for (; i < n; i++) {
        char c = s[i];
        if (c == '{' || c == '[') { depth++; continue; }
        if (c == '}' || c == ']') { depth--; continue; }
        if (c != '"') continue;
        /* string token */
        size_t start = ++i;
        while (i < n && s[i] != '"') { if (s[i] == '\\') i++; i++; }
        if (i >= n) return 0;
        size_t len = i - start;
        if (depth == 1 && len == 4 && memcmp(s + start, "type", 4) == 0) {
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) j++;
            if (j >= n || s[j] != ':') continue;   /* "type" was a value, not a key */
            j++;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) j++;
            static const char want[] = "\"HandshakeAck\"";
            return (n - j >= sizeof(want) - 1 && memcmp(s + j, want, sizeof(want) - 1) == 0) ? 1 : 0;
        }
    }
    return 0;
}

/* {"content":"<escaped>","cv":"","id":"5c5f2b40-...<1000+counter>","target":"<path>","type":"Message"} */
static int wrap_message(const char *path, const char *content, int counter, char *out, size_t cap) {
    size_t o = 0;
#define EMIT(str) do { size_t _l = strlen(str); if (o + _l >= cap) return -1; memcpy(out + o, (str), _l); o += _l; } while (0)
    EMIT("{\"content\":\"");
    for (const char *c = content; *c; c++) {
        if (o + 3 >= cap) return -1;
        if (*c == '"' || *c == '\\') out[o++] = '\\';
        out[o++] = *c;
    }
    char tail[160];
    snprintf(tail, sizeof(tail), "\",\"cv\":\"\",\"id\":\"5c5f2b40-0000-4000-8000-00000000%d\",\"target\":\"%s\",\"type\":\"Message\"}",
             1000 + counter, path);
    EMIT(tail);
#undef EMIT
    out[o] = 0;
    return (int)o;
}

int xcp_startup_message(int i, int width, int height, int max_bitrate_kbps, int fps, char *out, size_t cap) {
    char content[512];
    const char *path;
    switch (i) {
        case 0:
            path = "/streaming/systemUi/configuration";
            snprintf(content, sizeof(content), "{\"systemUis\":[],\"version\":[0,2,0]}");
            break;
        case 1:
            path = "/streaming/properties/clientappinstallidchanged";
            snprintf(content, sizeof(content), "{\"clientAppInstallId\":\"c97d7ee0-73b2-4239-bf1d-9d805a338429\"}");
            break;
        case 2:
            path = "/streaming/characteristics/orientationchanged";
            snprintf(content, sizeof(content), "{\"orientation\":0}");
            break;
        case 3:
            path = "/streaming/characteristics/touchinputenabledchanged";
            snprintf(content, sizeof(content), "{\"touchInputEnabled\":false}");
            break;
        case 4:
            path = "/streaming/characteristics/clientdevicecapabilities";
            snprintf(content, sizeof(content),
                     "{\"maxBitrateKbps\":%d,\"maxHeight\":%d,\"maxWidth\":%d,\"supportsCustomResolution\":true,"
                     "\"supportsFps\":%d,\"supportsHdr\":false,\"supportsHevc\":false,\"video\":{\"height\":%d,"
                     "\"maxBitrateKbps\":%d,\"maxHeight\":%d,\"maxWidth\":%d,\"width\":%d}}",
                     max_bitrate_kbps, height, width, fps, height, max_bitrate_kbps, height, width, width);
            break;
        case 5:
            path = "/streaming/characteristics/dimensionschanged";
            snprintf(content, sizeof(content),
                     "{\"horizontal\":%d,\"preferredHeight\":%d,\"preferredWidth\":%d,\"safeAreaBottom\":%d,"
                     "\"safeAreaLeft\":0,\"safeAreaRight\":%d,\"safeAreaTop\":0,\"supportsCustomResolution\":true,"
                     "\"vertical\":%d}",
                     width, height, width, height, width, height);
            break;
        default:
            return -1;
    }
    return wrap_message(path, content, i, out, cap);
}
