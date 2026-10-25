/*
 * XCloud360 stage 4 — MBEDTLS_TIMING_ALT. mbedTLS' own timing.c refuses any
 * OS it does not know (bare-metal Xenon). DTLS only needs the delay timer
 * used for handshake retransmissions; the clock is the port layer's
 * monotonic millisecond counter (xcw_now_ms: mftb on Xenon,
 * CLOCK_MONOTONIC on the host). Implementation: deps/webrtc-xenon/port/
 * xcw_timing.c.
 */
#ifndef XC360_TIMING_ALT_H
#define XC360_TIMING_ALT_H
#include <stdint.h>

struct mbedtls_timing_hr_time {
    uint64_t start_ms;
};

typedef struct mbedtls_timing_delay_context {
    struct mbedtls_timing_hr_time timer;
    uint32_t int_ms;
    uint32_t fin_ms;
} mbedtls_timing_delay_context;

#endif
