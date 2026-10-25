/*
 * XCloud360 stage 4 — the ONLY things the WebRTC libraries (libpeer, usrsctp,
 * libsrtp, mbedTLS) need from the outside world. Everything is non-blocking
 * except xcw_idle() and xcw_resolve().
 *
 * Implemented by:
 *   - Xbox:  app/h264play/source/webrtc/xc_udp_xenon.c  (UDP over a lock-free
 *            pipe to the hw1 network thread, mftb clock, jitter entropy)
 *   - host:  app/h264play/hosttest/webrtc/xc_udp_posix.c (BSD sockets,
 *            CLOCK_MONOTONIC, /dev/urandom, optional induced packet loss)
 *
 * IPv4 addresses are host-order uint32 (a.b.c.d == a<<24|b<<16|c<<8|d), the
 * same convention as source/login/net_pipe.c.
 */
#ifndef XCW_PORT_H
#define XCW_PORT_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP endpoint bound to 0.0.0.0:port (0 = ephemeral). Returns a handle >= 0
 * and the bound port in *bound_port, or < 0. */
int  xcw_udp_open(uint16_t port, uint16_t *bound_port);
void xcw_udp_close(int h);
/* Queue one datagram. Returns len, or < 0 (no room / closed). Never blocks. */
int  xcw_udp_send(int h, uint32_t ip, uint16_t port, const void *buf, int len);
/* Pop one datagram. Returns its length, 0 if none is waiting, < 0 on error.
 * A datagram larger than cap is truncated (and counted as an error). */
int  xcw_udp_recv(int h, uint32_t *ip, uint16_t *port, void *buf, int cap);

/* First usable local IPv4 (the ICE host candidate). 0 = ok. */
int  xcw_local_ipv4(uint32_t *ip);
/* Hostname -> IPv4 (STUN/TURN servers). May block up to a few seconds.
 * Literal dotted quads must always work. 0 = ok. */
int  xcw_resolve(const char *host, uint32_t *ip);

/* Monotonic clock. */
uint64_t xcw_now_ms(void);
uint64_t xcw_now_us(void);
/* Wait about `ms` milliseconds WITHOUT starving the network: on the Xbox,
 * when called on the network thread itself this pumps lwIP. Only used on
 * the cold paths (STUN gathering, teardown). */
void xcw_idle(int ms);

/* Cryptographically strong random bytes (ICE credentials, SCTP tags). */
void xcw_random(void *buf, size_t n);

/* One log line, no trailing newline needed. */
void xcw_log(const char *line);

#ifdef __cplusplus
}
#endif
#endif
