/*
 * XCloud360 — TCP live H.264 stream client (Aurora-safe).
 * Wire: [uint32_be size][annex-b AU bytes] repetido.
 * lwip RAW API do libxenon (LWIP_SOCKET=0, NO_SYS=1). Cooperative single-thread.
 */
#ifndef XCLOUD360_TCP_STREAM_H
#define XCLOUD360_TCP_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int    stream_init(const char *server_ip, int server_port);
void   stream_poll(void);
size_t stream_read(uint8_t *dst, size_t n);
size_t stream_available(void);
size_t stream_dropped(void);
void   stream_reset(void);
/* v0.11.1: fecha o TCP atual + zera ring; próximo stream_poll reconecta.
 * Usado quando o framing corrompeu — força capture-rtp a mandar novo IDR. */
void   stream_force_reconnect(void);

#ifdef __cplusplus
}
#endif

#endif
