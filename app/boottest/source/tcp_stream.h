/*
 * XCloud360 — TCP live H.264 stream client (dev-loop)
 *
 * Substitui embedded_stream por live feed do devbox via TCP LAN.
 * Wire: [uint32_be size][annex-b bytes] repetido (1 AU/mensagem).
 *
 * Modelo: single-thread cooperativo (lwip 1.4.1 RAW API do libxenon).
 * Producer = recv_cb → ring. Consumer = extractor length-prefix no main loop.
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

#ifdef __cplusplus
}
#endif

#endif
