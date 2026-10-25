/*
 * XCloud360 stage 4 — xCloud application protocol on the WebRTC datachannels
 * ("message"/"control" JSON, "input" binary gamepad reports), in C.
 *
 * Close port of green-nx src/core/xcloud_protocol.cpp (GPL-3.0, (c) the
 * green-nx authors) — the byte strings it produces are compared against the
 * C++ original by hosttest/webrtc/proto_golden.sh. nlohmann::json dumps object
 * keys sorted, so the literals below are in sorted-key order.
 *
 * Endianness: the input reports are LITTLE-endian on the wire (the C++ code
 * memcpy'd host values on little-endian Switch/PC). Xenon is big-endian, so
 * everything is written byte by byte here.
 */
#ifndef XC_PROTO_H
#define XC_PROTO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gnx::xcloud::GamepadFrame */
typedef struct xcp_gamepad {
    uint8_t index;
    uint8_t nexus, menu, view;
    uint8_t a, b, x, y;
    uint8_t dpad_up, dpad_down, dpad_left, dpad_right;
    uint8_t left_shoulder, right_shoulder;
    uint8_t left_thumb, right_thumb;
    float left_x, left_y, right_x, right_y;   /* -1..1 */
    float left_trigger, right_trigger;        /* 0..1 */
} xcp_gamepad_t;

/* gnx::xcloud::InputSerializer (running sequence) */
typedef struct xcp_input_ser {
    uint32_t sequence;
} xcp_input_ser_t;

#define XCP_CLIENT_METADATA_LEN 15
#define XCP_GAMEPAD_PACKET_LEN  38

/* Fill `out` (>= XCP_CLIENT_METADATA_LEN). Returns the length. */
size_t xcp_client_metadata(xcp_input_ser_t *s, uint8_t max_touchpoints, uint8_t *out);
/* Fill `out` (>= XCP_GAMEPAD_PACKET_LEN). Returns the length. */
size_t xcp_gamepad_packet(xcp_input_ser_t *s, const xcp_gamepad_t *pad, double elapsed_ms, uint8_t *out);
static inline void xcp_rollback_sequence(xcp_input_ser_t *s) { s->sequence--; }

/* Decode the 24-byte little-endian Xbox -> PC UDP :9001 wire format the
 * h264play app sends today (layout in capture-rtp.cpp input_udp_loop).
 * Returns 0, or -1 if n < 15 (capture-rtp ignored those too). */
int xcp_gamepad_from_wire24(const uint8_t *buf, size_t n, xcp_gamepad_t *out);

/* JSON messages. Each returns a static, NUL-terminated string. */
const char *xcp_authorization_request(void);
const char *xcp_gamepad_changed(uint8_t index, int was_added);   /* static buffer */
const char *xcp_video_keyframe_requested(void);
const char *xcp_user_requested_resolution(const char *alias);    /* static buffer */
const char *xcp_message_handshake(void);
/* 1 if payload is a JSON object whose top-level "type" is "HandshakeAck". */
int xcp_is_handshake_ack(const char *payload, size_t n);

/* The six "message" channel startup messages (after HandshakeAck). Writes
 * message `i` (0..5) into out (cap bytes, NUL-terminated). Returns its length,
 * or -1 (i out of range / too small). */
#define XCP_STARTUP_MESSAGES 6
int xcp_startup_message(int i, int width, int height, int max_bitrate_kbps, int fps, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
