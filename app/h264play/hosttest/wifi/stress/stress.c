/*
 * orbita v0.21.8 — estresse do caminho de dados do Wi-Fi no host:
 *
 *   par TCP (servidor, em massa) -> "firmware" (RxPD, fila limitada, ZLP)
 *   -> controlador EHCI falso (fila assíncrona com a semântica do EHCI 4.10:
 *      overlay, qTD inativo segura a fila, pacote curto, ZLP, NAK, toggle)
 *   -> source/wifi/xw_ehs_xenon.c (o código do Xbox, anéis de qTD)
 *   -> source/wifi/xw_core.c -> lwIP 1.4.1 do libxenon (TCP, OOSEQ, pool)
 *   -> app com a política do net_pipe (buffer de 256 KB, ERR_MEM quando cheio)
 *   e o caminho de volta (ACKs) pelo EP2 OUT.
 *
 * O controlador roda no mesmo laço (modo determinístico, com ASan) ou numa
 * thread de verdade (modo concorrente: corridas entre o xhs e o "hardware").
 * Confere: dados completos e corretos, nenhum vazamento de pbuf/segmento TCP,
 * anéis consistentes, e o detector de RX parado (xw_rxwatch) com um EP2 IN
 * congelado de propósito.
 *
 * Uso: stress [det|thread] [pacotes] [semente]
 */
#include "xhs_host.h"
#include "xw_ehs_xenon.h"
#include "xw_core.h"
#include "xw_rxwatch.h"

#include <pthread.h>
#include <stdarg.h>
#include <sched.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/memp.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/tcp_impl.h"
#include "lwip/ip_addr.h"
#include "netif/etharp.h"

static int g_fail = 0, g_checks = 0, g_verbose = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------ plataforma */
static volatile uint64_t g_us = 1000000;
static int g_threaded = 0;
static void hc_pass(void);
uint64_t mftb(void) { return g_us; }
void udelay(int us) {
    g_us += (uint64_t)us;
    if (!g_threaded) hc_pass();
    else sched_yield();
}
static unsigned g_logs = 0;
void xc_log(const char *fmt, ...) {
    g_logs++;
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define ARENA_SZ (16u << 20)
#define DMA_BASE 0x01000000u
static uint8_t *g_arena;
static size_t   g_aoff;
void *xhs_host_memalign(size_t al, size_t n) {
    size_t o = (g_aoff + al - 1) & ~(al - 1);
    if (o + n > ARENA_SZ) { fprintf(stderr, "arena cheia\n"); abort(); }
    g_aoff = o + n;
    return g_arena + o;
}
uint32_t xhs_host_dma(const volatile void *p) { return DMA_BASE + (uint32_t)((const uint8_t *)p - g_arena); }
void *xhs_host_ptr(uint32_t d) { return g_arena + (d - DMA_BASE); }

/* ------------------------------------------------------------ "firmware" (fila do dispositivo) */
#define DQ 64
typedef struct { uint8_t b[1600]; int n; } msg_t;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static msg_t   g_rxq[DQ];          /* ar -> host (EP2 IN) */
static int     g_rxh, g_rxn;
static int     g_zlp_pending = 0;  /* a transferência anterior encheu o qTD: o ZLP cai no próximo */
static unsigned g_dev_drop = 0, g_in_xfers = 0, g_out_xfers = 0, g_zlps = 0, g_nak_out = 0;
static msg_t   g_txq[256];         /* host -> ar (EP2 OUT, já sem o TxPD) */
static int     g_txh, g_txn;
static int     g_freeze_din = 0;
static unsigned g_out_zlps = 0, g_out_mult64 = 0, g_out_hist[64], g_out_data = 0;
static int     g_tx_wedged = 0;   /* firmware que recebeu ZLP no EP2 OUT */   /* simula o QH do EP2 IN "esquecido" pelo controlador */
static unsigned g_rand = 1;
static unsigned rnd(void) { g_rand = g_rand * 1103515245u + 12345u; return (g_rand >> 8) & 0xffffff; }

static void dev_rx_push(const uint8_t *eth, int n, int snap) {
    pthread_mutex_lock(&g_mx);
    if (g_rxn >= DQ) { g_dev_drop++; pthread_mutex_unlock(&g_mx); return; }
    msg_t *m = &g_rxq[(g_rxh + g_rxn) % DQ];
    const int off = 26;
    memset(m->b, 0, off);
    int len;
    if (snap) {   /* 802.3 + LLC/SNAP (tipo 0) */
        memcpy(m->b + off, eth, 12);
        m->b[off + 12] = (uint8_t)((n - 14 + 8) >> 8);
        m->b[off + 13] = (uint8_t)(n - 14 + 8);
        memcpy(m->b + off + 14, "\xaa\xaa\x03\x00\x00\x00", 6);
        memcpy(m->b + off + 20, eth + 12, 2);
        memcpy(m->b + off + 22, eth + 14, (size_t)n - 14);
        len = n + 8;
        m->b[6] = 0;
    } else {      /* Ethernet II, tipo 2 (como o firmware 43.x entrega) */
        memcpy(m->b + off, eth, (size_t)n);
        len = n;
        m->b[6] = 2;
    }
    m->b[2] = (uint8_t)len; m->b[3] = (uint8_t)(len >> 8);
    m->b[4] = off; m->b[5] = 0;
    m->b[12] = 40; m->b[13] = (uint8_t)-90;
    m->n = off + len;
    g_rxn++;
    pthread_mutex_unlock(&g_mx);
}

/* ------------------------------------------------------------ EHCI falso */
struct ehci_hcd ehci_hcds[2];
static struct ehci_caps g_caps[2];
static struct ehci_regs g_regs[2];
static const uint8_t k_cfg[60] = {
    9, 2, 60, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 6, 0xff, 0xff, 0xff, 0,
    7, 5, 0x01, 2, 0, 2, 0,  7, 5, 0x81, 2, 0, 2, 0,
    7, 5, 0x02, 2, 0, 2, 0,  7, 5, 0x82, 2, 0, 2, 0,
    7, 5, 0x03, 2, 0, 2, 0,  7, 5, 0x83, 2, 0, 2, 0 };
static uint8_t g_setup[8];

#define T_ACT  (1u << 7)
#define T_HALT (1u << 6)
#define T_TOG  (1u << 31)

static void hc_complete(volatile uint32_t *qh, uint32_t tok, int moved, int mps) {
    volatile uint32_t *td = (volatile uint32_t *)xhs_host_ptr(qh[3]);
    uint32_t len = (tok >> 16) & 0x7fff;
    uint32_t rem = len - (uint32_t)moved;
    int pk = moved == 0 ? 1 : (moved + mps - 1) / mps;
    uint32_t nt = (tok & ~(T_ACT | (0x7fffu << 16))) | (rem << 16);
    if (pk & 1) nt ^= T_TOG;
    td[2] = nt;            /* escreve de volta no qTD... */
    qh[6] = nt;            /* ...e no overlay (inativo: na próxima passada segue o next) */
}

static void hc_qh(volatile uint32_t *qh) {
    uint32_t info1 = qh[1];
    int ep = (int)((info1 >> 8) & 15), dtc = (int)((info1 >> 14) & 1), mps = (int)((info1 >> 16) & 0x7ff);
    uint32_t tok = qh[6];
    if (tok & T_HALT) return;
    if (!(tok & T_ACT)) {
        uint32_t nx = qh[4];
        if (nx & 1) return;
        volatile uint32_t *td = (volatile uint32_t *)xhs_host_ptr(nx & ~0x1fu);
        uint32_t t = td[2];
        if (!(t & T_ACT)) return;          /* qTD inativo segura a fila (EHCI 4.10.2) */
        qh[3] = nx & ~0x1fu;
        qh[4] = td[0];
        qh[5] = td[1];
        for (int k = 0; k < 10; k++) qh[7 + k] = td[3 + k];
        if (!dtc) t = (t & ~T_TOG) | (tok & T_TOG);
        qh[6] = t;
        tok = t;
    }
    int pid = (int)((tok >> 8) & 3);
    int len = (int)((tok >> 16) & 0x7fff);
    uint8_t *buf = (uint8_t *)xhs_host_ptr(qh[7]);
    if (pid == 2) {                       /* SETUP */
        memcpy(g_setup, buf, 8);
        hc_complete(qh, tok, 8, mps);
        return;
    }
    if (ep == 0) {
        int n = 0;
        if (pid == 1 && len > 0 && g_setup[1] == 6) {   /* GET_DESCRIPTOR(config) */
            n = len < (int)sizeof(k_cfg) ? len : (int)sizeof(k_cfg);
            memcpy(buf, k_cfg, (size_t)n);
        }
        hc_complete(qh, tok, n, mps);
        return;
    }
    if (pid == 1) {                       /* IN */
        if (ep == 1 || ep == 3) return;   /* sem eventos/comandos no estresse: NAK */
        if (g_freeze_din) return;         /* QH "esquecido" (o detector tem de achar) */
        pthread_mutex_lock(&g_mx);
        if (g_zlp_pending) {
            g_zlp_pending = 0;
            g_zlps++;
            pthread_mutex_unlock(&g_mx);
            hc_complete(qh, tok, 0, mps);
            return;
        }
        if (!g_rxn) { pthread_mutex_unlock(&g_mx); return; }   /* NAK */
        msg_t *m = &g_rxq[g_rxh];
        if (m->n > len) {                 /* babble: não deve acontecer (4 KB) */
            pthread_mutex_unlock(&g_mx);
            qh[6] = tok | T_HALT;
            return;
        }
        memcpy(buf, m->b, (size_t)m->n);
        int n = m->n;
        if (n % mps == 0 && n == len) g_zlp_pending = 1;   /* encheu o qTD: o ZLP vai pro próximo */
        if (n % mps == 0 && n < len) g_zlps++;            /* múltiplo de 512: o ZLP fecha este */
        g_rxh = (g_rxh + 1) % DQ;
        g_rxn--;
        g_in_xfers++;
        pthread_mutex_unlock(&g_mx);
        hc_complete(qh, tok, n, mps);
        return;
    }
    /* OUT */
    if (ep == 2) {
        pthread_mutex_lock(&g_mx);
        if (g_txn >= 256 || (rnd() % 100) < 3) { g_nak_out++; pthread_mutex_unlock(&g_mx); return; }   /* NAK/PING */
        /* v0.21.8: o firmware da flash não espera ZLP no EP2 OUT: depois de um,
         * aceita as transferências pelo USB mas não transmite mais nada (o que
         * os logs do console mostraram: TX conclui no USB, RX só pinga
         * retransmissões do outro lado). */
        if (len == 0) { g_out_zlps++; g_tx_wedged = 1; }
        if (len > 0 && len % 64 == 0) g_out_mult64++;
        if (len > 20 + 54) { g_out_data++; g_out_hist[len % 64]++; }
        if (len > 0 && !g_tx_wedged) {
            int off = buf[4] | (buf[5] << 8), pl = buf[2] | (buf[3] << 8);
            if (off >= 20 && off + pl <= len && pl <= 1514) {
                msg_t *m = &g_txq[(g_txh + g_txn) % 256];
                memcpy(m->b, buf + off, (size_t)pl);
                m->n = pl;
                g_txn++;
            }
            g_out_xfers++;
        }
        pthread_mutex_unlock(&g_mx);
    }
    hc_complete(qh, tok, len, mps);
}

static void hc_pass(void) {
    struct ehci_qh *a = ehci_hcds[1].async;
    uint32_t link = a->hw_next;
    for (int guard = 0; guard < 16; guard++) {
        uint32_t qd = link & ~0x1fu;
        if (qd == xhs_host_dma(a)) break;
        volatile uint32_t *qh = (volatile uint32_t *)xhs_host_ptr(qd);
        hc_qh(qh);
        link = qh[0];
    }
}

static volatile int g_hc_run = 0;
static void *hc_thread(void *u) {
    (void)u;
    while (g_hc_run) { hc_pass(); sched_yield(); }
    return NULL;
}

static void ehci_setup(void) {
    for (int b = 0; b < 2; b++) {
        struct ehci_hcd *h = &ehci_hcds[b];
        memset(h, 0, sizeof(*h));
        h->caps = &g_caps[b];
        h->regs = &g_regs[b];
        h->num_port = 5;
        g_caps[b].hcs_params = 0x1105;
        g_regs[b].command = 0x21;
        h->async = (struct ehci_qh *)xhs_host_memalign(64, sizeof(struct ehci_qh));
        memset(h->async, 0, sizeof(struct ehci_qh));
        h->async->hw_next = xhs_host_dma(h->async) | 2;
        h->async->hw_info1 = 1u << 15;
        h->async->hw_token = T_HALT;
    }
    g_regs[1].port_status[3] = 0x1005;
    struct ehci_device *d = &ehci_hcds[1].devices[3];
    d->desc.idVendor = bswap_16(0x045e);
    d->desc.idProduct = bswap_16(0x0765);
    d->id = 4;
}

/* ------------------------------------------------------------ par TCP (servidor) */
static const uint8_t HOST_MAC[6] = { 0x00, 0x22, 0x48, 0x01, 0x02, 0x03 };
static const uint8_t PEER_MAC[6] = { 0x84, 0x0b, 0xbb, 0x10, 0x20, 0x30 };
#define HOST_IP 0x0a000002u
#define PEER_IP 0x0a000001u

static uint16_t csum(const uint8_t *p, int n, uint32_t s) {
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint8_t pattern(int conn, uint64_t i) { return (uint8_t)(i * 31u + (i >> 9) + (uint64_t)conn * 7u); }

typedef struct {
    int      used, state, conn;     /* 0 nada, 1 SYN_RCVD, 2 ESTAB, 3 FIN enviado, 4 fechado */
    uint16_t lport, rport;
    uint32_t iss, rcv_nxt, snd_una, snd_nxt;
    uint32_t wnd;
    uint64_t total;
    uint64_t t_progress;
    uint64_t rto;                   /* RTO com recuo exponencial (300 ms .. 8 s), como um TCP de verdade */
} peer_t;
#define NPEER 4
static peer_t g_peer[NPEER];
static uint64_t g_bytes_total = 0;
static unsigned g_seg_sizes[8];

static void peer_send(peer_t *c, uint32_t seq, const uint8_t *data, int dn, uint8_t flags) {
    uint8_t f[1600];
    memcpy(f, HOST_MAC, 6);
    memcpy(f + 6, PEER_MAC, 6);
    f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14, *tcp = ip + 20;
    memset(ip, 0, 40);
    ip[0] = 0x45; put16(ip + 2, (unsigned)(40 + dn)); ip[8] = 64; ip[9] = 6;
    put32(ip + 12, PEER_IP); put32(ip + 16, HOST_IP);
    put16(ip + 10, csum(ip, 20, 0));
    put16(tcp, c->lport); put16(tcp + 2, c->rport);
    put32(tcp + 4, seq); put32(tcp + 8, c->rcv_nxt);
    tcp[12] = 5 << 4; tcp[13] = flags; put16(tcp + 14, 65535);
    if (dn) memcpy(tcp + 20, data, (size_t)dn);
    uint8_t ph[12];
    put32(ph, PEER_IP); put32(ph + 4, HOST_IP); ph[8] = 0; ph[9] = 6; put16(ph + 10, (unsigned)(20 + dn));
    uint32_t s = 0;
    for (int i = 0; i < 12; i += 2) s += (uint32_t)((ph[i] << 8) | ph[i + 1]);
    put16(tcp + 16, csum(tcp, 20 + dn, s));
    int n = 54 + dn;
    if (n < 60) { memset(f + n, 0, (size_t)(60 - n)); n = 60; }
    int snap = (rnd() % 8) == 0;
    int loss = dn && (rnd() % 1000) < 8;   /* 0,8% de perda no ar */
    if (!loss) dev_rx_push(f, n, snap);
    if (dn && (rnd() % 1000) < 2) dev_rx_push(f, n, snap);   /* duplicado */
}

/* tamanhos: miúdos, 512/1024/1536 exatos no USB (RxPD 26 + 54 de cabeçalhos), cheios */
static int pick_size(void) {
    static const int k[8] = { 6, 432, 944, 1456, 1460, 1460, 1000, 300 };
    unsigned r = rnd() % 16;
    int i = r < 8 ? (int)r : 4;
    g_seg_sizes[i]++;
    if (i == 6) return 1 + (int)(rnd() % 1460);
    return k[i];
}

static void peer_tick(void) {
    uint64_t now = g_us;
    for (int i = 0; i < NPEER; i++) {
        peer_t *c = &g_peer[i];
        if (!c->used || c->state != 2) continue;
        uint32_t end = c->iss + 1 + (uint32_t)c->total;
        uint32_t limit = c->snd_una + (c->wnd < 48000 ? c->wnd : 48000);
        int burst = 0;
        while (c->snd_nxt != end && (int32_t)(limit - c->snd_nxt) > 0 && burst < 6) {
            int dn = pick_size();
            if ((int32_t)(end - c->snd_nxt) < dn) dn = (int)(end - c->snd_nxt);
            if ((int32_t)(limit - c->snd_nxt) < dn) dn = (int)(limit - c->snd_nxt);
            if (dn <= 0) break;
            uint8_t d[1460];
            uint64_t base = (uint64_t)(c->snd_nxt - (c->iss + 1));
            for (int k = 0; k < dn; k++) d[k] = pattern(c->conn, base + (uint64_t)k);
            peer_send(c, c->snd_nxt, d, dn, 0x18);
            c->snd_nxt += (uint32_t)dn;
            burst++;
        }
        if (c->snd_una == end) {
            peer_send(c, end, NULL, 0, 0x11);   /* FIN */
            c->snd_nxt = end + 1;
            c->state = 3;
        } else if (now - c->t_progress > (c->rto ? c->rto : 300000)) {   /* RTO: volta pro primeiro sem ACK */
            c->snd_nxt = c->snd_una;
            c->t_progress = now;
            c->rto = c->rto ? (c->rto * 2 > 8000000 ? 8000000 : c->rto * 2) : 600000;
        }
    }
}

static void peer_rx(const uint8_t *f, int n) {
    if (n >= 42 && f[12] == 0x08 && f[13] == 0x06 && f[21] == 1) {   /* ARP request */
        uint8_t r[60];
        memset(r, 0, sizeof(r));
        memcpy(r, f + 6, 6); memcpy(r + 6, PEER_MAC, 6); r[12] = 8; r[13] = 6;
        r[14] = 0; r[15] = 1; r[16] = 8; r[17] = 0; r[18] = 6; r[19] = 4; r[20] = 0; r[21] = 2;
        memcpy(r + 22, PEER_MAC, 6); put32(r + 28, PEER_IP); memcpy(r + 32, f + 22, 6); memcpy(r + 38, f + 28, 4);
        dev_rx_push(r, 60, 0);
        return;
    }
    if (n < 54 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 6) return;
    const uint8_t *ip = f + 14, *tcp = ip + (ip[0] & 15) * 4;
    int iplen = (ip[2] << 8) | ip[3];
    int dlen = iplen - (ip[0] & 15) * 4 - (tcp[12] >> 4) * 4;
    uint16_t sport = (uint16_t)((tcp[0] << 8) | tcp[1]), dport = (uint16_t)((tcp[2] << 8) | tcp[3]);
    uint8_t fl = tcp[13];
    peer_t *c = NULL;
    for (int i = 0; i < NPEER; i++)
        if (g_peer[i].used && g_peer[i].rport == sport && g_peer[i].lport == dport) c = &g_peer[i];
    if (g_verbose && (fl & 0x07)) printf("    [par] t=%llu ms: %s%s%s porta %u -> %u, %s\n", (unsigned long long)(g_us / 1000),
                                        (fl & 2) ? "SYN " : "", (fl & 1) ? "FIN " : "", (fl & 4) ? "RST " : "", sport, dport,
                                        c ? "conexão conhecida" : "sem conexão");
    if (fl & 0x02) {   /* SYN do host */
        if (!c) for (int i = 0; i < NPEER && !c; i++) if (!g_peer[i].used) c = &g_peer[i];
        if (!c) return;
        if (!c->used) {
            memset(c, 0, sizeof(*c));
            c->used = 1;
            c->conn = sport;   /* o padrão dos dados depende da porta do host */
            c->lport = dport;
            c->rport = sport;
            c->iss = 0x10000000u + (uint32_t)rnd();
            c->total = g_bytes_total;
        }
        c->rcv_nxt = get32(tcp + 4) + 1;
        c->state = 1;
        uint8_t opt[4] = { 2, 4, (uint8_t)(1460 >> 8), (uint8_t)1460 };
        (void)opt;
        peer_send(c, c->iss, NULL, 0, 0x12);   /* SYN-ACK (MSS padrão 536? o lwIP usa o próprio) */
        c->snd_una = c->snd_nxt = c->iss + 1;
        return;
    }
    if (!c) return;
    if (fl & 0x10) {   /* ACK */
        uint32_t ack = get32(tcp + 8);
        c->wnd = (uint32_t)((tcp[14] << 8) | tcp[15]);
        if (c->state == 1) { c->state = 2; c->t_progress = g_us; }
        if ((int32_t)(ack - c->snd_una) > 0 && (int32_t)(ack - c->snd_nxt) <= 0) { c->snd_una = ack; c->t_progress = g_us; c->rto = 0; }
        else if ((int32_t)(ack - c->snd_una) > 0 && c->state == 3) { c->snd_una = ack; }
    }
    if (dlen > 0) c->rcv_nxt += (uint32_t)dlen;
    if (fl & 0x01) {   /* FIN do host */
        c->rcv_nxt += 1;
        peer_send(c, c->snd_nxt, NULL, 0, 0x10);
        c->state = 4;
        c->used = 0;
    }
}

/* ------------------------------------------------------------ host: core + lwIP + app */
static xw_core_t g_core;
static struct netif g_wl;
static unsigned g_pbuf_fail = 0, g_rx_frames = 0;

/* v0.21.13: a bomba do xcw_idle chamada de dentro do caminho do RX (o que
 * reentrava o colhedor no console): com g_reenter, de vez em quando o RX
 * chama o poll de novo, como a bomba faria */
static int g_reenter = 0;
int xhs_test_pir = 1;   /* v0.21.17: a thread do poll */
static unsigned g_reenter_calls = 0;
static void hs_rx(int ep, const uint8_t *b, size_t n, void *u) {
    (void)u;
    static uint8_t copy[4096];
    static int depth = 0;
    if (g_reenter && depth == 0 && (rnd() % 20) == 0) {
        depth = 1;
        g_reenter_calls++;
        xhs_poll(hs_rx, NULL);
        depth = 0;
    }
    memcpy(copy, b, n);
    if (ep == 2 || ep == 3) xw_on_data_in(&g_core, copy, n, (uint32_t)(g_us / 1000));
    else xw_on_cmd_in(&g_core, copy, n, (uint32_t)(g_us / 1000));
}
static int op_send_cmd(void *u, const uint8_t *b, size_t n) { (void)u; return xhs_send(1, b, n); }
static int op_send_data(void *u, const uint8_t *b, size_t n) { (void)u; return xhs_send(2, b, n); }
static void op_rx_eth(void *u, uint8_t *f, size_t n) {
    (void)u;
    g_rx_frames++;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
    if (!p) { g_pbuf_fail++; return; }
    pbuf_take(p, f, (u16_t)n);
    if (ethernet_input(p, &g_wl) != ERR_OK) pbuf_free(p);
}
static void op_link(void *u, int up) { (void)u; (void)up; }
static void op_log(void *u, const char *l) { (void)u; if (g_verbose) printf("%s\n", l); }
static int op_rand(void *u, uint8_t *b, size_t n) { (void)u; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(); return 0; }
static const xw_ops_t OPS = { op_send_cmd, op_send_data, op_rx_eth, op_link, op_log, op_rand, NULL };

static err_t wl_out(struct netif *nf, struct pbuf *p) {
    (void)nf;
    static uint8_t f[1600];
    if (p->tot_len > sizeof(f)) return ERR_IF;
    pbuf_copy_partial(p, f, p->tot_len, 0);
    return xw_tx_eth(&g_core, f, p->tot_len) == 0 ? ERR_OK : ERR_IF;
}
static err_t wl_init(struct netif *nf) {
    nf->name[0] = 'w'; nf->name[1] = 'l';
    nf->output = etharp_output;
    nf->linkoutput = wl_out;
    nf->hwaddr_len = 6;
    memcpy(nf->hwaddr, HOST_MAC, 6);
    nf->mtu = 1500;
    nf->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

/* app com a política do net_pipe: buffer de 256 KB, ERR_MEM quando cheio */
#define APPBUF (256 * 1024)
typedef struct {
    struct tcp_pcb *pcb;
    int conn, state;       /* 0 livre, 1 conectando, 2 aberta, 3 fim, 4 erro */
    uint64_t got, verified;
    size_t len;
    uint8_t *buf;
    unsigned errmem, bad;
} app_t;
static app_t g_app[NPEER];
static int g_stall_consumer = 0;

static err_t app_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    app_t *a = (app_t *)arg;
    if (!p) { a->state = 3; tcp_arg(pcb, NULL); tcp_recv(pcb, NULL); tcp_close(pcb); a->pcb = NULL; return ERR_OK; }
    if (err != ERR_OK) { pbuf_free(p); return err; }
    if (APPBUF - a->len < p->tot_len) { a->errmem++; return ERR_MEM; }
    pbuf_copy_partial(p, a->buf + a->len, p->tot_len, 0);
    a->len += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static void app_err(void *arg, err_t err) { app_t *a = (app_t *)arg; if (a) { a->state = 4; a->pcb = NULL; } (void)err; }
/* conectou: manda um "pedido" de tamanho variado (como o HTTP/TLS do app),
 * pra o TX do host ter todos os tamanhos de quadro */
static int g_req_rem = -1;   /* >= 0: resto fixo do pedido (fase 2b) */
static err_t app_conn(void *arg, struct tcp_pcb *pcb, err_t err) {
    app_t *a = (app_t *)arg;
    if (err) return ERR_OK;
    a->state = 2;
    static uint8_t req[2048];
    for (int i = 0; i < 2048; i++) req[i] = (uint8_t)i;
    /* o par não manda opção de MSS, então o lwIP corta em 536: o último
     * segmento fica com resto r. Metade das vezes r ≡ 54 (mod 64), que com
     * TxPD de 20 daria transferência múltipla de 64 (r = 438: 512 bytes, ZLP
     * no EP2 OUT, o caso do POST do log no console). */
    u16_t r;
    if (g_req_rem >= 0) r = (u16_t)g_req_rem;
    else if (rnd() % 2) r = (u16_t)(54 + 64 * (rnd() % 8));
    else r = (u16_t)(1 + rnd() % 535);
    u16_t n = (u16_t)(536 * (rnd() % 3) + r);
    if (tcp_write(pcb, req, n, TCP_WRITE_FLAG_COPY) == ERR_OK) tcp_output(pcb);
    return ERR_OK;
}

static void app_open(int i) {
    app_t *a = &g_app[i];
    a->pcb = tcp_new();
    a->state = 1;
    a->got = a->verified = 0;
    a->len = 0;
    tcp_arg(a->pcb, a);
    tcp_recv(a->pcb, app_recv);
    tcp_err(a->pcb, app_err);
    ip_addr_t peer;
    IP4_ADDR(&peer, 10, 0, 0, 1);
    tcp_connect(a->pcb, &peer, 80, app_conn);
    a->conn = a->pcb->local_port;
}

/* consumidor: lê o buffer e confere o padrão */
static void app_drain(void) {
    for (int i = 0; i < NPEER; i++) {
        app_t *a = &g_app[i];
        if (!a->len) continue;
        if (g_stall_consumer && (rnd() % 4)) continue;
        size_t take = a->len > 96 * 1024 ? 96 * 1024 : a->len;
        for (size_t k = 0; k < take; k++)
            if (a->buf[k] != pattern(a->conn, a->got + k)) { a->bad++; break; }
        a->got += take;
        memmove(a->buf, a->buf + take, a->len - take);
        a->len -= take;
    }
}

static int count_pool(void) {
    struct pbuf *l[2048];
    int n = 0;
    while (n < 2048 && (l[n] = pbuf_alloc(PBUF_RAW, 1, PBUF_POOL)) != NULL) n++;
    for (int i = 0; i < n; i++) pbuf_free(l[i]);
    return n;
}
static int count_segs(void) {
    void *l[1024];
    int n = 0;
    while (n < 1024 && (l[n] = memp_malloc(MEMP_TCP_SEG)) != NULL) n++;
    for (int i = 0; i < n; i++) memp_free(MEMP_TCP_SEG, l[i]);
    return n;
}

/* ------------------------------------------------------------ laço (= hw1, 1 ms por volta) */
static xrw_t g_rw;
static unsigned g_rw_fires = 0;
static uint64_t g_tcp_tmr = 0, g_arp_tmr = 0;

static unsigned g_desync_every = 0, g_desyncs = 0;   /* v0.21.11 */
/* v0.21.20: uma porta da frente do EHCI 1 (índice 1) com hotplug, modelo da
 * PORTSC (RWC, reset, PE de high speed, PORT_OWNER devolvido na desconexão) */
enum { FD_NONE = 0, FD_LS, FD_FS, FD_HS };
static struct { int dev, csc, reset, enabled, po; } g_fp;
static void front_connect(int dev) { g_fp.dev = dev; g_fp.csc = 1; g_fp.enabled = 0; g_fp.po = 0; g_fp.reset = 0; }
static void front_disconnect(void) { g_fp.dev = FD_NONE; g_fp.csc = 1; g_fp.enabled = 0; g_fp.po = 0; g_fp.reset = 0; }
static void front_model(void) {
    volatile uint32_t *r = &ehci_hcds[1].regs->port_status[1];
    uint32_t w = *r;   /* o que o driver escreveu */
    if ((w & 0x2u) && g_fp.csc) g_fp.csc = 0;            /* CSC é RWC: escrever 1 limpa */
    int pr = (w & 0x100u) != 0;
    int ended = g_fp.reset && !pr;
    if (ended) g_fp.enabled = g_fp.dev == FD_HS;   /* fim do reset: o HC habilita só high speed */
    g_fp.reset = pr && g_fp.dev != FD_NONE;
    if (!ended && !(w & 0x4u) && !g_fp.reset) g_fp.enabled = 0;   /* escrever PE=0 (depois) desabilita */
    if ((w & 0x2000u) && g_fp.dev != FD_NONE) g_fp.po = 1;
    uint32_t v = 0x1000u;
    if (g_fp.dev != FD_NONE && !g_fp.po) {
        v |= 0x1u;
        v |= g_fp.dev == FD_LS ? 0x400u : 0x800u;
        if (g_fp.enabled) v |= 0x4u;
    }
    if (g_fp.reset) v |= 0x100u;
    if (g_fp.po) v |= 0x2000u;
    if (g_fp.csc) v |= 0x2u;
    *r = v;
}
/* v0.21.12: começo do stream: UDP de 1000-1400 B (RTP) a g_udp_rate por ms */
static unsigned g_udp_rate = 0, g_udp_sent = 0, g_udp_rx = 0, g_udp_dup = 0, g_udp_reord = 0, g_udp_bad = 0;
static uint32_t g_udp_seq = 0, g_udp_last = 0;
static struct udp_pcb *g_udp_pcb = NULL;
static void udp_rx_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;
    uint8_t h[8];
    if (pbuf_copy_partial(p, h, 8, 0) == 8) {
        uint32_t q = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
        uint32_t len = ((uint32_t)h[4] << 8) | h[5];
        if (len != p->tot_len) g_udp_bad++;
        if (g_udp_rx && q == g_udp_last) g_udp_dup++;
        else if (g_udp_rx && (int32_t)(q - g_udp_last) < 0) g_udp_reord++;
        g_udp_last = q;
        g_udp_rx++;
    }
    pbuf_free(p);
}
static void udp_flood_tick(void) {
    for (unsigned k = 0; k < g_udp_rate; k++) {
        uint8_t f[1600];
        int dn = 1000 + (int)(rnd() % 401);
        memcpy(f, HOST_MAC, 6);
        memcpy(f + 6, PEER_MAC, 6);
        f[12] = 0x08; f[13] = 0x00;
        uint8_t *ip = f + 14, *udp = ip + 20, *d = udp + 8;
        memset(ip, 0, 28);
        ip[0] = 0x45; put16(ip + 2, (unsigned)(28 + dn)); ip[8] = 64; ip[9] = 17;
        put32(ip + 12, PEER_IP); put32(ip + 16, HOST_IP);
        put16(ip + 10, csum(ip, 20, 0));
        put16(udp, 3478); put16(udp + 2, 50000); put16(udp + 4, (unsigned)(8 + dn)); put16(udp + 6, 0);
        uint32_t q = ++g_udp_seq;
        d[0] = (uint8_t)(q >> 24); d[1] = (uint8_t)(q >> 16); d[2] = (uint8_t)(q >> 8); d[3] = (uint8_t)q;
        d[4] = (uint8_t)(dn >> 8); d[5] = (uint8_t)dn;
        for (int i = 6; i < dn; i++) d[i] = (uint8_t)(q + (uint32_t)i);
        dev_rx_push(f, 42 + dn, 0);
        g_udp_sent++;
    }
}
static void step_ms(void) {
    front_model();
    xhs_port_watch();
    front_model();
    if (g_udp_rate) udp_flood_tick();
    if (g_desync_every && (g_us / 1000) % g_desync_every == 0) {
        /* a dessincronia do console: o software acha o anel vazio (prod == cons) */
        xhs_test_desync(3);
        g_desyncs++;
    }
    g_us += 1000;
    if (!g_threaded) { int k = (int)(rnd() % 5); for (int i = 0; i < k; i++) hc_pass(); }
    uint32_t now = (uint32_t)(g_us / 1000);
    xhs_poll(hs_rx, NULL);
    xw_tick(&g_core, now);
    xhs_poll(hs_rx, NULL);
    if (xrw_step(&g_rw, now, g_core.link == XW_LINK_UP, xhs_rx_count(), xhs_tx_done(2))) {
        g_rw_fires++;
        char o[600];
        for (int w = 1; w < xhs_npipes(); w++) if (xhs_dump_pipe(w, o, sizeof(o)) && g_verbose) printf("  %s\n", o);
        if (g_verbose) {
            printf("  [t=%llu ms] rx=%u tx=%u fila do firmware=%d freeze=%d\n", (unsigned long long)(g_us / 1000), xhs_rx_count(),
                   xhs_tx_done(2), g_rxn, g_freeze_din);
            for (int i = 0; i < NPEER; i++)
                if (g_peer[i].used) printf("    par %d: estado %d una=%u nxt=%u wnd=%u fim=%u\n", i, g_peer[i].state,
                                           g_peer[i].snd_una - g_peer[i].iss, g_peer[i].snd_nxt - g_peer[i].iss, g_peer[i].wnd,
                                           (unsigned)g_peer[i].total + 1);
            for (int i = 0; i < NPEER; i++)
                if (g_app[i].pcb) printf("    app %d: estado %d buf=%zu got=%llu errmem=%u pcb st=%d rcv_wnd=%u refused=%p\n", i,
                                         g_app[i].state, g_app[i].len, (unsigned long long)g_app[i].got, g_app[i].errmem,
                                         g_app[i].pcb->state, g_app[i].pcb->rcv_wnd, (void *)g_app[i].pcb->refused_data);
        }
        int stuck = xhs_rx_rearm(3);
        if (g_verbose) printf("  RX parado: rearmado (%d presos)\n", stuck);
        g_freeze_din = 0;
    }
    if (g_us - g_tcp_tmr >= 250000) { g_tcp_tmr = g_us; tcp_tmr(); }
    if (g_us - g_arp_tmr >= 5000000) { g_arp_tmr = g_us; etharp_tmr(); }
    /* o que o host mandou chega no "ar" */
    for (;;) {
        msg_t m;
        pthread_mutex_lock(&g_mx);
        if (!g_txn) { pthread_mutex_unlock(&g_mx); break; }
        m = g_txq[g_txh];
        g_txh = (g_txh + 1) % 256;
        g_txn--;
        pthread_mutex_unlock(&g_mx);
        peer_rx(m.b, m.n);
    }
    peer_tick();
    app_drain();
}

static int run_transfers(int nconn, uint64_t bytes, int parallel, uint64_t max_ms, const char *name) {
    g_bytes_total = bytes;
    memset(g_peer, 0, sizeof(g_peer));
    int done_conns = 0, started = 0;
    uint64_t t0 = g_us;
    while (done_conns < nconn && (g_us - t0) / 1000 < max_ms) {
        for (int i = 0; i < parallel; i++)
            if (g_app[i].state == 0 || g_app[i].state == 3 || g_app[i].state == 4) {
                if (g_app[i].state == 3 || g_app[i].state == 4) {
                    app_t *a = &g_app[i];
                    CHECK(a->state == 3 && a->bad == 0 && a->got + a->len == bytes,
                          "%s: conexão %d: estado %d, %llu de %llu bytes, %u divergências", name, a->conn, a->state,
                          (unsigned long long)(a->got + a->len), (unsigned long long)bytes, a->bad);
                    done_conns++;
                    a->state = 0;
                }
                if (started < nconn) {
                    app_open(i);
                    started++;
                }
            }
        step_ms();
    }
    for (int k = 0; k < 3000; k++) {   /* drena os que acabaram por último */
        step_ms();
        int all = 1;
        for (int i = 0; i < parallel; i++) if (g_app[i].state == 1 || g_app[i].state == 2) all = 0;
        if (all) break;
    }
    for (int i = 0; i < parallel; i++)
        if (g_app[i].state == 3 || g_app[i].state == 4) {
            app_t *a = &g_app[i];
            CHECK(a->state == 3 && a->bad == 0 && a->got + a->len == bytes, "%s: conexão %d (fim): estado %d, %llu bytes, %u divergências",
                  name, a->conn, a->state, (unsigned long long)(a->got + a->len), a->bad);
            done_conns++;
            a->state = 0;
        }
    CHECK(done_conns == nconn, "%s: %d de %d conexões terminaram em %llu ms", name, done_conns, nconn,
          (unsigned long long)((g_us - t0) / 1000));
    return done_conns;
}

int main(int argc, char **argv) {
    g_threaded = argc > 1 && !strcmp(argv[1], "thread");
    long target = argc > 2 ? atol(argv[2]) : 120000;
    g_rand = argc > 3 ? (unsigned)atol(argv[3]) : 7;
    g_verbose = getenv("STRESS_VERBOSE") != NULL;
    g_arena = aligned_alloc(4096, ARENA_SZ);
    memset(g_arena, 0, ARENA_SZ);
    ehci_setup();
    if (g_threaded) { g_hc_run = 1; pthread_t th; pthread_create(&th, NULL, hc_thread, NULL); pthread_detach(th); }

    xhs_eps_t eps;
    CHECK(xhs_open(&eps) == 0, "xhs_open: %s", xhs_why());
    CHECK(eps.din == 0x82 && eps.dout == 0x02 && eps.d3in == 0x83, "endpoints");
    xw_init(&g_core, &OPS);
    g_core.dev = XW_DEV_READY;
    g_core.link = XW_LINK_UP;
    memcpy(g_core.mac, HOST_MAC, 6);
    memcpy(g_core.cur.bssid, PEER_MAC, 6);

    lwip_init();
    ip_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 10, 0, 0, 2); IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 10, 0, 0, 1);
    netif_add(&g_wl, &ip, &mask, &gw, NULL, wl_init, ethernet_input);
    netif_set_default(&g_wl);
    netif_set_up(&g_wl);
    for (int i = 0; i < NPEER; i++) g_app[i].buf = malloc(APPBUF);
    xrw_init(&g_rw, (uint32_t)(g_us / 1000));

    int pool0 = count_pool(), segs0 = count_segs();
    printf("--- %s: alvo %ld pacotes RX\n", g_threaded ? "controlador numa thread" : "determinístico", target);

    /* v0.21.16: QH/qTD em linhas de cache de 128 B só deles */
    {
        char w[120];
        CHECK(xhs_dma_layout_ok(w, sizeof(w)) == 0, "QH/qTD alinhados em 128: %s", w);
    }
    /* v0.21.16: DIAG overlay=0 (o poll não lê o overlay): o caminho de dados anda igual */
    xhs_diag_overlay = 0;
    {
        unsigned ov0 = xhs_overlay_reads();
        run_transfers(2, 512 * 1024, 2, 120000, "sem overlay");
        CHECK(xhs_overlay_reads() == ov0, "overlay=0: nenhuma leitura do overlay (%u)", xhs_overlay_reads() - ov0);
    }
    xhs_diag_overlay = 1;

    /* 1) volume: transferências em massa, 2 em paralelo, consumidor às vezes lento (ERR_MEM) */
    int rounds = 0;
    while ((long)g_in_xfers < target && rounds < 400) {
        g_stall_consumer = rounds % 3 == 1;
        run_transfers(2, 2u * 1024 * 1024 + (rnd() % 100000), 2, 120000, "volume");
        rounds++;
        if (g_fail) break;
    }
    printf("  RX %u transferências USB (%u ZLP), TX %u, NAK OUT %u, perdas na fila do firmware %u, quadros na pilha %u, "
           "pbuf_alloc falhou %u\n", g_in_xfers, g_zlps, g_out_xfers, g_nak_out, g_dev_drop, g_rx_frames, g_pbuf_fail);
    printf("  segmentos por tamanho: 6=%u 432=%u 944=%u 1456=%u 1460=%u misto=%u 300=%u\n", g_seg_sizes[0], g_seg_sizes[1],
           g_seg_sizes[2], g_seg_sizes[3], g_seg_sizes[4] + g_seg_sizes[5], g_seg_sizes[6], g_seg_sizes[7]);
    printf("  TX com dados %u; restos mod 64:", g_out_data);
    for (int q = 0; q < 64; q++) if (g_out_hist[q]) printf(" %d:%u", q, g_out_hist[q]);
    printf("\n");
    CHECK((long)g_in_xfers >= target, "pelo menos %ld pacotes RX (%u)", target, g_in_xfers);
    CHECK(g_zlps > 100, "ZLPs do dispositivo (RX) exercitados (%u)", g_zlps);
    CHECK(g_out_zlps == 0 && g_out_mult64 == 0 && !g_tx_wedged,
          "nenhum ZLP/múltiplo de 64 no EP2 OUT (ZLP %u, múltiplos %u): o firmware não trava o TX", g_out_zlps, g_out_mult64);
    CHECK(g_core.tx_padded > 0, "quadros com TxPD de 24 (pad) exercitados (%u)", g_core.tx_padded);
    CHECK(g_rw_fires == 0, "detector de RX parado não dispara com tráfego normal (%u)", g_rw_fires);
    CHECK(xhs_resyncs() == 0, "nenhuma ressincronia com o anel normal (%u)", xhs_resyncs());
    char why[200];
    CHECK(xhs_check(why, sizeof(why)) == 0, "anéis consistentes: %s", why);

    /* 1b) v0.21.11: o estado do console (EP2 IN com prod == cons e o HC
     * parado no segurador com os outros qTDs concluídos): a ressincronia
     * entrega tudo em ordem sem o detector nem reassociação */
    {
        unsigned rs0 = xhs_resyncs(), f0 = g_rw_fires;
        g_stall_consumer = 0;
        g_desync_every = 1997;
        run_transfers(4, 1536 * 1024 + (rnd() % 50000), 2, 120000, "dessincronia");
        g_desync_every = 0;
        for (int i = 0; i < 1000; i++) step_ms();   /* a última provocada se acerta (>= 100 ms) */
        printf("  dessincronia: %u provocadas, %u ressincronias, detector %u\n", g_desyncs, xhs_resyncs() - rs0,
               g_rw_fires - f0);
        CHECK(g_desyncs >= 3 && xhs_resyncs() - rs0 >= g_desyncs / 2,
              "ressincronizou (%u provocadas, %u ressincronias)", g_desyncs, xhs_resyncs() - rs0);
        CHECK(g_rw_fires == f0, "o detector não precisou disparar (%u)", g_rw_fires - f0);
        CHECK(xhs_check(why, sizeof(why)) == 0, "anéis consistentes depois das ressincronias: %s", why);
    }

    /* 1c) v0.21.12: começo do stream (o congelamento da v0.21.11): UDP de
     * 1000-1400 B em rajada (a fila do "firmware" enche e descarta, como no
     * ar), o HC numa thread no modo thread. Nenhuma ressincronia falsa,
     * nada duplicado nem fora de ordem, o detector quieto. */
    {
        unsigned rs0 = xhs_resyncs(), f0 = g_rw_fires, d0 = g_dev_drop;
        g_udp_pcb = udp_new();
        udp_bind(g_udp_pcb, IP_ADDR_ANY, 50000);
        udp_recv(g_udp_pcb, udp_rx_cb, NULL);
        g_udp_rate = 4;
        for (int i = 0; i < 15000; i++) step_ms();
        g_udp_rate = 0;
        for (int i = 0; i < 200; i++) step_ms();
        unsigned lost_dev = g_dev_drop - d0;
        printf("  rajada UDP: %u enviados, %u recebidos, %u descartados na fila do firmware, dup %u, fora de ordem %u, "
               "suspeitas %u, ressincronias %u\n", g_udp_sent, g_udp_rx, lost_dev, g_udp_dup, g_udp_reord,
               xhs_resync_suspects(), xhs_resyncs() - rs0);
        CHECK(g_udp_rx + lost_dev >= g_udp_sent * 99 / 100 && g_udp_rx > g_udp_sent / 4, "rajada UDP chegou (%u de %u)", g_udp_rx,
              g_udp_sent);
        CHECK(g_udp_dup == 0 && g_udp_reord == 0 && g_udp_bad == 0, "nada duplicado/fora de ordem/cortado (%u/%u/%u)", g_udp_dup,
              g_udp_reord, g_udp_bad);
        CHECK(xhs_resyncs() == rs0, "nenhuma ressincronia falsa na rajada (%u)", xhs_resyncs() - rs0);
        CHECK(g_rw_fires == f0, "detector quieto na rajada (%u)", g_rw_fires - f0);
        CHECK(xhs_check(why, sizeof(why)) == 0, "anéis consistentes depois da rajada: %s", why);
        /* a mesma rajada com a dessincronia do console: acerta e segue */
        unsigned rx0 = g_udp_rx, rs1 = xhs_resyncs();
        g_udp_reord = g_udp_dup = 0;
        g_udp_rx = 0;
        g_udp_rate = 4;
        g_desync_every = 1009;
        for (int i = 0; i < 10000; i++) step_ms();
        g_desync_every = 0;
        g_udp_rate = 0;
        for (int i = 0; i < 300; i++) step_ms();
        printf("  rajada + dessincronia: %u recebidos, %u ressincronias, dup %u, fora de ordem %u\n", g_udp_rx,
               xhs_resyncs() - rs1, g_udp_dup, g_udp_reord);
        CHECK(g_udp_rx > 10000 && xhs_resyncs() - rs1 >= 3 && g_udp_dup == 0 && g_udp_reord == 0,
              "rajada com dessincronia: segue (%u recebidos, %u ressincronias)", g_udp_rx, xhs_resyncs() - rs1);
        (void)rx0;
        /* v0.21.13: a mesma rajada com o poll reentrado de dentro do RX: o poll
         * recusa a reentrada (antes: o colhedor de fora seguia com cons/prod
         * velhos e os quadros saíam fora de ordem) */
        unsigned re0 = xhs_poll_reenters();
        g_udp_reord = g_udp_dup = 0;
        g_udp_rx = 0;
        g_udp_rate = 4;
        g_reenter = 1;
        for (int i = 0; i < 8000; i++) step_ms();
        g_reenter = 0;
        g_udp_rate = 0;
        for (int i = 0; i < 300; i++) step_ms();
        printf("  rajada + poll reentrado: %u recebidos, %u reentradas tentadas, %u recusadas, dup %u, fora de ordem %u\n",
               g_udp_rx, g_reenter_calls, xhs_poll_reenters() - re0, g_udp_dup, g_udp_reord);
        CHECK(g_reenter_calls > 50 && xhs_poll_reenters() - re0 == g_reenter_calls, "reentradas recusadas (%u de %u)",
              xhs_poll_reenters() - re0, g_reenter_calls);
        CHECK(g_udp_rx > 5000 && g_udp_dup == 0 && g_udp_reord == 0, "poll reentrado: nada fora de ordem (%u recebidos, %u fora)",
              g_udp_rx, g_udp_reord);
        CHECK(xhs_check(why, sizeof(why)) == 0, "anéis consistentes depois das reentradas: %s", why);
        /* v0.21.17: a trava do poll esquecida "ocupada" (o console da v0.21.16:
         * todo poll recusado, caminho de dados morto) e a memória em volta
         * dela pisada: o poll se recupera e o RX segue */
        unsigned st0 = xhs_guard_stale(), tr0 = xhs_guard_trampled();
        g_udp_reord = g_udp_dup = 0;
        g_udp_rx = 0;
        g_udp_rate = 4;
        for (int i = 0; i < 8000; i++) {
            if (i % 700 == 350) xhs_test_stick_guard();
            if (i == 4000) xhs_test_trample_guard();
            step_ms();
        }
        g_udp_rate = 0;
        for (int i = 0; i < 300; i++) step_ms();
        printf("  trava esquecida/pisada: %u recebidos, %u travas velhas limpas, %u pisadas, dup %u, fora de ordem %u\n", g_udp_rx,
               xhs_guard_stale() - st0, xhs_guard_trampled() - tr0, g_udp_dup, g_udp_reord);
        CHECK(xhs_guard_stale() - st0 >= 11 && xhs_guard_trampled() - tr0 == 1, "travas velhas limpas (%u) e pisada vista (%u)",
              xhs_guard_stale() - st0, xhs_guard_trampled() - tr0);
        CHECK(g_udp_rx > 5000 && g_udp_dup == 0 && g_udp_reord == 0, "com a trava esquecida o RX segue (%u recebidos)", g_udp_rx);
        /* v0.21.20: hotplug numa porta da frente do EHCI 1 com o Wi-Fi no ar
         * (o controle com fio): full speed e baixa velocidade vão pro OHCI
         * sem reset do controlador nem do anel; high speed é bloqueado; o
         * RX do Wi-Fi não para nem sai de ordem */
        {
            unsigned h0 = xhs_port_handoffs(), b0 = xhs_port_hs_blocked();
            g_udp_reord = g_udp_dup = 0;
            g_udp_rx = 0;
            g_udp_rate = 4;
            int ok_fs = 0, ok_ls = 0, ok_hs = 0, ok_busy = 0;
            for (int i = 0; i < 6000; i++) {
                if (i == 100) front_connect(FD_FS);
                if (i == 400) { ok_fs = g_fp.po; ok_busy = ehci_hcds[1].devices[1].busy; front_disconnect(); }
                if (i == 600) front_connect(FD_LS);
                if (i == 700) { ok_ls = g_fp.po; front_disconnect(); }
                if (i == 900) front_connect(FD_HS);
                if (i == 1300) { ok_hs = !g_fp.enabled && !g_fp.po && g_fp.dev == FD_HS; front_disconnect(); }
                if (i == 1500) front_connect(FD_FS);   /* religa: tem de ir pro OHCI de novo */
                step_ms();
            }
            int ok_re = g_fp.po;
            front_disconnect();
            g_udp_rate = 0;
            for (int i = 0; i < 300; i++) step_ms();
            printf("  hotplug na frente: FS->OHCI %d, LS->OHCI %d, HS bloqueado %d, religado->OHCI %d, tinyehci fora %d; "
                   "RX %u, fora de ordem %u\n", ok_fs, ok_ls, ok_hs, ok_re, ok_busy, g_udp_rx, g_udp_reord);
            CHECK(ok_fs && ok_ls && ok_hs && ok_re && ok_busy, "hotplug tratado (FS %d LS %d HS %d religado %d busy %d)", ok_fs, ok_ls,
                  ok_hs, ok_re, ok_busy);
            CHECK(xhs_port_handoffs() - h0 == 3 && xhs_port_hs_blocked() - b0 == 1, "contadores: OHCI %u, HS %u",
                  xhs_port_handoffs() - h0, xhs_port_hs_blocked() - b0);
            CHECK(g_udp_rx > 5000 && g_udp_reord == 0 && g_udp_dup == 0, "o Wi-Fi seguiu durante o hotplug (%u, fora %u)", g_udp_rx,
                  g_udp_reord);
            CHECK(xhs_check(why, sizeof(why)) == 0, "anéis consistentes depois do hotplug: %s", why);
        }
        udp_remove(g_udp_pcb);
        g_udp_pcb = NULL;
    }

    /* 2) EP2 IN congelado no meio de uma transferência: o detector acha, rearma, termina */
    g_stall_consumer = 0;
    unsigned before = g_rw_fires;
    g_bytes_total = 1024 * 1024;
    app_open(0);
    for (int i = 0; i < 300; i++) step_ms();
    g_freeze_din = 1;
    uint64_t t0 = g_us;
    while (g_app[0].state == 1 || g_app[0].state == 2) { step_ms(); if (g_us - t0 > 60000000) break; }
    CHECK(g_rw_fires == before + 1, "detector disparou uma vez (%u)", g_rw_fires - before);
    CHECK(g_app[0].state == 3 && g_app[0].bad == 0 && g_app[0].got + g_app[0].len == 1024 * 1024,
          "transferência terminou depois do rearme (estado %d, %llu bytes)", g_app[0].state,
          (unsigned long long)(g_app[0].got + g_app[0].len));
    g_app[0].state = 0;
    CHECK(xhs_check(why, sizeof(why)) == 0, "anéis consistentes depois do rearme: %s", why);

    /* 2b) o código antigo (TxPD sempre de 20): algum envio acaba múltiplo de 64,
     * sai um ZLP, o "firmware" para de transmitir e o RX só pinga
     * retransmissões. É o sintoma do console; o detector tem de ver. */
    xw_tx_nopad_test = 1;
    g_req_rem = 438;
    unsigned fires0 = g_rw_fires;
    for (int r = 0; r < 40 && !g_tx_wedged; r++) {
        g_bytes_total = 64 * 1024;
        app_open(0);
        uint64_t t1 = g_us;
        while (g_app[0].state == 1 || g_app[0].state == 2) { step_ms(); if (g_us - t1 > 20000000) break; }
        if (g_app[0].pcb) { tcp_abort(g_app[0].pcb); g_app[0].pcb = NULL; }
        g_app[0].state = 0;
    }
    CHECK(g_tx_wedged, "código antigo: um envio múltiplo de 64 gerou ZLP e travou o TX do firmware (reproduz o console)");
    CHECK(g_rw_fires > fires0, "o detector de RX parado viu o travamento (%u)", g_rw_fires - fires0);
    printf("  código antigo: ZLP no EP2 OUT %u, travou=%d, detector disparou %u vez(es)\n", g_out_zlps, g_tx_wedged,
           g_rw_fires - fires0);
    xw_tx_nopad_test = 0;
    g_req_rem = -1;
    g_tx_wedged = 0;
    for (int i = 0; i < NPEER; i++) g_peer[i].used = 0;

    /* 3) vazamentos: tudo fechado, deixa o TIME_WAIT expirar */
    for (int i = 0; i < 130000; i++) step_ms();
    int pool1 = count_pool(), segs1 = count_segs();
    CHECK(pool1 == pool0, "pool de pbuf: %d livres antes, %d depois", pool0, pool1);
    CHECK(segs1 == segs0, "segmentos TCP: %d livres antes, %d depois", segs0, segs1);
    g_hc_run = 0;
    printf("stress (%s): %d verificações, %d falhas\n", g_threaded ? "thread" : "det", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
