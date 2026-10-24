/*
 * XCloud360 — TCP live H.264 stream client for Xbox 360 (libxenon + lwip 1.4.1)
 *
 * ARQUITETURA (obrigatoria pelo libxenon):
 *   lwipopts.h do libxenon fixa NO_SYS=1 e LWIP_SOCKET=0. Nao existe
 *   lwip_socket()/lwip_recv() — so RAW API (callbacks). Todos os callbacks
 *   TCP rodam no MESMO thread do main loop, disparados sincronamente por
 *   network_poll() → enet_poll() → ip_input().
 *
 *   Producer = on_recv (copy pbuf pro ring). Consumer = main loop lendo do ring.
 *   Mesmo thread = sem lock. Chame stream_poll() a cada iteracao do main loop.
 */

#include "tcp_stream.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <ppc/timebase.h>
#include <network/network.h>
#include <lwip/tcp.h>
#include <lwip/ip_addr.h>
#include <lwip/err.h>
#include <lwip/pbuf.h>

#define RING_CAPACITY         (1024 * 1024)    /* 1 MB — IDR 720p60 ~200KB */
#define RECONNECT_DELAY_MS    500

typedef struct {
    uint8_t *buf;
    size_t   cap;
    volatile size_t head;
    volatile size_t tail;
    size_t   dropped;
} ring_t;

typedef enum {
    NET_IDLE = 0, NET_CONNECTING, NET_CONNECTED, NET_ERROR, NET_CLOSED
} net_state_t;

static ring_t          g_ring;
static struct tcp_pcb *g_pcb = NULL;
static net_state_t     g_state = NET_IDLE;
static uint64_t        g_last_reconnect_tb = 0;
static char            g_server_ip[32] = "0.0.0.0";
static int             g_server_port = 0;

/* --- RING (SPSC, single-thread cooperative = seguro sem lock) ------------- */
static int ring_init(ring_t *r, size_t cap) {
    r->buf = (uint8_t*)malloc(cap);
    if (!r->buf) return -1;
    r->cap = cap; r->head = r->tail = r->dropped = 0;
    return 0;
}

static inline size_t ring_used(const ring_t *r) {
    size_t h = r->head, t = r->tail;
    return (h >= t) ? (h - t) : (r->cap - (t - h));
}
static inline size_t ring_free_(const ring_t *r) {
    return r->cap - ring_used(r) - 1;
}
static size_t ring_write(ring_t *r, const uint8_t *src, size_t n) {
    size_t avail = ring_free_(r);
    if (n > avail) { r->dropped += (n - avail); n = avail; }
    if (!n) return 0;
    size_t h = r->head;
    size_t first = (h + n <= r->cap) ? n : (r->cap - h);
    memcpy(r->buf + h, src, first);
    if (n > first) memcpy(r->buf, src + first, n - first);
    r->head = (h + n) % r->cap;
    return n;
}
static size_t ring_read_(ring_t *r, uint8_t *dst, size_t n) {
    size_t used = ring_used(r);
    if (n > used) n = used;
    if (!n) return 0;
    size_t t = r->tail;
    size_t first = (t + n <= r->cap) ? n : (r->cap - t);
    memcpy(dst, r->buf + t, first);
    if (n > first) memcpy(dst + first, r->buf, n - first);
    r->tail = (t + n) % r->cap;
    return n;
}

/* --- LWIP RAW API CALLBACKS ---------------------------------------------- */

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (p == NULL) {
        printf("[stream] server closed connection\n");
        g_state = NET_CLOSED;
        tcp_close(pcb);
        g_pcb = NULL;
        return ERR_OK;
    }
    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        return err;
    }

    u16_t total = p->tot_len;
    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        ring_write(&g_ring, (const uint8_t *)q->payload, q->len);
    }

    tcp_recved(pcb, total);   /* abre janela; senao throughput cai */
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)arg;
    printf("[stream] tcp err=%d\n", err);
    g_pcb = NULL;   /* lwip ja liberou */
    g_state = NET_ERROR;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)pcb;
    if (err != ERR_OK) {
        printf("[stream] connect failed err=%d\n", err);
        g_state = NET_ERROR;
        return err;
    }
    printf("[stream] connected to %s:%d\n", g_server_ip, g_server_port);
    g_state = NET_CONNECTED;

    /* Handshake opcional. Feeder aceita client sem hello, mas manda pra
     * facilitar debug em tcpdump. */
    const char *hello = "XCLD\n";
    tcp_write(pcb, hello, 5, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

/* --- CONNECT LOGIC -------------------------------------------------------- */
static int stream_connect(void) {
    ip_addr_t remote;
    if (!ipaddr_aton(g_server_ip, &remote)) return -1;

    g_pcb = tcp_new();
    if (!g_pcb) { printf("[stream] tcp_new failed\n"); return -1; }

    tcp_setprio(g_pcb, TCP_PRIO_MIN + 5);
    tcp_arg(g_pcb, NULL);
    tcp_err(g_pcb, on_err);
    tcp_recv(g_pcb, on_recv);

    g_state = NET_CONNECTING;
    err_t e = tcp_connect(g_pcb, &remote, g_server_port, on_connected);
    if (e != ERR_OK) {
        printf("[stream] tcp_connect err=%d\n", e);
        tcp_abort(g_pcb);
        g_pcb = NULL;
        g_state = NET_ERROR;
        return -1;
    }
    return 0;
}

/* --- STREAM API PUBLIC ---------------------------------------------------- */

int stream_init(const char *server_ip, int server_port) {
    strncpy(g_server_ip, server_ip, sizeof(g_server_ip) - 1);
    g_server_ip[sizeof(g_server_ip) - 1] = '\0';
    g_server_port = server_port;

    /* Aurora normalmente ja subiu a rede. libxenon nao verifica antes de
     * re-init, entao pulamos se netif ja tem IP. */
    if (netif.ip_addr.addr == 0) {
        int r = network_init();
        if (r == NETWORK_INIT_FAILURE) {
            printf("[stream] network_init hard fail\n");
            return -1;
        }
        if (r == NETWORK_INIT_DHCP_FAILURE) {
            printf("[stream] DHCP falhou (fallback IP estatico 192.168.1.99)\n");
            return -2;
        }
    } else {
        printf("[stream] network ja up (Aurora), skip init\n");
    }
    network_print_config();

    if (ring_init(&g_ring, RING_CAPACITY) < 0) return -1;
    g_last_reconnect_tb = mftb();
    return stream_connect();
}

void stream_poll(void) {
    uint64_t now = mftb();
    network_poll();

    if (g_state == NET_ERROR || g_state == NET_CLOSED || g_state == NET_IDLE) {
        if (tb_diff_msec(now, g_last_reconnect_tb) > RECONNECT_DELAY_MS) {
            g_last_reconnect_tb = now;
            printf("[stream] reconnecting...\n");
            stream_connect();
        }
    }
}

size_t stream_read(uint8_t *dst, size_t n) {
    return ring_read_(&g_ring, dst, n);
}

size_t stream_available(void) {
    return ring_used(&g_ring);
}

size_t stream_dropped(void) {
    return g_ring.dropped;
}
