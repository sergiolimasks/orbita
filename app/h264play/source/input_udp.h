/*
 * XCloud360 — UDP input sender. Xbox gamepad -> devbox:9001 -> capture-rtp
 * -> xCloud DataChannel "input" @ 125Hz.
 *
 * We use UDP instead of a second lwip TCP client because opening a second
 * tcp_pcb after stream_client's tcp_connect (before its callback runs and
 * lwIP timers gyrate) corrupted tcp_active_pcbs and caused an XeLL red
 * screen (bug v0.8.2, root-caused 2026-09-09).
 *
 * Wire format is 24 bytes little-endian, layout in capture-rtp.cpp (P11).
 */
#ifndef XCLOUD360_INPUT_UDP_H
#define XCLOUD360_INPUT_UDP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  input_udp_init(const char *server_ip, int server_port);
void input_udp_send(const uint8_t *buf, size_t n);
/* v0.14: 1 = input_udp_send só enfileira; quem envia é input_udp_flush(),
 * chamado pela thread de rede (lwIP não é thread-safe). */
void input_udp_set_deferred(int on);
void input_udp_flush(void);
/* v0.14.2: texto "STAT ..." de telemetria; o capture-rtp loga como [xbox]. */
void input_udp_send_stat(const char *txt, size_t n);

#ifdef __cplusplus
}
#endif

#endif
