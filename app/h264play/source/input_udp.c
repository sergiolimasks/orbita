/*
 * XCloud360 — lwip UDP send-only client. Fire-and-forget.
 * Depends on netif being already up (stream_init has run and connected).
 */
#include "input_udp.h"

#include <stdio.h>
#include <string.h>

#include <lwip/udp.h>
#include <lwip/pbuf.h>
#include <lwip/ip_addr.h>
#include <lwip/err.h>

static struct udp_pcb *g_pcb = NULL;
static ip_addr_t       g_remote;
static int             g_port = 0;

int input_udp_init(const char *server_ip, int server_port) {
    if (g_pcb) return 0;
    if (!ipaddr_aton(server_ip, &g_remote)) return -1;
    g_port = server_port;
    g_pcb = udp_new();
    if (!g_pcb) return -1;
    printf("[input-udp] pcb=%p -> %s:%d\n", (void*)g_pcb, server_ip, server_port);
    return 0;
}

static unsigned long g_sent = 0;
static unsigned long g_fail_pcb = 0;
static unsigned long g_fail_pbuf = 0;
static unsigned long g_fail_send = 0;

void input_udp_send(const uint8_t *buf, size_t n) {
    static unsigned long last_fails = 0;
    unsigned long cur_fails;
    unsigned long total;
    if (!g_pcb) { g_fail_pcb++; goto report; }
    if (n == 0 || n > 256) return;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
    if (!p) { g_fail_pbuf++; goto report; }
    memcpy(p->payload, buf, n);
    err_t e = udp_sendto(g_pcb, p, &g_remote, g_port);
    pbuf_free(p);
    if (e != ERR_OK) { g_fail_send++; goto report; }
    g_sent++;
report:
    cur_fails = g_fail_pcb + g_fail_pbuf + g_fail_send;
    total = g_sent + cur_fails;
    /* Print stats every 30000 calls (~5-15min) OR on any fail transition.
     * Debug provado (UDP path funciona), reduzir spam pra não poluir a tela. */
    if ((total % 30000 == 0) || (cur_fails > last_fails)) {
        printf("[input-udp] sent=%lu fail_pcb=%lu fail_pbuf=%lu fail_send=%lu\n",
               g_sent, g_fail_pcb, g_fail_pbuf, g_fail_send);
        last_fails = cur_fails;
    }
}
