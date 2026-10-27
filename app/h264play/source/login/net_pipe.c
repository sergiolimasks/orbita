/* XCloud360 v0.17 — ver net_pipe.h. */
#include "net_pipe.h"
#include "xc_port.h"
#include "dns_msg.h"
#include "xc_logup.h"
#include "../wifi/xw_xenon.h"
#include "../xc_wdog.h"
#include "../xc_lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppc/atomic.h>
#include <ppc/timebase.h>
#include "../polite_wait.h"
#include <time/time.h>
#include <network/network.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/ip_addr.h>
#include <lwip/err.h>
#include <lwip/pbuf.h>

#define NP_SLOTS          4   /* v0.20.2: worker das capas segura 2 conexões persistentes (gamepass + logs); com 2 slots o displaycatalog dava ENOSLOT */
#define NP_RX_CAP         (256 * 1024)
#define NP_TX_CAP         (32 * 1024)
#define NP_SYN_TIMEOUT_MS 10000
#define NP_SERIALIZE_MS   3000
#define DNS_TRY_MS        1500

/* lwIP 1.4.1 dhcp.c: valores decodificados da última resposta DHCP aceita
 * (globais exportados, válidos até o próximo pacote DHCP pra este MAC). */
#define DHCP_IDX_DNS 8
extern u32_t dhcp_rx_options_val[];
extern u8_t  dhcp_rx_options_given[];

static inline void lwsync(void) { __asm__ __volatile__("lwsync" ::: "memory"); }

static unsigned long ms_since(uint64_t now, uint64_t then) {
    return now > then ? tb_diff_msec(now, then) : 0;
}

/* ------------------------------------------------------------ TCP pipes */

enum { NS_FREE = 0, NS_REQ, NS_CONNECTING, NS_OPEN, NS_EOF, NS_ERR };

typedef struct {
    volatile int    st;
    volatile int    close_req;
    volatile int    err;
    uint32_t        ip;
    int             port;
    struct tcp_pcb *pcb;
    uint64_t        t0;
    xlk_t           rx_lock;   /* v0.21.14: lock instrumentado */
    uint8_t        *rx;
    size_t          rx_rd, rx_wr;
    xlk_t           tx_lock;
    uint8_t        *tx;
    size_t          tx_len;
} np_slot_t;

static np_slot_t g_slot[NP_SLOTS];
static volatile int g_np_inited = 0;

static void ipa_from_u32(ip_addr_t *a, uint32_t ip) {
    IP4_ADDR(a, (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static uint32_t u32_from_ipa(const ip_addr_t *a) {
    return ((uint32_t)ip4_addr1(a) << 24) | ((uint32_t)ip4_addr2(a) << 16) |
           ((uint32_t)ip4_addr3(a) << 8) | ip4_addr4(a);
}

static void slot_detach(np_slot_t *s) {
    if (s->pcb) {
        tcp_arg(s->pcb, NULL);
        tcp_err(s->pcb, NULL);
        tcp_recv(s->pcb, NULL);
        tcp_sent(s->pcb, NULL);
    }
}

static void on_err(void *arg, err_t err) {
    np_slot_t *s = (np_slot_t *)arg;
    if (!s) return;
    s->pcb = NULL;   /* o lwIP já liberou */
    s->err = err;
    lwsync();
    s->st = NS_ERR;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    np_slot_t *s = (np_slot_t *)arg;
    if (!s) { if (p) pbuf_free(p); return ERR_OK; }
    if (p == NULL) {
        slot_detach(s);
        s->pcb = NULL;
        lwsync();
        s->st = NS_EOF;
        if (tcp_close(pcb) != ERR_OK) { tcp_abort(pcb); return ERR_ABRT; }
        return ERR_OK;
    }
    if (err != ERR_OK) { pbuf_free(p); return err; }
    xlk_lock(&s->rx_lock, "net_pipe rx " XLK_HERE);
    if (NP_RX_CAP - s->rx_wr < p->tot_len && s->rx_rd > 0) {
        memmove(s->rx, s->rx + s->rx_rd, s->rx_wr - s->rx_rd);
        s->rx_wr -= s->rx_rd;
        s->rx_rd = 0;
    }
    if (NP_RX_CAP - s->rx_wr < p->tot_len) {
        xlk_unlock(&s->rx_lock);
        return ERR_MEM;   /* consumidor atrasado: o lwIP reentrega depois */
    }
    pbuf_copy_partial(p, s->rx + s->rx_wr, p->tot_len, 0);
    s->rx_wr += p->tot_len;
    xlk_unlock(&s->rx_lock);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    np_slot_t *s = (np_slot_t *)arg;
    (void)pcb;
    if (!s) return ERR_OK;
    if (err != ERR_OK) { s->err = err; lwsync(); s->st = NS_ERR; return err; }
    lwsync();
    s->st = NS_OPEN;
    return ERR_OK;
}

/* Um SYN por vez (bug da v0.8.2). v0.20.0: sem vídeo TCP :9000 nem
 * controle :9002 (modo PC saiu): só os canos daqui. */
static int syn_busy(void) {
    return np_connecting_ms() >= 0;
}

static void slot_connect(np_slot_t *s) {
    ip_addr_t remote;
    ipa_from_u32(&remote, s->ip);
    s->pcb = tcp_new();
    if (!s->pcb) { s->err = ERR_MEM; lwsync(); s->st = NS_ERR; return; }
    tcp_arg(s->pcb, s);
    tcp_err(s->pcb, on_err);
    tcp_recv(s->pcb, on_recv);
    s->t0 = mftb();
    s->st = NS_CONNECTING;
    err_t e = tcp_connect(s->pcb, &remote, (u16_t)s->port, on_connected);
    if (e != ERR_OK) {
        slot_detach(s);
        tcp_abort(s->pcb);
        s->pcb = NULL;
        s->err = e;
        lwsync();
        s->st = NS_ERR;
    }
}

static void slot_flush(np_slot_t *s) {
    if (!s->pcb || s->st != NS_OPEN) return;
    int sent = 0;
    xlk_lock(&s->tx_lock, "net_pipe tx " XLK_HERE);
    size_t n = s->tx_len, room = tcp_sndbuf(s->pcb);
    if (n > room) n = room;
    if (n > 0xFFFF) n = 0xFFFF;
    if (n > 0 && tcp_write(s->pcb, s->tx, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        memmove(s->tx, s->tx + n, s->tx_len - n);
        s->tx_len -= n;
        sent = 1;
    }
    xlk_unlock(&s->tx_lock);
    if (sent) tcp_output(s->pcb);
}

int np_connecting_ms(void) {
    int best = -1;
    uint64_t now = mftb();
    for (int i = 0; i < NP_SLOTS; i++) {
        if (g_slot[i].st != NS_CONNECTING) continue;
        int v = (int)ms_since(now, g_slot[i].t0);
        if (v > best) best = v;
    }
    return best;
}

/* ------------------------------------------------------------------ DNS */

static struct udp_pcb *g_dns_pcb = NULL;
static uint32_t        g_dns_srv[4];
static int             g_dns_nsrv = 0;
enum { DQ_IDLE = 0, DQ_REQ, DQ_BUSY, DQ_DONE, DQ_FAIL };
static volatile int    g_dq_st = DQ_IDLE;
static char            g_dq_host[256];
static volatile uint32_t g_dq_ip;
static uint16_t        g_dq_id;
static int             g_dq_try;
static uint64_t        g_dq_t0;

static void dns_add_srv(uint32_t ip) {
    if (!ip || ip == 0xFFFFFFFFu || g_dns_nsrv >= 4) return;
    for (int i = 0; i < g_dns_nsrv; i++) if (g_dns_srv[i] == ip) return;
    g_dns_srv[g_dns_nsrv++] = ip;
}

static void dns_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;
    uint8_t buf[1500];
    size_t n = p->tot_len < sizeof(buf) ? p->tot_len : sizeof(buf);
    pbuf_copy_partial(p, buf, (u16_t)n, 0);
    pbuf_free(p);
    if (g_dq_st != DQ_BUSY) return;
    uint32_t ip = 0;
    int rcode = 0;
    int r = dns_parse_response(buf, n, g_dq_id, &ip, &rcode);
    if (r == 0) { g_dq_ip = ip; lwsync(); g_dq_st = DQ_DONE; }
    else if (r == 1 && rcode == 3) { lwsync(); g_dq_st = DQ_FAIL; }   /* NXDOMAIN */
    /* r == 1 sem A / -1 lixo: espera o timeout e tenta o próximo servidor */
}

static void dns_send(void) {
    uint8_t q[300];
    g_dq_id = (uint16_t)(mftb() ^ (mftb() >> 17));
    int ql = dns_build_query(q, sizeof(q), g_dq_id, g_dq_host);
    if (ql < 0 || !g_dns_pcb || g_dns_nsrv == 0) { g_dq_st = DQ_FAIL; return; }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)ql, PBUF_RAM);
    if (!p) return;   /* tenta no próximo passo (timeout) */
    memcpy(p->payload, q, (size_t)ql);
    ip_addr_t srv;
    ipa_from_u32(&srv, g_dns_srv[g_dq_try % g_dns_nsrv]);
    udp_sendto(g_dns_pcb, p, &srv, 53);
    pbuf_free(p);
}

static void dns_step(void) {
    if (g_dq_st == DQ_REQ) {
        g_dq_try = 0;
        g_dq_t0 = mftb();
        g_dq_st = DQ_BUSY;
        dns_send();
    } else if (g_dq_st == DQ_BUSY && ms_since(mftb(), g_dq_t0) > DNS_TRY_MS) {
        if (++g_dq_try >= 2 * (g_dns_nsrv > 0 ? g_dns_nsrv : 1)) { lwsync(); g_dq_st = DQ_FAIL; return; }
        g_dq_t0 = mftb();
        dns_send();
    }
}

/* ------------------------------------------------------------------ log */

/* v0.20.1: o LOG "LOG <texto>" por UDP :9003 pro PC saiu. As linhas vão pro
 * buffer do upload dos testadores beta (xc_logup.c), que só envia com
 * XC360/BETAKEY.TXT no pendrive. */
void np_log_line(const char *text) {
    /* v0.21.5: as linhas do Wi-Fi (e do cabo) também vão pro XC360/WIFILOG.TXT
     * (lido pelo FTP quando não houve internet nenhuma) */
    if (text && (!strncmp(text, "[wifi]", 6) || !strncmp(text, "[net] cabo", 10) || !strncmp(text, "[wdog]", 6))) {
        xw_logfile_line(text);
    } else if (text && xw_sys_session()) {
        /* v0.21.10: na sessão vai tudo (sessão, WebRTC, TLS, falhas), no
         * máximo 12 linhas por segundo das outras fontes (o libpeer fala muito) */
        static uint64_t win = 0;
        static int cnt = 0;
        uint64_t t = xcn_now_ms();
        if (t - win >= 1000) { win = t; cnt = 0; }
        if (++cnt <= 12 || !strncmp(text, "[sess]", 6) || !strncmp(text, "[xn]", 4) || !strncmp(text, "[menu]", 6))
            xw_logfile_line(text);
    }
    xlu_line(text);
}

/* --------------------------------------------------------- init / step */

/* v0.21.3: Cloudflare primeiro (pedido dos testadores: DNS de provedor
 * lento/bloqueando), depois Google, e só então o DNS do DHCP e o gateway.
 * Devolve quantos vieram do DHCP. Só a thread de rede. */
static int dns_fill(uint32_t gw) {
    g_dns_nsrv = 0;
    dns_add_srv(0x01010101u);   /* 1.1.1.1 */
    dns_add_srv(0x01000001u);   /* 1.0.0.1 */
    dns_add_srv(0x08080808u);   /* 8.8.8.8 */
    int before = g_dns_nsrv;
    for (int i = 0; i < 2; i++)
        if (dhcp_rx_options_given[DHCP_IDX_DNS + i]) dns_add_srv(dhcp_rx_options_val[DHCP_IDX_DNS + i]);
    int from_dhcp = g_dns_nsrv - before;
    dns_add_srv(gw);
    return from_dhcp;
}

int np_init(void) {
    if (g_np_inited) return 0;
    for (int i = 0; i < NP_SLOTS; i++) {
        memset(&g_slot[i], 0, sizeof(g_slot[i]));
        g_slot[i].rx = (uint8_t *)malloc(NP_RX_CAP);
        g_slot[i].tx = (uint8_t *)malloc(NP_TX_CAP);
        if (!g_slot[i].rx || !g_slot[i].tx) return -1;
    }
    g_dns_pcb = udp_new();
    if (!g_dns_pcb) return -1;
    udp_recv(g_dns_pcb, dns_recv_cb, NULL);

    int from_dhcp = dns_fill(u32_from_ipa(&netif.gw));
    char s[160];
    int l = snprintf(s, sizeof(s), "[net] DNS (%d do DHCP):", from_dhcp);
    for (int i = 0; i < g_dns_nsrv; i++)
        l += snprintf(s + l, sizeof(s) - (size_t)l, " %u.%u.%u.%u", g_dns_srv[i] >> 24,
                      g_dns_srv[i] >> 16 & 255, g_dns_srv[i] >> 8 & 255, g_dns_srv[i] & 255);
    lwsync();
    g_np_inited = 1;
    printf("%s\n", s);
    np_log_line(s);
    return 0;
}

/* v0.21.0: o Wi-Fi pegou DHCP depois do boot (sem cabo): refaz a lista de
 * DNS (públicos, opção 6 dessa resposta, gateway do Wi-Fi). Só a
 * thread de rede. */
void np_dns_reload(uint32_t gw) {
    int from_dhcp = dns_fill(gw);
    char s[160];
    int l = snprintf(s, sizeof(s), "[net] DNS do Wi-Fi (%d do DHCP):", from_dhcp);
    for (int i = 0; i < g_dns_nsrv; i++)
        l += snprintf(s + l, sizeof(s) - (size_t)l, " %u.%u.%u.%u", g_dns_srv[i] >> 24,
                      g_dns_srv[i] >> 16 & 255, g_dns_srv[i] >> 8 & 255, g_dns_srv[i] & 255);
    np_log_line(s);
}

void np_step(void) {
    if (!g_np_inited) return;
    uint64_t now = mftb();
    for (int i = 0; i < NP_SLOTS; i++) {
        np_slot_t *s = &g_slot[i];
        if (s->close_req) {
            if (s->pcb) {
                slot_detach(s);
                tcp_abort(s->pcb);
                s->pcb = NULL;
            }
            xlk_lock(&s->tx_lock, "net_pipe tx " XLK_HERE);
            s->tx_len = 0;
            xlk_unlock(&s->tx_lock);
            lwsync();
            s->st = NS_FREE;
            lwsync();
            s->close_req = 0;
            continue;
        }
        switch (s->st) {
        case NS_REQ:
            if (!syn_busy()) slot_connect(s);
            break;
        case NS_CONNECTING:
            if (ms_since(now, s->t0) > NP_SYN_TIMEOUT_MS) {
                slot_detach(s);
                if (s->pcb) tcp_abort(s->pcb);
                s->pcb = NULL;
                s->err = ERR_TIMEOUT;
                lwsync();
                s->st = NS_ERR;
            }
            break;
        case NS_OPEN:
            slot_flush(s);
            break;
        default:
            break;
        }
    }
    dns_step();
}

/* ============ lado consumidor (thread do login): xc_port.h ============ */

uint64_t xcn_now_ms(void) { return mftb() / (PPC_TIMEBASE_FREQ / 1000ULL); }
uint64_t xcn_now_us(void) { return mftb() / (PPC_TIMEBASE_FREQ / 1000000ULL); }
void     xcn_sleep_ms(int ms) { xwd_observe(); xc_polite_udelay(ms * 1000); }

/* Cache de DNS pequeno: o fluxo fala com os mesmos hosts várias vezes. */
#define DNS_CACHE 8
static struct { char host[96]; uint32_t ip; uint64_t at; } g_dcache[DNS_CACHE];

int xcn_resolve(const char *host, uint32_t *ip, int timeout_ms) {
    xwd_beat2("rede: DNS", host);
    ip_addr_t lit;
    if (ipaddr_aton(host, &lit)) { *ip = u32_from_ipa(&lit); return 0; }
    if (!g_np_inited || strlen(host) >= sizeof(g_dq_host)) return XCN_EDNS;
    uint64_t now = xcn_now_ms();
    for (int i = 0; i < DNS_CACHE; i++)
        if (g_dcache[i].ip && !strcmp(g_dcache[i].host, host) && now - g_dcache[i].at < 300000) {
            *ip = g_dcache[i].ip;
            return 0;
        }
    /* Mailbox livre = IDLE/DONE/FAIL. Uma consulta nossa que deu timeout
     * pode ainda estar BUSY na rede (ela desiste em ≤ 2 tentativas/servidor). */
    for (uint64_t w = xcn_now_ms() + 15000; g_dq_st == DQ_REQ || g_dq_st == DQ_BUSY;) {
        if (xcn_cancelled()) return XCN_ECANCEL;
        if (xcn_now_ms() > w) return XCN_EDNS;
        xwd_observe();
        xc_polite_udelay(1000);
    }
    snprintf(g_dq_host, sizeof(g_dq_host), "%s", host);
    lwsync();
    g_dq_st = DQ_REQ;
    uint64_t end = xcn_now_ms() + (uint64_t)timeout_ms;
    int rc = XCN_EDNS;
    for (;;) {
        int st = g_dq_st;
        if (st == DQ_DONE) { lwsync(); *ip = g_dq_ip; rc = 0; break; }
        if (st == DQ_FAIL) break;
        if (xcn_cancelled()) { rc = XCN_ECANCEL; break; }
        if (xcn_now_ms() > end) { rc = XCN_ETIMEOUT; break; }
        xwd_observe();
        xc_polite_udelay(500);
    }
    /* Devolve a mailbox (DONE/FAIL já são "livre"; BUSY após timeout nosso
     * a rede termina sozinha e a próxima consulta espera). */
    if (rc == 0) {
        int k = 0;
        for (int i = 1; i < DNS_CACHE; i++) if (g_dcache[i].at < g_dcache[k].at) k = i;
        snprintf(g_dcache[k].host, sizeof(g_dcache[k].host), "%s", host);
        g_dcache[k].ip = *ip;
        g_dcache[k].at = xcn_now_ms();
    } else {
        xc_log("[dns] %s falhou (%d)", host, rc);
    }
    return rc == XCN_ETIMEOUT ? XCN_EDNS : rc;
}

int xcn_connect(uint32_t ip, int port, int timeout_ms, int *h) {
    xwd_beat("rede: conectando (TCP)");
    if (!g_np_inited) return XCN_ECONN;
    np_slot_t *s = NULL;
    int idx = -1;
    for (int i = 0; i < NP_SLOTS; i++)
        if (g_slot[i].st == NS_FREE && !g_slot[i].close_req) { s = &g_slot[i]; idx = i; break; }
    if (!s) return XCN_ENOSLOT;
    xlk_lock(&s->rx_lock, "net_pipe rx " XLK_HERE);
    s->rx_rd = s->rx_wr = 0;
    xlk_unlock(&s->rx_lock);
    xlk_lock(&s->tx_lock, "net_pipe tx " XLK_HERE);
    s->tx_len = 0;
    xlk_unlock(&s->tx_lock);
    s->ip = ip;
    s->port = port;
    s->err = 0;
    lwsync();
    s->st = NS_REQ;
    uint64_t end = xcn_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        int st = s->st;
        if (st == NS_OPEN) { *h = idx; return 0; }
        if (st == NS_ERR || st == NS_EOF) {
            int err = s->err;
            s->close_req = 1;
            xc_log("[net] connect %u.%u.%u.%u:%d falhou (lwIP err %d)", ip >> 24, ip >> 16 & 255,
                   ip >> 8 & 255, ip & 255, port, err);
            return XCN_ECONN;
        }
        if (xcn_cancelled()) { s->close_req = 1; return XCN_ECANCEL; }
        if (xcn_now_ms() > end) { s->close_req = 1; return XCN_ETIMEOUT; }
        xwd_observe();
        xc_polite_udelay(500);
    }
}

int xcn_send(int h, const uint8_t *b, size_t n, int timeout_ms) {
    xwd_beat("rede: enviando");
    if (h < 0 || h >= NP_SLOTS) return XCN_EIO;
    np_slot_t *s = &g_slot[h];
    size_t off = 0;
    uint64_t end = xcn_now_ms() + (uint64_t)timeout_ms;
    while (off < n) {
        if (s->st != NS_OPEN) return XCN_ECLOSED;
        xlk_lock(&s->tx_lock, "net_pipe tx " XLK_HERE);
        size_t room = NP_TX_CAP - s->tx_len, k = n - off < room ? n - off : room;
        if (k) { memcpy(s->tx + s->tx_len, b + off, k); s->tx_len += k; }
        xlk_unlock(&s->tx_lock);
        off += k;
        if (off >= n) break;
        if (xcn_cancelled()) return XCN_ECANCEL;
        if (xcn_now_ms() > end) return XCN_ETIMEOUT;
        xwd_observe();
        xc_polite_udelay(200);
    }
    return (int)n;
}

int xcn_recv(int h, uint8_t *b, size_t n, int timeout_ms) {
    xwd_beat("rede: recebendo");
    if (h < 0 || h >= NP_SLOTS) return XCN_EIO;
    np_slot_t *s = &g_slot[h];
    uint64_t end = xcn_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        int st = s->st;   /* lê o estado ANTES dos dados (EOF só vale com ring vazio) */
        lwsync();
        xlk_lock(&s->rx_lock, "net_pipe rx " XLK_HERE);
        size_t avail = s->rx_wr - s->rx_rd;
        if (avail) {
            size_t k = avail < n ? avail : n;
            memcpy(b, s->rx + s->rx_rd, k);
            s->rx_rd += k;
            if (s->rx_rd == s->rx_wr) s->rx_rd = s->rx_wr = 0;
            xlk_unlock(&s->rx_lock);
            return (int)k;
        }
        xlk_unlock(&s->rx_lock);
        if (st == NS_EOF) return 0;
        if (st != NS_OPEN) return XCN_EIO;
        if (xcn_cancelled()) return XCN_ECANCEL;
        if (xcn_now_ms() > end) return XCN_ETIMEOUT;
        xwd_observe();
        xc_polite_udelay(200);
    }
}

/* ---- v0.19.2: não-bloqueantes (a thread de rede chama; o np_step dela
 * mesma faz o SYN, o flush do TX e o fechamento no próximo passo) ---- */

int xcn_connect_start(uint32_t ip, int port, int *h) {
    if (!g_np_inited) return XCN_ECONN;
    for (int i = 0; i < NP_SLOTS; i++) {
        np_slot_t *s = &g_slot[i];
        if (s->st != NS_FREE || s->close_req) continue;
        xlk_lock(&s->rx_lock, "net_pipe rx " XLK_HERE);
        s->rx_rd = s->rx_wr = 0;
        xlk_unlock(&s->rx_lock);
        xlk_lock(&s->tx_lock, "net_pipe tx " XLK_HERE);
        s->tx_len = 0;
        xlk_unlock(&s->tx_lock);
        s->ip = ip;
        s->port = port;
        s->err = 0;
        lwsync();
        s->st = NS_REQ;
        *h = i;
        return 0;
    }
    return XCN_ENOSLOT;
}

int xcn_connect_poll(int h) {
    if (h < 0 || h >= NP_SLOTS) return XCN_EIO;
    np_slot_t *s = &g_slot[h];
    int st = s->st;
    if (st == NS_OPEN) return 0;
    if (st == NS_REQ || st == NS_CONNECTING) return XCN_EAGAIN;
    s->close_req = 1;
    return XCN_ECONN;
}

int xcn_try_send(int h, const uint8_t *b, size_t n) {
    if (h < 0 || h >= NP_SLOTS) return XCN_EIO;
    np_slot_t *s = &g_slot[h];
    if (s->st != NS_OPEN) return XCN_ECLOSED;
    xlk_lock(&s->tx_lock, "net_pipe tx " XLK_HERE);
    size_t room = NP_TX_CAP - s->tx_len, k = n < room ? n : room;
    if (k) { memcpy(s->tx + s->tx_len, b, k); s->tx_len += k; }
    xlk_unlock(&s->tx_lock);
    return (int)k;
}

int xcn_try_recv(int h, uint8_t *b, size_t n) {
    if (h < 0 || h >= NP_SLOTS) return XCN_EIO;
    np_slot_t *s = &g_slot[h];
    int st = s->st;   /* estado ANTES dos dados (EOF só vale com ring vazio) */
    lwsync();
    xlk_lock(&s->rx_lock, "net_pipe rx " XLK_HERE);
    size_t avail = s->rx_wr - s->rx_rd;
    if (avail) {
        size_t k = avail < n ? avail : n;
        memcpy(b, s->rx + s->rx_rd, k);
        s->rx_rd += k;
        if (s->rx_rd == s->rx_wr) s->rx_rd = s->rx_wr = 0;
        xlk_unlock(&s->rx_lock);
        return (int)k;
    }
    xlk_unlock(&s->rx_lock);
    if (st == NS_EOF) return 0;
    if (st != NS_OPEN) return XCN_EIO;
    return XCN_EAGAIN;
}

void xcn_close_nb(int h) {
    if (h < 0 || h >= NP_SLOTS) return;
    g_slot[h].close_req = 1;
}

void xcn_close(int h) {
    if (h < 0 || h >= NP_SLOTS) return;
    g_slot[h].close_req = 1;
    /* espera a rede liberar (≤ alguns ms) pra o próximo connect achar slot */
    uint64_t end = xcn_now_ms() + 200;
    while (g_slot[h].close_req && xcn_now_ms() < end) xc_polite_udelay(200);
}
