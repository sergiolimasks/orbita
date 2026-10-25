/*
 * XCloud360 — lwip UDP send-only client. Fire-and-forget.
 * Depends on netif being already up (stream_init has run and connected).
 *
 * v0.14: com a thread de rede ligada (input_udp_set_deferred(1)) o main não
 * pode chamar lwIP. input_udp_send() só deixa o pacote numa mailbox e a thread
 * de rede despacha em input_udp_flush(): estado do controle quando muda ou a
 * cada PAD_HEARTBEAT_US, e o "KFRQ" (pedido de IDR) assim que aparecer.
 */
#include "input_udp.h"

#include <stdio.h>
#include <string.h>

#include <ppc/atomic.h>
#include <ppc/timebase.h>
#include <lwip/udp.h>
#include <lwip/pbuf.h>
#include <lwip/ip_addr.h>
#include <lwip/err.h>

#define PAD_MAX           64
#define PAD_HEARTBEAT_US  8000   /* 125Hz, igual ao heartbeat do capture-rtp */

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

static void send_now(const uint8_t *buf, size_t n);

static volatile int g_deferred = 0;
static unsigned int g_box_lock = 0;
/* v0.19: uma caixa por controle (buf[0] = slot), senão o controle 2
 * sobrescreve o 1 antes da rede mandar. */
#define PAD_SLOTS 4
static uint8_t      g_box[PAD_SLOTS][PAD_MAX];
static size_t       g_box_len[PAD_SLOTS];
static volatile unsigned g_box_seq[PAD_SLOTS];
static volatile int g_kfrq_pending = 0;

#ifdef XCLOUD360_AUDIO
#define STAT_MAX 320   /* v0.18: + chaves do áudio (a_pkts ... a_dec_ms) */
#else
#define STAT_MAX 240
#endif
static unsigned int g_stat_lock = 0;
static char         g_stat[STAT_MAX];
static size_t       g_stat_len = 0;
static volatile int g_stat_pending = 0;

void input_udp_set_deferred(int on) { g_deferred = on; }

void input_udp_send_stat(const char *txt, size_t n) {
    if (n > STAT_MAX) n = STAT_MAX;
    if (!g_deferred) { send_now((const uint8_t *)txt, n); return; }
    lock(&g_stat_lock);
    memcpy(g_stat, txt, n);
    g_stat_len = n;
    g_stat_pending = 1;
    unlock(&g_stat_lock);
}

void input_udp_send(const uint8_t *buf, size_t n) {
    if (!g_deferred) { send_now(buf, n); return; }
    if (n == 4 && memcmp(buf, "KFRQ", 4) == 0) { g_kfrq_pending = 1; return; }
    if (n == 0 || n > PAD_MAX) return;
    int k = buf[0] < PAD_SLOTS ? buf[0] : 0;
    lock(&g_box_lock);
    memcpy(g_box[k], buf, n);
    g_box_len[k] = n;
    g_box_seq[k]++;
    unlock(&g_box_lock);
}

/* Só a thread de rede chama. */
void input_udp_flush(void) {
    static uint8_t  last[PAD_SLOTS][PAD_MAX];
    static size_t   last_len[PAD_SLOTS];
    static unsigned last_seq[PAD_SLOTS];
    static uint64_t last_tb[PAD_SLOTS];
    if (!g_deferred) return;
    if (g_kfrq_pending) {
        g_kfrq_pending = 0;
        send_now((const uint8_t *)"KFRQ", 4);
    }
    if (g_stat_pending) {
        char s[STAT_MAX];
        size_t n;
        lock(&g_stat_lock);
        memcpy(s, g_stat, g_stat_len);
        n = g_stat_len;
        g_stat_pending = 0;
        unlock(&g_stat_lock);
        send_now((const uint8_t *)s, n);
    }
    for (int k = 0; k < PAD_SLOTS; k++) {
        if (g_box_seq[k] == last_seq[k]) continue;
        uint8_t cur[PAD_MAX];
        size_t  cur_len;
        lock(&g_box_lock);
        memcpy(cur, g_box[k], g_box_len[k]);
        cur_len = g_box_len[k];
        last_seq[k] = g_box_seq[k];
        unlock(&g_box_lock);
        uint64_t now = mftb();
        int changed = cur_len != last_len[k] || memcmp(cur, last[k], cur_len) != 0;
        if (!changed && tb_diff_usec(now, last_tb[k]) < PAD_HEARTBEAT_US) continue;
        memcpy(last[k], cur, cur_len);
        last_len[k] = cur_len;
        last_tb[k] = now;
        send_now(cur, cur_len);
    }
}

static void send_now(const uint8_t *buf, size_t n) {
    static unsigned long last_fails = 0;
    unsigned long cur_fails;
    unsigned long total;
    if (!g_pcb) { g_fail_pcb++; goto report; }
#ifdef XCLOUD360_AUDIO
    if (n == 0 || n > STAT_MAX) return;   /* v0.18: STAT passou de 256 */
#else
    if (n == 0 || n > 256) return;
#endif
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
