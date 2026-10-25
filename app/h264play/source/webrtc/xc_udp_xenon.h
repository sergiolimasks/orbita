/*
 * XCloud360 stage 4 — Xbox-only extras of the xcw_port.h implementation
 * (xc_udp_xenon.c): the hooks the app wires into its network thread.
 */
#ifndef XC_UDP_XENON_H
#define XC_UDP_XENON_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NETWORK THREAD (hw1) ONLY: call from net_step() after network_poll(),
 * next to np_step(). Services socket open/close and flushes queued sends.
 * The first call records which hardware thread owns lwIP. */
void xcw_net_step(void);

/* Function xcw_idle() runs while waiting ON the network thread (STUN
 * gathering inside xcw_create_offer, teardown). Typically the app's
 * net_step(): network_poll() + np_step() + xcw_net_step() ... */
void xcw_net_set_pump(void (*pump)(void));

/* Hostname resolver for STUN/TURN URLs (e.g. net_pipe's xcn_resolve, which
 * returns host-order IPv4 like xcw). Never used from the network thread. */
void xcw_set_resolver(int (*fn)(const char *host, uint32_t *ip));

/* Log sink (e.g. xc_log / np_log_line); default printf. */
void xcw_set_log_hook(void (*fn)(const char *line));

typedef struct {
    uint32_t rx_pkts, tx_pkts, rx_drop, rx_trunc, tx_drop, tx_err, rx_queued;
} xcw_udp_stats_t;
void xcw_udp_stats(int handle, xcw_udp_stats_t *out);

#ifdef __cplusplus
}
#endif
#endif
