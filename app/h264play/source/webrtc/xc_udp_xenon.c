/*
 * XCloud360 stage 4 — xcw_port.h on the Xbox (bare metal, libxenon lwIP 1.4.1
 * raw API, NO_SYS=1). Same pattern as source/login/net_pipe.c:
 *
 *   - the hw1 NETWORK THREAD is the only one that touches lwIP. It calls
 *     xcw_net_step() from its net_step() (next to np_step()): that services
 *     open/close requests and flushes the TX rings with udp_sendto();
 *   - the lwIP recv callback (also on hw1, inside network_poll) copies each
 *     datagram into a lock-free single-producer/single-consumer RX ring;
 *   - the WebRTC thread pops RX / pushes TX with xcw_udp_recv/xcw_udp_send.
 *     If the WebRTC thread IS the network thread (recommended placement, see
 *     WEBRTC-STAGE4.md) sends skip the ring and go straight to udp_sendto,
 *     and xcw_idle() keeps the network alive through xcw_net_set_pump().
 *
 * Rings: RX 1024 x 1.5 KB per socket (~1.5 MB: >2 s of 4 Mbps, a whole IDR
 * burst several times over), TX 256 x 1.5 KB. A full ring drops (counted);
 * RTP loss is then repaired by NACK like any network loss.
 *
 * Also: clock (mftb), host address (netif), CSPRNG (CTR-DRBG over
 * mbedtls_hardware_poll; a weak jitter-based poll is provided for builds
 * without source/login, whose strong one wins), log sink.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <network/network.h>
#include <ppc/timebase.h>
#include "../polite_wait.h"
#include <time/time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#include "xc_udp_xenon.h"
#include "xcw_port.h"

#define XUS_SOCKS 4
#define XUS_RX_SLOTS 1024u /* power of two */
#define XUS_TX_SLOTS 256u  /* power of two */
#define XUS_DGRAM 1500

typedef struct {
    uint16_t len;
    uint16_t port;
    uint32_t ip;
    uint8_t data[XUS_DGRAM];
} xus_dgram_t;

enum { XS_FREE = 0, XS_REQ_OPEN, XS_OPEN, XS_REQ_CLOSE, XS_ERR };

typedef struct {
    volatile int st;
    uint16_t req_port;
    volatile uint16_t bound_port;
    struct udp_pcb *pcb;
    xus_dgram_t *rx;
    volatile uint32_t rx_w, rx_r;
    xus_dgram_t *tx;
    volatile uint32_t tx_w, tx_r;
    volatile uint32_t rx_drop, rx_trunc, tx_drop, tx_err, rx_pkts, tx_pkts;
} xus_t;

static xus_t g_s[XUS_SOCKS + 1];   /* handles 1..XUS_SOCKS */
static volatile int g_net_pir = -1;
static void (*volatile g_pump)(void) = NULL;
static int (*volatile g_resolver)(const char *, uint32_t *) = NULL;
static void (*volatile g_log_hook)(const char *) = NULL;

static inline void lwsync(void) { __asm__ __volatile__("lwsync" ::: "memory"); }

static inline int cur_pir(void) {
    unsigned int pir;
    __asm__ __volatile__("mfspr %0, 1023" : "=r"(pir));
    return (int)pir;
}

/* lwIP is "ours" when we are the network thread, or before it exists (the
 * main thread owns lwIP until stream_start_thread, like np_init). */
static int on_net_thread(void) { return g_net_pir < 0 || cur_pir() == g_net_pir; }

static void ipa_from_u32(ip_addr_t *a, uint32_t ip) {
    IP4_ADDR(a, (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static uint32_t u32_from_ipa(const ip_addr_t *a) {
    return ((uint32_t)ip4_addr1(a) << 24) | ((uint32_t)ip4_addr2(a) << 16) | ((uint32_t)ip4_addr3(a) << 8) |
           ip4_addr4(a);
}

/* ------------------------------------------------------ network thread */

static void on_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, ip_addr_t *addr, u16_t port) {
    xus_t *s = (xus_t *)arg;
    (void)pcb;
    if (!s || s->st != XS_OPEN || !s->rx) {
        pbuf_free(p);
        return;
    }
    uint32_t w = s->rx_w;
    if (w - s->rx_r >= XUS_RX_SLOTS) {   /* consumer behind: drop (NACK repairs RTP) */
        s->rx_drop++;
        pbuf_free(p);
        return;
    }
    xus_dgram_t *d = &s->rx[w & (XUS_RX_SLOTS - 1)];
    u16_t n = p->tot_len;
    if (n > XUS_DGRAM) {
        n = XUS_DGRAM;
        s->rx_trunc++;
    }
    pbuf_copy_partial(p, d->data, n, 0);
    pbuf_free(p);
    d->len = n;
    d->port = port;
    d->ip = u32_from_ipa(addr);
    lwsync();   /* slot contents before the index */
    s->rx_w = w + 1;
    s->rx_pkts++;
}

static int raw_send(xus_t *s, uint32_t ip, uint16_t port, const void *buf, int len) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) {
        s->tx_err++;
        return -1;
    }
    memcpy(p->payload, buf, (size_t)len);
    ip_addr_t dst;
    ipa_from_u32(&dst, ip);
    err_t e = udp_sendto(s->pcb, p, &dst, port);
    pbuf_free(p);
    if (e != ERR_OK) {
        s->tx_err++;
        return -1;
    }
    s->tx_pkts++;
    return len;
}

static void do_open(xus_t *s) {
    s->pcb = udp_new();
    if (!s->pcb || udp_bind(s->pcb, IP_ADDR_ANY, s->req_port) != ERR_OK) {
        if (s->pcb) udp_remove(s->pcb);
        s->pcb = NULL;
        lwsync();
        s->st = XS_ERR;
        return;
    }
    s->bound_port = s->pcb->local_port;
    udp_recv(s->pcb, on_udp_recv, s);
    lwsync();
    s->st = XS_OPEN;
}

static void do_close(xus_t *s) {
    if (s->pcb) {
        udp_recv(s->pcb, NULL, NULL);
        udp_remove(s->pcb);
        s->pcb = NULL;
    }
    lwsync();
    s->st = XS_FREE;
}

void xcw_net_step(void) {
    if (g_net_pir != cur_pir()) {
        g_net_pir = cur_pir();
        lwsync();
    }
    for (int h = 1; h <= XUS_SOCKS; h++) {
        xus_t *s = &g_s[h];
        switch (s->st) {
            case XS_REQ_OPEN: do_open(s); break;
            case XS_REQ_CLOSE: do_close(s); break;
            case XS_OPEN:
                while (s->tx_r != s->tx_w) {
                    lwsync();   /* see the producer's slot contents */
                    xus_dgram_t *d = &s->tx[s->tx_r & (XUS_TX_SLOTS - 1)];
                    raw_send(s, d->ip, d->port, d->data, d->len);
                    lwsync();
                    s->tx_r++;
                }
                break;
            default: break;
        }
    }
}

void xcw_net_set_pump(void (*pump)(void)) { g_pump = pump; }
void xcw_set_resolver(int (*fn)(const char *host, uint32_t *ip)) { g_resolver = fn; }
void xcw_set_log_hook(void (*fn)(const char *line)) { g_log_hook = fn; }

void xcw_udp_stats(int h, xcw_udp_stats_t *o) {
    memset(o, 0, sizeof(*o));
    if (h < 1 || h > XUS_SOCKS) return;
    xus_t *s = &g_s[h];
    o->rx_pkts = s->rx_pkts;
    o->tx_pkts = s->tx_pkts;
    o->rx_drop = s->rx_drop;
    o->rx_trunc = s->rx_trunc;
    o->tx_drop = s->tx_drop;
    o->tx_err = s->tx_err;
    o->rx_queued = s->rx_w - s->rx_r;
}

/* --------------------------------------------------- WebRTC-thread side */

static void wait_state(xus_t *s, int while_st, int ms) {
    uint64_t end = xcw_now_ms() + (uint64_t)ms;
    while (s->st == while_st && xcw_now_ms() < end) xc_polite_udelay(200);
}

int xcw_udp_open(uint16_t port, uint16_t *bound_port) {
    int h;
    for (h = 1; h <= XUS_SOCKS && g_s[h].st != XS_FREE; h++) {}
    if (h > XUS_SOCKS) return -1;
    xus_t *s = &g_s[h];
    if (!s->rx) s->rx = (xus_dgram_t *)malloc(sizeof(xus_dgram_t) * XUS_RX_SLOTS);
    if (!s->tx) s->tx = (xus_dgram_t *)malloc(sizeof(xus_dgram_t) * XUS_TX_SLOTS);
    if (!s->rx || !s->tx) return -1;
    s->rx_w = s->rx_r = s->tx_w = s->tx_r = 0;
    s->rx_drop = s->rx_trunc = s->tx_drop = s->tx_err = s->rx_pkts = s->tx_pkts = 0;
    s->req_port = port;
    s->bound_port = 0;
    if (on_net_thread()) {
        do_open(s);
    } else {
        lwsync();
        s->st = XS_REQ_OPEN;
        wait_state(s, XS_REQ_OPEN, 2000);
    }
    if (s->st != XS_OPEN) {
        s->st = XS_FREE;
        return -1;
    }
    *bound_port = s->bound_port;
    return h;
}

void xcw_udp_close(int h) {
    if (h < 1 || h > XUS_SOCKS) return;
    xus_t *s = &g_s[h];
    if (s->st != XS_OPEN) return;
    if (on_net_thread()) {
        do_close(s);
    } else {
        lwsync();
        s->st = XS_REQ_CLOSE;
        wait_state(s, XS_REQ_CLOSE, 500);
    }
}

int xcw_udp_send(int h, uint32_t ip, uint16_t port, const void *buf, int len) {
    if (h < 1 || h > XUS_SOCKS || len <= 0 || len > XUS_DGRAM) return -1;
    xus_t *s = &g_s[h];
    if (s->st != XS_OPEN) return -1;
    if (on_net_thread()) return raw_send(s, ip, port, buf, len);
    uint32_t w = s->tx_w;
    if (w - s->tx_r >= XUS_TX_SLOTS) {
        s->tx_drop++;
        return -1;
    }
    xus_dgram_t *d = &s->tx[w & (XUS_TX_SLOTS - 1)];
    memcpy(d->data, buf, (size_t)len);
    d->len = (uint16_t)len;
    d->ip = ip;
    d->port = port;
    lwsync();
    s->tx_w = w + 1;
    return len;
}

int xcw_udp_recv(int h, uint32_t *ip, uint16_t *port, void *buf, int cap) {
    if (h < 1 || h > XUS_SOCKS) return -1;
    xus_t *s = &g_s[h];
    if (s->st != XS_OPEN) return -1;
    uint32_t r = s->rx_r;
    if (r == s->rx_w) return 0;
    lwsync();   /* slot written before rx_w */
    xus_dgram_t *d = &s->rx[r & (XUS_RX_SLOTS - 1)];
    int n = d->len;
    if (n > cap) {
        n = cap;
        s->rx_trunc++;
    }
    memcpy(buf, d->data, (size_t)n);
    *ip = d->ip;
    *port = d->port;
    lwsync();   /* done with the slot before releasing it */
    s->rx_r = r + 1;
    return n;
}

int xcw_local_ipv4(uint32_t *ip) {
    /* v0.21.10: o IP da rota padrão. Sem cabo o enet fica com o 192.168.1.99
     * de reserva e o candidato host do WebRTC saía com esse IP, não o do Wi-Fi. */
    uint32_t v = netif_default ? u32_from_ipa(&netif_default->ip_addr) : 0;
    if (!v) v = u32_from_ipa(&netif.ip_addr);
    if (!v) return -1;
    *ip = v;
    return 0;
}

static int parse_v4(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned n = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || n > 255) return 0;
        }
        v = (v << 8) | n;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}

int xcw_resolve(const char *host, uint32_t *ip) {
    if (parse_v4(host, ip)) return 0;
    /* DNS lives on the network thread (net_pipe.c): calling it FROM the
     * network thread would wait for itself. */
    if (!g_resolver || on_net_thread()) return -1;
    return g_resolver(host, ip);
}

uint64_t xcw_now_ms(void) { return mftb() / (PPC_TIMEBASE_FREQ / 1000ULL); }

uint64_t xcw_now_us(void) {
    uint64_t tb = mftb();
    return (tb / PPC_TIMEBASE_FREQ) * 1000000ULL + (tb % PPC_TIMEBASE_FREQ) * 1000000ULL / PPC_TIMEBASE_FREQ;
}

void xcw_idle(int ms) {
    uint64_t end = xcw_now_ms() + (uint64_t)(ms > 0 ? ms : 0);
    void (*pump)(void) = g_pump;
    if (g_net_pir >= 0 && cur_pir() == g_net_pir && pump) {
        do {
            pump();   /* network_poll + xcw_net_step (+ whatever net_step does) */
            xc_polite_udelay(100);
        } while (xcw_now_ms() < end);
        return;
    }
    if (ms > 0) xc_polite_udelay(ms * 1000);
}

/* ------------------------------------------------------------- entropy */

/* Weak fallback for builds without source/login (bench ELF): timebase jitter
 * over random accesses to a 512 KB buffer, 16 samples per output byte. The
 * login's xc_port_xenon.c (strong symbol, 2 MB buffer) wins when linked. */
#define JIT_BUF (512 * 1024)
static uint8_t g_jit[JIT_BUF] __attribute__((aligned(128)));
static uint32_t g_jit_x = 0x9E3779B9u;

static uint32_t jitter_sample(void) {
    uint64_t t0 = mftb();
    uint32_t x = g_jit_x;
    for (int i = 0; i < 48; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        g_jit[x & (JIT_BUF - 1)] += (uint8_t)(i + (t0 & 0xFF));
    }
    uint64_t t1 = mftb();
    g_jit_x = x ^ (uint32_t)t1;
    return (uint32_t)(t1 - t0);
}

__attribute__((weak)) int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    for (size_t i = 0; i < len; i++) {
        uint32_t v = 0;
        for (int k = 0; k < 16; k++) {
            uint32_t d = jitter_sample();
            v = (v << 3 | v >> 29) ^ d ^ (d >> 8);
        }
        output[i] = (uint8_t)(v ^ v >> 8 ^ v >> 16 ^ v >> 24);
    }
    *olen = len;
    return 0;
}

static mbedtls_entropy_context g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static int g_drbg_ok;

void xcw_random(void *buf, size_t n) {
    if (!g_drbg_ok) {
        uint64_t tb = mftb();
        mbedtls_entropy_init(&g_ent);
        mbedtls_ctr_drbg_init(&g_drbg);
        if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent, (const unsigned char *)&tb, sizeof(tb)) == 0)
            g_drbg_ok = 1;
    }
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        size_t k = n > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : n;
        if (!g_drbg_ok || mbedtls_ctr_drbg_random(&g_drbg, p, k) != 0) {
            /* never fall back silently to something predictable */
            size_t ol;
            mbedtls_hardware_poll(NULL, p, k, &ol);
        }
        p += k;
        n -= k;
    }
}

void xcw_log(const char *line) {
    void (*hook)(const char *) = g_log_hook;
    if (hook)
        hook(line);
    else
        printf("%s\n", line);
}
