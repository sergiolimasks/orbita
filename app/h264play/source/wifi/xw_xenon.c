/*
 * orbita v0.21.0 — Wi-Fi interno do Xbox 360 S/E: USB (OHCI do libxenon),
 * netif "wl" do lwIP e a ponte com o menu. Ver xw_xenon.h e WIFI.md.
 */
#include "xw_xenon.h"
#include "xw_wpa.h"
#include "xw_ehs_xenon.h"
#include "xw_rxwatch.h"
#include "../xc_lock.h"
#include "../login/xc_logup.h"
#include "../tcp_stream.h"
#include "../xc_wdog.h"
#include "../login/xc_port.h"
#include "../login/net_pipe.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppc/atomic.h>
#include <ppc/timebase.h>
#include <nocfe/lib_queue.h>
typedef unsigned int physaddr_t;   /* nocfe/cfe.h (não incluído: puxa o resto do CFE) */
#include <usb/usbchap9.h>
#include <usb/usbd.h>
#include <lwip/netif.h>
#include <lwip/dhcp.h>
#include <lwip/pbuf.h>
#include <lwip/ip_addr.h>
#include <netif/etharp.h>
#include <network/network.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

extern usbbus_t *usb_buses[];
extern int usb_buscnt;
int xw_ehci_release_wifi(int *bus, int *port, int release);
int xw_ehci_enum_unknown(void);   /* v0.21.41 */
void xw_ehci_ports_text(char *o, size_t n);   /* v0.21.43 */
uint32_t xw_ehci_port_status(int bus, int port);
int ohci_debug_hc(usbbus_t *bus, char *out, int cap);   /* libxenon (patch GIP): estado do HC e das portas raiz */

#define FIND_EVERY_MS    100
#define PRESENT_WAIT_MS  8000
#define FIND_GIVEUP_MS   30000
#define BOOT_WAIT_MS     25000
#define PUB_EVERY_MS     100
#define STAT_EVERY_MS    60000

/* ------------------------------------------------------------ estado */

static xlk_t        g_lk = XLK_INIT("wifi status");   /* mailbox main <-> hw1 (v0.21.14: instrumentado) */
static volatile int g_inited = 0;
static int          g_cable = 0;
static uint64_t     g_t0_tb = 0;

static xw_status_t  g_pub;             /* sob g_lk */
static xw_saved_t   g_saved;           /* sob g_lk */
static int          g_have_saved = 0;  /* sob g_lk */
/* pedidos (sob g_lk) */
static int          r_scan, r_connect, r_connect_saved, r_forget;
static xw_saved_t   r_target;
/* salvar quando conectar (hw1 marca, main grava) */
static xw_saved_t   g_save;            /* sob g_lk */
static int          g_save_ready = 0;  /* sob g_lk */

/* só hw1 */
/* v0.21.18: o g_core (11 KB) entre canários de 128 B (uma linha de cache
 * inteira de cada lado). Na v0.21.16 o g_crumb_t, logo DEPOIS do g_core, e a
 * trava do poll, logo ANTES, apareceram pisados no mesmo instante. Os
 * canários são conferidos a cada passo; o primeiro estrago vai pro log com o
 * deslocamento e os bytes achados. */
#define CBOX_WORDS 32
typedef struct {
    uint32_t  can_a[CBOX_WORDS];
    xw_core_t core;
    uint32_t  can_b[CBOX_WORDS];
} __attribute__((aligned(128))) corebox_t;
static corebox_t g_corebox __attribute__((aligned(128)));
#define g_core g_corebox.core
static xw_ops_t     g_ops;
static xw_saved_t   g_want_save;
static int          g_want_save_on = 0;
static int          g_present = XW_PRESENT_UNKNOWN;
static int          g_released = 0;
/* só liga o adaptador com rede salva e sem cabo, ou quando a tela do Wi-Fi
 * pede algo (o Ethernet de quem não usa Wi-Fi fica exatamente como antes) */
static volatile int g_activate = 0;
/* v0.21.29: cabo com internet = Wi-Fi desligado de vez (desassocia, tira a
 * interface do lwIP e o passo vira um return: nada de CPU/memória gasto) */
static volatile int g_off_req = 0;
static int g_off = 0;
void xhs_watch_release(void);   /* v0.21.43 */
static volatile int g_wake_req = 0;   /* v0.21.43: o menu pediu Wi-Fi (desfaz cabo/falha) */
static int g_watch_claimed = 0;   /* v0.21.29: vigia do hotplug sem o Wi-Fi aberto */
static uint32_t g_off_t = 0;
static uint32_t     g_last_arp = 0;
/* v0.21.1: transporte (1 = OHCI full speed, 2 = EHCI high speed nosso), etapas
 * com log e prazo: a tela nunca fica em "Ligando" pra sempre */
enum { TR_NONE = 0, TR_OHCI, TR_EHCI };
static int          g_tr = TR_NONE;
static int          g_hs_tried = 0;
static int g_enum_tried = 0;   /* v0.21.41: já identificou as portas sem endereço */
static uint32_t     g_t_act = 0, g_t_rel = 0, g_last_diag = 0;
static int          g_rel_bus = -1, g_rel_port = -1;
static char         g_why[320];         /* sob g_lk na leitura (publish) */
static char         g_hs_why[240];   /* v0.21.43: + portas */
static const char  *g_stage = "";
static int          g_hex_in = 0, g_hex_out = 0;
static unsigned     g_tx_seen = 0;          /* v0.21.4: conclusões do EP2 OUT já avisadas ao core */
static uint32_t     g_last_hslog = 0;
/* v0.21.8: detector de RX parado (link de pé, TX saindo, nada chegando há 3 s) */
static xrw_t        g_rw;
static int          g_rw_inited = 0;
static unsigned     g_rw_rearms = 0;        /* recuperações (rearme do anel) */
static unsigned     g_rw_reassoc = 0;       /* escalada: reassociação */
static unsigned     g_rx_pbuf_fail = 0;     /* pbuf_alloc falhou no RX do Wi-Fi */
static unsigned     g_fw_resets = 0, g_rec_ok = 0;   /* v0.21.9 */
static volatile int g_session = 0;                  /* v0.21.10: sessão/jogo (o main diz) */
static uint32_t     g_fwq_t = 0, g_fwq_log_at = 0;
/* RX: buraco entre quadros de dados (com o link de pé) */
static uint32_t     g_rx_last_t = 0, g_gap_max = 0;
static unsigned     g_gaps_slow = 0;
static char         g_last_hs[220];
#define ACT_TIMEOUT_MS  30000           /* ligado e sem "pronto" depois disto = falha visível */
#define FS_WAIT_MS      8000            /* full speed: o OHCI tem de ver o adaptador */
#define HS_FIND_MS      3000
static int          g_attached = 0, g_dead = 0;
static uint32_t     g_last_find = 0, g_last_pub = 0, g_last_stat = 0;
static mbedtls_entropy_context  g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static int          g_drbg_ok = 0;

/* lwIP */
static struct netif g_wl;
static int          g_wl_added = 0, g_wl_up = 0, g_dhcp_ok = 0, g_is_default = 0;

/* --------------------------------------------------------------- USB */

#define DMA_ALIGN 128
#define RX_DATA_N 3
#define TX_DATA_N 8
#define TX_DATA_INFLIGHT 4
#define TX_CMD_N  3

typedef struct {
    usbreq_t *ur;
    uint8_t  *buf;
    int       cap;
    volatile int done;
    volatile int status, xferred;
} rx_slot_t;

enum { TS_FREE = 0, TS_PENDING, TS_INFLIGHT, TS_DONE };
typedef struct {
    usbreq_t *ur, *zur;
    uint8_t  *buf;
    int       len;
    volatile int state, left, err;
} tx_slot_t;

typedef struct {
    tx_slot_t s[TX_DATA_N];
    int n, cap, wr, sub, fin, inflight, max_inflight;
    int ep, mps;
    unsigned drops, errs, reaped;
} tx_ring_t;

static usbdev_t  *g_dev;
static int        g_ep_cin = -1, g_ep_cout = -1, g_ep_din = -1, g_ep_dout = -1;
static rx_slot_t  g_rx_cmd;
static rx_slot_t  g_rx_data[RX_DATA_N];
static tx_ring_t  g_tx_cmd, g_tx_data;
static unsigned   g_rx_errs = 0, g_rx_err_run = 0;
/* v0.21.18: o buffer de cópia do RX também entre canários */
static struct {
    uint32_t can_a[CBOX_WORDS];
    uint8_t  buf[MWX_RX_DATA_BUF];
    uint32_t can_b[CBOX_WORDS];
} __attribute__((aligned(128))) g_rxbox;
#define g_rx_copy g_rxbox.buf

static uint32_t now_ms(void) { return (uint32_t)(mftb() / (PPC_TIMEBASE_FREQ / 1000)); }

static int rx_cb(usbreq_t *ur) {
    rx_slot_t *s = (rx_slot_t *)ur->ur_ref;
    s->status = ur->ur_status;
    s->xferred = ur->ur_xferred;
    __asm__ __volatile__("lwsync" ::: "memory");
    s->done = 1;
    return 0;
}

static int tx_cb(usbreq_t *ur) {
    tx_slot_t *s = (tx_slot_t *)ur->ur_ref;
    if (ur->ur_status && ur->ur_status != UR_ERR_DATAUNDERRUN) s->err = ur->ur_status;
    if (--s->left <= 0) s->state = TS_DONE;
    return 0;
}

static int rx_queue(rx_slot_t *s, int ep) {
    s->done = 0;
    s->ur->ur_buffer = s->buf;
    s->ur->ur_length = s->cap;
    s->ur->ur_xferred = 0;
    s->ur->ur_status = 0;
    s->ur->ur_flags = UR_FLAG_IN | UR_FLAG_SHORTOK;
    s->ur->ur_callback = rx_cb;
    s->ur->ur_ref = s;
    (void)ep;
    return usb_queue_request(s->ur);
}

static int ring_init(tx_ring_t *r, int n, int cap, int ep, int mps, int max_inflight) {
    memset(r, 0, sizeof(*r));
    r->n = n;
    r->cap = cap;
    r->ep = ep;
    r->mps = mps > 0 ? mps : 64;
    r->max_inflight = max_inflight;
    for (int i = 0; i < n; i++) {
        tx_slot_t *s = &r->s[i];
        s->buf = (uint8_t *)memalign(DMA_ALIGN, (size_t)cap);
        if (!s->buf) return -1;
        s->ur = usb_make_request(g_dev, ep, s->buf, 0, UR_FLAG_OUT);
        s->zur = usb_make_request(g_dev, ep, s->buf, 0, UR_FLAG_OUT);
        if (!s->ur || !s->zur) return -1;
        s->ur->ur_callback = s->zur->ur_callback = tx_cb;
        s->ur->ur_ref = s->zur->ur_ref = s;
    }
    return 0;
}

/* qualquer contexto da hw1 (inclusive o linkoutput do lwIP no net_step,
 * que pode cair dentro de um xc_usb_hold): só copia */
static int ring_put(tx_ring_t *r, const uint8_t *b, size_t n) {
    tx_slot_t *s = &r->s[r->wr];
    if (s->state != TS_FREE || (int)n > r->cap) { r->drops++; return -1; }
    memcpy(s->buf, b, n);
    s->len = (int)n;
    s->err = 0;
    s->state = TS_PENDING;
    r->wr = (r->wr + 1) % r->n;
    return 0;
}

/* só no xw_sys_step (USB seguro) */
static void ring_flush(tx_ring_t *r) {
    while (r->s[r->fin].state == TS_DONE) {
        tx_slot_t *s = &r->s[r->fin];
        if (s->err) r->errs++;
        else r->reaped++;
        s->state = TS_FREE;
        r->inflight--;
        r->fin = (r->fin + 1) % r->n;
    }
    while (r->s[r->sub].state == TS_PENDING && r->inflight < r->max_inflight) {
        tx_slot_t *s = &r->s[r->sub];
        int zlp = (s->len % r->mps) == 0;
        s->left = zlp ? 2 : 1;
        s->ur->ur_buffer = s->buf;
        s->ur->ur_length = s->len;
        s->ur->ur_xferred = 0;
        s->ur->ur_flags = UR_FLAG_OUT;
        if (usb_queue_request(s->ur) != 0) return;   /* sem TD agora: tenta no próximo passo */
        if (zlp) {
            s->zur->ur_buffer = s->buf;
            s->zur->ur_length = 0;
            s->zur->ur_xferred = 0;
            s->zur->ur_flags = UR_FLAG_OUT;
            if (usb_queue_request(s->zur) != 0) s->left = 1;
        }
        s->state = TS_INFLIGHT;
        r->inflight++;
        r->sub = (r->sub + 1) % r->n;
    }
}

static int dev_detach(usbdev_t *d) {
    (void)d;
    g_dead = 1;
    return 0;
}
static int dev_attach_stub(usbdev_t *d, usb_driver_t *drv) { (void)d; (void)drv; return 0; }
static usb_driver_t g_drv = { "orbita Wi-Fi (88W8786U)", dev_attach_stub, dev_detach };

static usbdev_t *find_dev(void) {
    for (int i = 0; i < usb_buscnt; i++) {
        usbbus_t *bus = usb_buses[i];
        if (!bus) continue;
        for (int a = 1; a < USB_MAX_DEVICES; a++) {
            usbdev_t *d = bus->ub_devices[a];
            if (!d || (d->ud_flags & UD_FLAG_HUB)) continue;
            if (mwx_is_id(GETUSBFIELD(&d->ud_devdescr, idVendor), GETUSBFIELD(&d->ud_devdescr, idProduct)))
                return d;
        }
    }
    return NULL;
}

static int attach(usbdev_t *d) {
    usb_config_descr_t *cfg = d->ud_cfgdescr;
    if (!cfg) { xc_log("[wifi] 045e:0765 sem descritor de configuração"); return -1; }
    if (d->ud_drv && d->ud_drv != &g_drv) {
        xc_log("[wifi] 045e:0765 já tem driver (%s), não mexo", d->ud_drv->udrv_name);
        return -1;
    }
    usb_endpoint_descr_t *cin = NULL, *cout = NULL, *din = NULL, *dout = NULL;
    for (int i = 0; i < 16; i++) {
        usb_endpoint_descr_t *e = usb_find_cfg_descr(d, USB_ENDPOINT_DESCRIPTOR_TYPE, i);
        if (!e) break;
        int num = e->bEndpointAddress & 0x0f, in = (e->bEndpointAddress & 0x80) != 0;
        int type = e->bmAttributes & USB_ENDPOINT_TYPE_MASK;
        xc_log("[wifi] endpoint 0x%02x tipo %d mps %d intervalo %d", e->bEndpointAddress, type,
               GETUSBFIELD(e, wMaxPacketSize), e->bInterval);
        if (num == MWX_EP_CMD && in) cin = e;
        if (num == MWX_EP_CMD && !in) cout = e;
        if (num == MWX_EP_DATA && in && type == USB_ENDPOINT_TYPE_BULK) din = e;
        if (num == MWX_EP_DATA && !in && type == USB_ENDPOINT_TYPE_BULK) dout = e;
    }
    if (!cin || !cout || !din || !dout) {
        xc_log("[wifi] faltam endpoints (cmd %d/%d, dados %d/%d)", !!cin, !!cout, !!din, !!dout);
        return -1;
    }
    int r = usb_set_configuration(d, cfg->bConfigurationValue);
    xc_log("[wifi] SET_CONFIGURATION %d -> %d", cfg->bConfigurationValue, r);
    g_dev = d;
#define OPEN(e) (((e)->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_INTERRUPT \
                 ? usb_open_pipe_interval(d, (e), (e)->bInterval ? (e)->bInterval : 1) : usb_open_pipe(d, (e)))
    g_ep_cout = OPEN(cout);
    g_ep_cin = OPEN(cin);
    g_ep_dout = OPEN(dout);
    g_ep_din = OPEN(din);
#undef OPEN
    if (g_ep_cin < 0 || g_ep_cout < 0 || g_ep_din < 0 || g_ep_dout < 0) {
        xc_log("[wifi] usb_open_pipe falhou");
        return -1;
    }
    if (ring_init(&g_tx_cmd, TX_CMD_N, MWX_CMD_MAX, g_ep_cout, GETUSBFIELD(cout, wMaxPacketSize), 1) != 0 ||
        ring_init(&g_tx_data, TX_DATA_N, 2048, g_ep_dout, GETUSBFIELD(dout, wMaxPacketSize), TX_DATA_INFLIGHT) != 0) {
        xc_log("[wifi] sem memória pros buffers de envio");
        return -1;
    }
    g_rx_cmd.cap = MWX_RX_CMD_BUF;
    g_rx_cmd.buf = (uint8_t *)memalign(DMA_ALIGN, MWX_RX_CMD_BUF);
    g_rx_cmd.ur = g_rx_cmd.buf ? usb_make_request(d, g_ep_cin, g_rx_cmd.buf, MWX_RX_CMD_BUF, UR_FLAG_IN | UR_FLAG_SHORTOK) : NULL;
    if (!g_rx_cmd.ur) return -1;
    for (int i = 0; i < RX_DATA_N; i++) {
        rx_slot_t *s = &g_rx_data[i];
        s->cap = MWX_RX_DATA_BUF;
        s->buf = (uint8_t *)memalign(DMA_ALIGN, MWX_RX_DATA_BUF);
        s->ur = s->buf ? usb_make_request(d, g_ep_din, s->buf, MWX_RX_DATA_BUF, UR_FLAG_IN | UR_FLAG_SHORTOK) : NULL;
        if (!s->ur) return -1;
    }
    d->ud_drv = &g_drv;   /* desplugar chama o nosso detach */
    if (rx_queue(&g_rx_cmd, g_ep_cin) != 0) xc_log("[wifi] fila do EP1 IN recusada");
    for (int i = 0; i < RX_DATA_N; i++)
        if (rx_queue(&g_rx_data[i], g_ep_din) != 0) xc_log("[wifi] fila do EP2 IN %d recusada", i);
    return 0;
}

/* ------------------------------------------------------- ops do core */

static void hexlog(const char *dir, const uint8_t *b, size_t n) {
    char h[3 * 20 + 1];
    size_t k = 0;
    for (size_t i = 0; i < n && i < 20; i++) k += (size_t)snprintf(h + k, sizeof(h) - k, "%02x ", b[i]);
    h[k ? k - 1 : 0] = 0;
    xc_log("[wifi] %s %u B: %s%s", dir, (unsigned)n, h, n > 20 ? " ..." : "");
}

static int op_send_cmd(void *u, const uint8_t *b, size_t n) {
    (void)u;
    if (g_core.dev < XW_DEV_READY && g_hex_out < 10) { g_hex_out++; hexlog("-> EP1", b, n); }
    return g_tr == TR_EHCI ? xhs_send(1, b, n) : ring_put(&g_tx_cmd, b, n);
}
static int op_send_data(void *u, const uint8_t *b, size_t n) {
    (void)u;
    return g_tr == TR_EHCI ? xhs_send(2, b, n) : ring_put(&g_tx_data, b, n);
}

static void op_rx_eth(void *u, uint8_t *f, size_t n) {
    (void)u;
    if (!g_wl_added || n < 14) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
    if (!p) { g_rx_pbuf_fail++; return; }
    pbuf_take(p, f, (u16_t)n);
    /* v0.21.13: o vigia diz em que protocolo do lwIP a hw1 parou */
    {
        unsigned et = n >= 14 ? (unsigned)((f[12] << 8) | f[13]) : 0, pr = n >= 24 ? f[23] : 0;
        xwd_crumb(et == 0x0806 ? "wifi: lwIP ARP" : et != 0x0800 ? "wifi: lwIP outro" : pr == 17 ? "wifi: lwIP UDP"
                  : pr == 6 ? "wifi: lwIP TCP" : pr == 1 ? "wifi: lwIP ICMP" : "wifi: lwIP IP");
    }
    if (ethernet_input(p, &g_wl) != ERR_OK) pbuf_free(p);
    xwd_crumb("wifi: lwIP voltou");
}

static err_t wl_linkoutput(struct netif *nf, struct pbuf *p) {
    (void)nf;
    static uint8_t f[1600];
    if (p->tot_len > sizeof(f)) return ERR_IF;
    pbuf_copy_partial(p, f, p->tot_len, 0);
    return xw_tx_eth(&g_core, f, p->tot_len) == 0 ? ERR_OK : ERR_IF;
}

static err_t wl_init(struct netif *nf) {
    nf->name[0] = 'w';
    nf->name[1] = 'l';
    nf->output = etharp_output;
    nf->linkoutput = wl_linkoutput;
    nf->hwaddr_len = 6;
    memcpy(nf->hwaddr, g_core.mac, 6);
    nf->mtu = 1500;
    nf->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
#if LWIP_NETIF_HOSTNAME
    nf->hostname = "orbita";
#endif
    return ERR_OK;
}

static void op_link(void *u, int up) {
    (void)u;
    if (up) {
        if (!g_wl_added) {
            ip_addr_t z;
            ip_addr_set_zero(&z);
            if (!netif_add(&g_wl, &z, &z, &z, NULL, wl_init, ethernet_input)) {
                xc_log("[wifi] netif_add falhou");
                return;
            }
            g_wl_added = 1;
            netif_set_up(&g_wl);
            netif_set_link_up(&g_wl);
            dhcp_start(&g_wl);
            xc_log("[wifi] interface wl no ar, pedindo DHCP");
        } else {
            netif_set_link_up(&g_wl);   /* lwIP 1.4.1: dhcp_network_changed (REBOOTING) */
            xc_log("[wifi] link de volta, renovando DHCP");
        }
        g_wl_up = 1;
        if (g_want_save_on && g_want_save.ssid_len == g_core.tssid_len &&
            !memcmp(g_want_save.ssid, g_core.tssid, (size_t)g_core.tssid_len)) {
            xlk_lock(&g_lk, "wifi status " XLK_HERE);
            g_save = g_want_save;
            g_save_ready = 1;
            xlk_unlock(&g_lk);
            memset(&g_want_save, 0, sizeof(g_want_save));
            g_want_save_on = 0;
        }
    } else {
        g_wl_up = 0;
        g_dhcp_ok = 0;
        if (g_wl_added) netif_set_link_down(&g_wl);
    }
}

static void op_log(void *u, const char *l) { (void)u; xc_log("%s", l); }

static int op_rand(void *u, uint8_t *b, size_t n) {
    (void)u;
    if (!g_drbg_ok) return -1;
    return mbedtls_ctr_drbg_random(&g_drbg, b, n) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------- passos */

static uint32_t ip_u32(const ip_addr_t *a) {
    return ((uint32_t)ip4_addr1(a) << 24) | ((uint32_t)ip4_addr2(a) << 16) | ((uint32_t)ip4_addr3(a) << 8) |
           ip4_addr4(a);
}

static void watch_netif(uint32_t now) {
    if (!g_wl_added) return;
    int ok = g_wl_up && g_wl.ip_addr.addr != 0 && g_wl.dhcp && g_wl.dhcp->state == DHCP_BOUND;
    if (ok && !g_dhcp_ok) {
        uint32_t ip = ip_u32(&g_wl.ip_addr), gw = ip_u32(&g_wl.gw);
        xc_log("[wifi] DHCP: %u.%u.%u.%u gw %u.%u.%u.%u%s", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255,
               gw >> 24, gw >> 16 & 255, gw >> 8 & 255, gw & 255, g_cable ? " (cabo segue como rota)" : "");
        if (!g_cable) {
            if (netif_default != &g_wl) {
                netif_set_default(&g_wl);
                xc_log("[wifi] rota padrão agora é o Wi-Fi");
            }
            g_is_default = 1;
            np_dns_reload(gw);
        }
    }
    g_dhcp_ok = ok;
    if (ok && now - g_last_stat >= STAT_EVERY_MS) {
        g_last_stat = now;
        xc_log("[wifi] stat rx=%u tx=%u rx_drop=%u tx_drop=%u fila_cheia=%u usb_err tx=%u rx=%u rssi=%d",
               (unsigned)g_core.rx_frames, (unsigned)g_core.tx_frames, (unsigned)g_core.rx_drop,
               (unsigned)g_core.tx_drop, g_tx_data.drops, g_tx_data.errs, g_rx_errs, g_core.rssi);
        if (g_tr == TR_EHCI) {
            char o[220];
            xhs_stats(o, sizeof(o));
            xc_log("[wifi] %s", o);
        }
    }
}

static void process_rx(uint32_t now) {
    if (g_rx_cmd.done) {
        int st = g_rx_cmd.status, n = g_rx_cmd.xferred;
        if ((st == 0 || st == UR_ERR_DATAUNDERRUN) && n > 0) {
            memcpy(g_rx_copy, g_rx_cmd.buf, (size_t)n);
            if (g_core.dev < XW_DEV_READY && g_hex_in < 10) { g_hex_in++; hexlog("<- EP1", g_rx_copy, (size_t)n); }
            xw_on_cmd_in(&g_core, g_rx_copy, (size_t)n, now);
            g_rx_err_run = 0;
        } else if (st && st != UR_ERR_CANCELLED) {
            g_rx_errs++;
            if (++g_rx_err_run == 20) xc_log("[wifi] EP1 IN: erro USB %d repetido", st);
        }
        if (st != UR_ERR_CANCELLED) rx_queue(&g_rx_cmd, g_ep_cin);
    }
    for (int i = 0; i < RX_DATA_N; i++) {
        rx_slot_t *s = &g_rx_data[i];
        if (!s->done) continue;
        int st = s->status, n = s->xferred;
        if ((st == 0 || st == UR_ERR_DATAUNDERRUN) && n > 0) {
            memcpy(g_rx_copy, s->buf, (size_t)n);
            xw_on_data_in(&g_core, g_rx_copy, (size_t)n, now);
        } else if (st && st != UR_ERR_CANCELLED) {
            g_rx_errs++;
        }
        if (st != UR_ERR_CANCELLED) rx_queue(s, g_ep_din);
    }
}

static void take_requests(void) {
    int sc, cn, cs, fg;
    xw_saved_t t, sv;
    int have_sv;
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    sc = r_scan; cn = r_connect; cs = r_connect_saved; fg = r_forget;
    t = r_target;
    sv = g_saved;
    have_sv = g_have_saved;
    r_scan = r_connect = r_connect_saved = r_forget = 0;
    memset(&r_target, 0, sizeof(r_target));
    xlk_unlock(&g_lk);
    if (fg) {
        xw_disconnect(&g_core);
        g_want_save_on = 0;
    }
    if (cn) {
        xw_connect(&g_core, t.ssid, t.ssid_len, t.secure ? t.pmk : NULL, 1);
        g_want_save = t;
        g_want_save_on = 1;
    } else if (cs && have_sv) {
        xw_connect(&g_core, sv.ssid, sv.ssid_len, sv.secure ? sv.pmk : NULL, 1);
    }
    if (sc) xw_scan(&g_core);
    memset(&t, 0, sizeof(t));
    memset(&sv, 0, sizeof(sv));
}

static void publish(uint32_t now) {
    xw_status_t *p = &g_pub;
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    p->seq++;
    p->present = g_present;
    p->dev = g_core.dev;
    p->link = g_core.link;
    p->err = g_core.err;
    p->err_code = g_core.err_code;
    p->want = g_core.want;
    p->dhcp_ok = g_dhcp_ok;
    p->ip = g_dhcp_ok ? ip_u32(&g_wl.ip_addr) : 0;
    p->is_default = g_is_default && g_dhcp_ok;
    p->cable = g_cable;
    memcpy(p->ssid, g_core.tssid, sizeof(p->ssid));
    p->rssi = g_core.rssi;
    p->scanning = g_core.job == 1 || g_core.want_user_scan;
    const char *stg = !g_attached ? g_stage
                    : g_core.dev == XW_DEV_PROBE ? "perguntando ao firmware"
                    : g_core.dev == XW_DEV_BOOT ? "subindo o firmware da flash"
                    : g_core.dev == XW_DEV_INIT ? "configurando o adaptador" : "";
    snprintf(p->stage, sizeof(p->stage), "%s", stg ? stg : "");
    p->stage_ms = g_t_act ? now - g_t_act : 0;
    snprintf(p->why, sizeof(p->why), "%s", g_why);
    snprintf(p->err_detail, sizeof(p->err_detail), "%s", g_core.err_detail);
    p->transport = g_tr;
    p->rx_rearms = g_rw_rearms;
    p->rx_reassoc = g_rw_reassoc;
    p->fw_resets = g_fw_resets;
    p->rx_recovered = g_rec_ok;
    if (p->scan_seq != g_core.scan_seq) {
        p->scan_seq = g_core.scan_seq;
        p->nnets = g_core.nbss;
        for (int i = 0; i < g_core.nbss; i++) {
            const mwx_bss_t *b = &g_core.bss[i];
            xw_net_t *o = &p->nets[i];
            memcpy(o->ssid, b->ssid, sizeof(o->ssid));
            o->ssid_len = b->ssid_len;
            o->rssi = b->rssi;
            o->sec = b->sec;
            o->chan = b->channel;
        }
    }
    xlk_unlock(&g_lk);
}

static void start_core(uint32_t now) {
    g_attached = 1;
    g_ops.send_cmd = op_send_cmd;
    g_ops.send_data = op_send_data;
    g_ops.rx_eth = op_rx_eth;
    g_ops.link = op_link;
    g_ops.log = op_log;
    g_ops.rand = op_rand;
    xw_init(&g_core, &g_ops);
    xw_start(&g_core, now);
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    xw_saved_t sv = g_saved;
    int have = g_have_saved;
    xlk_unlock(&g_lk);
    if (have && !g_cable) {
        xc_log("[wifi] rede salva \"%s\": conectando sozinho", sv.ssid);
        xw_connect(&g_core, sv.ssid, sv.ssid_len, sv.secure ? sv.pmk : NULL, 0);
    } else if (have) {
        xc_log("[wifi] rede salva \"%s\", mas o cabo pegou DHCP: Wi-Fi parado", sv.ssid);
    }
    memset(&sv, 0, sizeof(sv));
}

/* falha visível (a tela mostra o motivo; nunca "Ligando" pra sempre) */
static void glue_fail(uint32_t now, const char *why) {
    char w[320];
    if (g_hs_why[0] && g_tr != TR_EHCI) snprintf(w, sizeof(w), "%s (high speed: %s)", why, g_hs_why);
    else snprintf(w, sizeof(w), "%s", why);
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    snprintf(g_why, sizeof(g_why), "%s", w);
    xlk_unlock(&g_lk);
    xc_log("[wifi] FALHOU depois de %u ms: %s", g_t_act ? (unsigned)(now - g_t_act) : 0u, w);
    g_dead = 1;
    g_core.dev = XW_DEV_FAILED;
    g_core.err = XW_E_DEVICE;
    g_stage = "";
    if (g_wl_up) op_link(NULL, 0);
}

static void hs_rx(int ep, const uint8_t *b, size_t n, void *u) {
    (void)u;
    xwd_crumb(ep == 1 ? "wifi: RX comando/evento" : "wifi: RX dados -> driver");
    if (n > sizeof(g_rx_copy)) return;
    memcpy(g_rx_copy, b, n);
    uint32_t now = now_ms();
    if (ep == 1 && n >= 4 && mwx_get32(g_rx_copy) == 0xBEADC0DEu) {
        static unsigned k;
        if (k++ < 5) xc_log("[wifi] EP1 IN trouxe DADOS (0xBEADC0DE, %u B): tratando como RX de dados", (unsigned)n);
        xw_on_data_in(&g_core, g_rx_copy + 4, n - 4, now);
    } else if (ep == 1) {
        if (g_core.dev < XW_DEV_READY && g_hex_in < 10) { g_hex_in++; hexlog("<- EP1", g_rx_copy, n); }
        xw_on_cmd_in(&g_core, g_rx_copy, n, now);
    } else {
        if (ep == 3) {
            static unsigned k3;
            if (k3++ < 5) xc_log("[wifi] EP3 IN trouxe %u B: tratando como RX de dados", (unsigned)n);
        }
        if (g_core.link == XW_LINK_UP && g_rx_last_t) {
            uint32_t gap = now - g_rx_last_t;
            if (gap > g_gap_max) g_gap_max = gap;
            if (gap >= 500u) g_gaps_slow++;
        }
        g_rx_last_t = now;
        xw_on_data_in(&g_core, g_rx_copy, n, now);
    }
}

/* v0.21.7: held = 1 dentro de um xc_usb_hold (o pendrive está no tinyehci,
 * em outra thread). O Wi-Fi continua: só não procura/abre o adaptador (isso
 * mexe nas estruturas do tinyehci) e não religa o anel se o pendrive estiver
 * no mesmo EHCI. O pendrive e o Wi-Fi andam ao mesmo tempo. */
/* v0.21.7: o usb_do_poll sem o USBStorage_Init (tinyehci): o que o
 * usb_cfe_timer (static no libxenon) faz + os ticks do GIP e do log. Pra hw1
 * durante um xc_usb_hold: controles e receptor seguem, o pendrive fica com a
 * outra thread. */
void usbgip_tick(void);
void usblog_tick(void);
void xw_usb_poll_no_storage(void) {
    for (int i = 0; i < usb_buscnt; i++)
        if (usb_buses[i]) {
            usb_poll(usb_buses[i]);
            usb_daemon(usb_buses[i]);
        }
    usbgip_tick();
    usblog_tick();
}

/* v0.21.9: recuperação do RX parado, em degraus, tudo na hw1:
 *  0) com o link de pé e nada chegando há 1,5 s, um ARP pro gateway (o
 *     detector precisa de TX pra ver o silêncio mesmo com o app quieto);
 *  1) detector (3 s sem RX com TX saindo): loga tudo, refaz os anéis de
 *     entrada e REASSOCIA na hora, procurando só no canal atual;
 *  2) sem RX de volta 3 s depois: reinício do firmware (FUNC_SHUTDOWN,
 *     anéis zerados, FUNC_INIT/GET_HW_SPEC/MAC_CONTROL e reconexão);
 *  3) nada ainda em 10 s: volta ao 0 (o detector tenta de novo). */
enum { REC_IDLE = 0, REC_REASSOC, REC_FW_SHUT, REC_FW_UP };
static int          g_rec = REC_IDLE;
static uint32_t     g_rec_t = 0, g_rec_t0 = 0;
static unsigned     g_rec_rx_up = 0;       /* RX quando o link voltou */
static int          g_rec_up_seen = 0;
static char         g_rt_ssid[MWX_SSID_MAX + 1];
static int          g_rt_len = 0, g_rt_sec = 0, g_rt_ch = 0;
static uint8_t      g_rt_pmk[32];
static uint32_t     g_probe_t = 0;
static unsigned     g_probes = 0;

static void hs_rx(int ep, const uint8_t *b, size_t n, void *u);

static void rec_log_dump(void) {
    char o[600];
    xhs_stats(o, sizeof(o));
    xc_log("[wifi] RX PARADO há %u ms com TX saindo (disparo %u): %s", (unsigned)XRW_STALL_MS, g_rw.fires, o);
    char ev[160];
    xw_ev_summary(&g_core, ev, sizeof(ev));
    xc_log("[wifi] RX PARADO: quadros rx=%u tx=%u descartados rx=%u tx=%u, pad=%u, pbuf_alloc falhou=%u, "
           "eventos %u (%s), comandos %u (lentos %u, atrasados %u, pior 0x%04x %u ms), ARP de prova %u",
           (unsigned)g_core.rx_frames, (unsigned)g_core.tx_frames, (unsigned)g_core.rx_drop, (unsigned)g_core.tx_drop,
           (unsigned)g_core.tx_padded, g_rx_pbuf_fail, (unsigned)g_core.ev_total, ev, (unsigned)g_core.cmd_total,
           (unsigned)g_core.cmd_slow, (unsigned)g_core.cmd_late, g_core.cmd_worst, (unsigned)g_core.cmd_worst_ms, g_probes);
    for (int w = 1; w < xhs_npipes(); w++)
        if (xhs_dump_pipe(w, o, sizeof(o)) > 0) xc_log("[wifi] RX PARADO: %s", o);
    char why[160];
    if (xhs_check(why, sizeof(why)) != 0) xc_log("[wifi] RX PARADO: anel inconsistente: %s", why);
}

static void rec_save_target(void) {
    g_rt_len = g_core.tssid_len;
    memcpy(g_rt_ssid, g_core.tssid, sizeof(g_rt_ssid));
    g_rt_sec = g_core.tsecure;
    if (g_rt_sec) memcpy(g_rt_pmk, g_core.tpmk, sizeof(g_rt_pmk));
    g_rt_ch = g_core.cur.channel;
}

static void rec_wipe_target(void) {
    memset(g_rt_pmk, 0, sizeof(g_rt_pmk));
    g_rt_sec = 0;
}

/* sucesso: link de pé de novo e o RX andou depois disso */
static int rec_ok(uint32_t now) {
    if (g_core.link != XW_LINK_UP) { g_rec_up_seen = 0; return 0; }
    if (!g_rec_up_seen) { g_rec_up_seen = 1; g_rec_rx_up = xhs_rx_count(); g_probe_t = 0; return 0; }
    (void)now;
    return xhs_rx_count() - g_rec_rx_up >= 2;
}

static void rx_watch(uint32_t now) {
    if (!g_rw_inited) { xrw_init(&g_rw, now); g_rw_inited = 1; }
    {   /* v0.21.17: o poll em si anda? (fora da trava dele) */
        static unsigned runs0 = 0;
        static uint32_t t_runs = 0;
        unsigned r = xhs_poll_runs();
        if (r != runs0) { runs0 = r; t_runs = now; }
        else if (now - t_runs >= 1000u) {
            t_runs = now;
            xc_log("[wifi] o poll do EHCI não termina há 1 s (reentradas %u, trava velha %u, pisada %u)", xhs_poll_reenters(),
                   xhs_guard_stale(), xhs_guard_trampled());
        }
    }
    unsigned rx = xhs_rx_count();
    /* 0) prova: link de pé, DHCP feito, 1,5 s sem nada chegando -> ARP pro gateway */
    if (g_core.link == XW_LINK_UP && g_wl_added && g_wl_up && g_dhcp_ok && g_wl.gw.addr &&
        now - g_rx_last_t >= 1500u && now - g_probe_t >= 1500u) {
        g_probe_t = now;
        g_probes++;
        etharp_request(&g_wl, &g_wl.gw);
    }
    /* v0.21.29: conectado e sem DHCP há 8 s = o RX de dados está morto mesmo
     * que respostas de comando cheguem (elas enganam o xrw_step: na tela do
     * boot ficava 60 s "conectando"). Mesma recuperação do RX parado. */
    static uint32_t t_up = 0, t_kick = 0;
    int dhcp_stuck = 0;
    if (g_core.link != XW_LINK_UP || g_dhcp_ok) t_up = 0;
    else if (!t_up) t_up = now;
    else if (now - t_up >= 8000u && (!t_kick || now - t_kick >= 15000u)) {
        t_kick = now;
        t_up = now;
        dhcp_stuck = 1;
        xc_log("[wifi] conectado há 8 s sem resposta do DHCP: reassociando");
    }
    switch (g_rec) {
    case REC_IDLE:
        /* v0.21.17: o TX que conta é o TENTADO pelo core (fila do USB aceitou
         * ou não), não as conclusões do USB: com o poll parado as conclusões
         * também param e o detector ficava cego */
        if (!xrw_step(&g_rw, now, g_core.link == XW_LINK_UP, rx, (uint32_t)(g_core.tx_frames + g_core.tx_drop)) &&
            !dhcp_stuck) return;
        rec_log_dump();
        {
            /* v0.21.11: o rearme antigo reescrevia o overlay de um QH vivo (o
             * HC podia gravar o dele por cima e seguir de outro qTD). Agora só
             * acerta o software com onde o HC parou. */
            int s1 = xhs_in_resync(1, hs_rx, NULL), s3 = xhs_in_resync(3, hs_rx, NULL), s5 = xhs_in_resync(5, hs_rx, NULL);
            g_rw_rearms++;
            xc_log("[wifi] RX PARADO: anéis de entrada conferidos com o HC (EP1 IN %d, EP2 IN %d, EP3 IN %d quadros; -1 = "
                   "nada fora do lugar); reassociando já (recuperação %u)", s1, s3, s5, g_rw_rearms);
        }
        rec_save_target();
        g_rec_t0 = g_rec_t = now;
        g_rec_up_seen = 0;
        if (dhcp_stuck) xw_avoid_current(&g_core);   /* v0.21.29: tenta o outro ponto de acesso */
        if (g_rt_len > 0 && xw_reconnect(&g_core) == 0) {
            g_rw_reassoc++;
            g_rec = REC_REASSOC;
        } else {
            xc_log("[wifi] RX PARADO: sem rede alvo pra reassociar");
            rec_wipe_target();
        }
        return;
    case REC_REASSOC:
        if (rec_ok(now)) {
            g_rec_ok++;
            xc_log("[wifi] recuperado pela reassociação em %u ms (RX andando de novo)", (unsigned)(now - g_rec_t0));
            rec_wipe_target();
            g_rec = REC_IDLE;
            xrw_init(&g_rw, now);
            return;
        }
        if (now - g_rec_t < 3000u) return;
        /* 2) reinício do firmware */
        g_fw_resets++;
        xc_log("[wifi] reassociação não trouxe o RX em %u ms (link %d): reiniciando o firmware (vez %u)",
               (unsigned)(now - g_rec_t), g_core.link, g_fw_resets);
        {
            uint8_t b[64];
            size_t n = mwx_build_func_shutdown(b, 0x7f00);
            xhs_send(1, b, n);
        }
        g_rec = REC_FW_SHUT;
        g_rec_t = now;
        return;
    case REC_FW_SHUT:
        if (now - g_rec_t < 300u) return;   /* a resposta do FUNC_SHUTDOWN chega (o core loga como atrasada) */
        {
            int a3 = xhs_rx_rearm(3), a5 = xhs_rx_rearm(5), a1 = xhs_rx_rearm(1);
            int o2 = xhs_out_reset(2), o4 = xhs_out_reset(4);
            if (g_wl_up) op_link(NULL, 0);
            xc_log("[wifi] firmware: anéis zerados (presos: EP1 IN %d, EP2 IN %d, EP3 IN %d, EP1 OUT %d, EP2 OUT %d); "
                   "FUNC_INIT de novo", a1, a3, a5, o2, o4);
        }
        g_tx_seen = xhs_tx_done(2);
        g_t_act = now;            /* v0.21.11: o prazo de 30 s do "ligando" recomeça */
        g_last_diag = now;
        xw_init(&g_core, &g_ops);
        xw_start(&g_core, now);
        if (g_rt_len > 0) {
            xw_connect(&g_core, g_rt_ssid, g_rt_len, g_rt_sec ? g_rt_pmk : NULL, 0);
            g_core.fast_chan = g_rt_ch;
        }
        rec_wipe_target();
        g_rec = REC_FW_UP;
        g_rec_t = now;
        g_rec_up_seen = 0;
        return;
    case REC_FW_UP:
        if (g_core.dev == XW_DEV_FAILED) {
            xc_log("[wifi] o firmware não voltou depois do reinício");
            g_rec = REC_IDLE;
            xrw_init(&g_rw, now);
            return;
        }
        if (rec_ok(now)) {
            g_rec_ok++;
            xc_log("[wifi] recuperado pelo reinício do firmware em %u ms desde o RX parado", (unsigned)(now - g_rec_t0));
            g_rec = REC_IDLE;
            xrw_init(&g_rw, now);
            return;
        }
        if (now - g_rec_t > 10000u) {
            xc_log("[wifi] reinício do firmware não trouxe o RX em 10 s (link %d, dev %d): o detector tenta de novo",
                   g_core.link, g_core.dev);
            g_rec = REC_IDLE;
            xrw_init(&g_rw, now);
        }
        return;
    }
}

/* v0.21.9: cada passo da hw1 aqui dentro tem nome (o vigia diz em qual
 * parou) e tempo: passo de 150 ms ou mais vira uma linha (no máximo 1/s). */
static struct {
    uint32_t    can_a[4];
    const char *crumb;
    uint64_t    t;
    uint32_t    can_b[4];
} __attribute__((aligned(128))) g_cr = { { 0x5ca1ab1eu, 0x5ca1ab1eu, 0x5ca1ab1eu, 0x5ca1ab1eu }, NULL, 0,
                                         { 0x5ca1ab1eu, 0x5ca1ab1eu, 0x5ca1ab1eu, 0x5ca1ab1eu } };
static unsigned     g_cr_trampled = 0;
#define g_crumb   g_cr.crumb
#define g_crumb_t g_cr.t
static const char  *g_slow_name = NULL;
static uint32_t     g_slow_max = 0, g_slow_log_t = 0;
static void crumb(const char *n) {
    uint64_t t = mftb();
    {   /* v0.21.17: canários em volta (o g_crumb_t veio pisado no console) */
        int bad = 0;
        for (int i = 0; i < 4; i++) bad |= g_cr.can_a[i] != 0x5ca1ab1eu || g_cr.can_b[i] != 0x5ca1ab1eu;
        if (bad) {
            g_cr_trampled++;
            if (g_cr_trampled <= 5)
                xc_log("[wifi] MEMÓRIA PISADA nos sub-passos (#%u): %08x %08x | %08x %08x", g_cr_trampled, (unsigned)g_cr.can_a[0],
                       (unsigned)g_cr.can_a[3], (unsigned)g_cr.can_b[0], (unsigned)g_cr.can_b[3]);
            for (int i = 0; i < 4; i++) { g_cr.can_a[i] = 0x5ca1ab1eu; g_cr.can_b[i] = 0x5ca1ab1eu; }
            g_crumb = NULL;
        }
        if (g_crumb && t < g_crumb_t) g_crumb = NULL;   /* tempo pra trás: não mede */
    }
    if (g_crumb) {
        uint32_t ms = (uint32_t)((t - g_crumb_t) / (PPC_TIMEBASE_FREQ / 1000));
        if (ms > g_slow_max) { g_slow_max = ms; g_slow_name = g_crumb; }
        if (ms >= 150u) {
            uint32_t now = (uint32_t)(t / (PPC_TIMEBASE_FREQ / 1000));
            if (now - g_slow_log_t >= 1000u) {
                g_slow_log_t = now;
                xc_log("[wifi] hw1 lenta: passo \"%s\" levou %u ms", g_crumb, ms);
            }
        }
    }
    g_crumb = n;
    g_crumb_t = t;
    xwd_crumb(n ? n : "wifi: fim do passo");
}

static void step(int held);
void xw_sys_step(void) { step(0); }
void xw_sys_step_held(void) { step(1); }

static volatile int       g_in_step = 0;
static volatile uintptr_t g_step_frame = 0;
static unsigned g_step_reenter = 0, g_step_stale = 0;
static void step_body(int held);
/* v0.21.13: o passo do Wi-Fi nunca entra em si mesmo (a bomba do xcw_idle
 * chamada de dentro de um caminho que já está no passo) */
static void wake_body(void);
static void step(int held) {
    if (!g_inited) return;
    if (g_wake_req && !held && !g_in_step) wake_body();   /* v0.21.43 */
    if (g_off) {
        /* v0.21.29: Wi-Fi desligado, mas se ele chegou a abrir o EHCI as portas
         * desse controlador são nossas: o vigia de hotplug (controle com fio
         * na frente) continua, sozinho (a cada 10 ms, só registradores) */
        if ((g_tr == TR_EHCI || g_watch_claimed) && !held) xhs_port_watch();
        return;
    }
    /* v0.21.17: reentrada de verdade = pilha mais funda que a de quem entrou;
     * fora disso a trava está velha (memória pisada): limpa e loga */
    uintptr_t fr = (uintptr_t)__builtin_frame_address(0);   /* a pilha de verdade (o ASan move as locais) */
    if (g_in_step) {
        if (fr < g_step_frame) { g_step_reenter++; return; }
        g_step_stale++;
        if (g_step_stale <= 5) xc_log("[wifi] trava do passo VELHA (#%u): limpando", g_step_stale);
    }
    g_step_frame = fr;
    g_in_step = 1;
    step_body(held);
    g_in_step = 0;
}

/* v0.21.18: canários. Valor esperado: 0xC0DE0000 | (lado << 8) | índice */
static unsigned g_canary_hits = 0;
static uint32_t can_val(int side, int i) { return 0xC0DE0000u | ((uint32_t)side << 8) | (uint32_t)i; }
static void canary_fill(uint32_t *a, uint32_t *b) {
    for (int i = 0; i < CBOX_WORDS; i++) { a[i] = can_val(0, i); b[i] = can_val(1, i); }
}
static void canary_check(const char *what, uint32_t *a, uint32_t *b, size_t inner) {
    for (int side = 0; side < 2; side++) {
        uint32_t *w = side ? b : a;
        int first = -1, last = -1;
        for (int i = 0; i < CBOX_WORDS; i++)
            if (w[i] != can_val(side, i)) { if (first < 0) first = i; last = i; }
        if (first < 0) continue;
        g_canary_hits++;
        if (g_canary_hits <= 8) {
            char hx[8 * 9 + 1];
            int k = 0;
            for (int i = first; i < first + 8 && i < CBOX_WORDS; i++) k += snprintf(hx + k, sizeof(hx) - (size_t)k, "%08x ", (unsigned)w[i]);
            /* lado 0 = ANTES do bloco (escrita pra trás / índice negativo); lado 1 = DEPOIS do fim (estouro) */
            xc_log("[wifi] MEMÓRIA PISADA %s %s (%u B): palavras %d..%d do canário, a partir de %d B %s; achado: %s",
                   side ? "depois do fim do" : "antes do começo do", what, (unsigned)inner, first, last,
                   side ? first * 4 : (CBOX_WORDS - first) * 4, side ? "depois do fim" : "antes do começo", hx);
        }
        for (int i = 0; i < CBOX_WORDS; i++) w[i] = can_val(side, i);
    }
}
unsigned xw_canary_hits(void) { return g_canary_hits; }

/* v0.21.43: o usuário pediu Wi-Fi pelo menu (Procurar / Conectar). Antes, o
 * Wi-Fi desligado pelo cabo (ou que falhou) nunca mais voltava: a tela ficava
 * em "Ligando o Wi-Fi" pra sempre. Aqui (hw1, fora do hold) desfaz o
 * desligamento, devolve o EHCI do vigia e zera a procura do adaptador. O
 * cabo deixa de ser a rota se o Wi-Fi pegar DHCP (escolha do usuário). */
static void wake_body(void) {
    g_wake_req = 0;
    if (!g_off && !g_off_req && !g_dead) return;
    int was_off = g_off || g_off_req, was_dead = g_dead;
    if (g_watch_claimed) { xhs_watch_release(); g_watch_claimed = 0; }
    g_off = 0;
    g_off_req = 0;
    g_off_t = 0;
    g_cable = 0;
    g_dead = 0;
    g_hs_tried = 0;
    g_enum_tried = 0;
    g_released = 0;
    g_last_find = 0;
    g_t_act = 0;   /* o passo marca de novo ("ligando o adaptador") */
    if (g_present == XW_PRESENT_NO) g_present = XW_PRESENT_UNKNOWN;
    if (g_attached) g_core.dev = XW_DEV_READY;   /* o adaptador seguiu com o firmware (só saiu da rede) */
    g_activate = 1;
    xc_log("[wifi] ligado pelo menu (%s%s%s)", was_off ? "estava desligado pelo cabo" : "",
           was_off && was_dead ? ", " : "", was_dead ? "tinha falhado" : "");
}

/* v0.21.29: 1 = segue o passo normal (mandando o deauth), 0 = desligou */
static int wifi_off_step(uint32_t now) {
    if (!g_off_t) {
        g_off_t = now ? now : 1;
        if (g_attached && g_core.dev == XW_DEV_READY) {
            xw_disconnect(&g_core);
            return 1;   /* uns passos pro deauth sair pelo USB */
        }
    } else if (g_attached && now - g_off_t < 300u) {
        return 1;
    }
    if (g_wl_added) {
        if (netif_default == &g_wl) netif_set_default(&netif);
        dhcp_stop(&g_wl);
        netif_set_down(&g_wl);
        netif_remove(&g_wl);
        g_wl_added = 0;
    }
    g_wl_up = 0;
    g_dhcp_ok = 0;
    g_is_default = 0;
    g_activate = 0;
    g_core.dev = XW_DEV_OFF;
    g_core.link = XW_LINK_IDLE;
    publish(now);
    xc_log("[wifi] desligado: o cabo tem internet (sem passo, sem interface)");
    if (g_tr != TR_EHCI && xhs_watch_claim() == 0) g_watch_claimed = 1;
    g_off = 1;
    return 0;
}

static void step_body(int held) {
    canary_check("g_core", g_corebox.can_a, g_corebox.can_b, sizeof(xw_core_t));
    canary_check("g_rx_copy", g_rxbox.can_a, g_rxbox.can_b, sizeof(g_rxbox.buf));
    xwd_beat(held ? "rede: wifi (pendrive em uso)" : "rede: wifi");
    uint32_t now = now_ms();
    uint32_t since = (uint32_t)((mftb() - g_t0_tb) / (PPC_TIMEBASE_FREQ / 1000));
    if (g_off_req && !wifi_off_step(now)) return;
    if (!g_attached) {
        if (g_dead || held) goto pub;
        uint32_t every = g_present == XW_PRESENT_NO ? 1000u : (g_present == XW_PRESENT_YES && !g_activate) ? 500u
                       : FIND_EVERY_MS;
        if (now - g_last_find < every) goto pub;
        g_last_find = now;
        int hb = 0, hp = 0, ha = 0;
        int in_ehci = xhs_find(&hb, &hp, &ha);
        usbdev_t *d = find_dev();
        if (!g_activate) {
            /* só detecta (nada é mexido sem rede salva ou a tela pedir) */
            if ((in_ehci || d) && g_present != XW_PRESENT_YES) {
                g_present = XW_PRESENT_YES;
                if (in_ehci) xc_log("[wifi] 045e:0765 no EHCI %d porta %d, endereço %d (parado até a tela do Wi-Fi pedir)", hb, hp, ha);
                else xc_log("[wifi] 045e:0765 no OHCI (parado até a tela do Wi-Fi pedir)");
            } else if (!in_ehci && !d && g_present == XW_PRESENT_UNKNOWN && since > PRESENT_WAIT_MS) {
                g_present = XW_PRESENT_NO;
                xc_log("[wifi] adaptador 045e:0765 não encontrado (Xbox Fat ou sem Wi-Fi)");
            } else if (g_present == XW_PRESENT_NO && since > FIND_GIVEUP_MS) {
                g_dead = 1;
            }
            goto pub;
        }
        if (!g_t_act) {
            g_t_act = now;
            g_stage = "procurando o adaptador no USB";
            xc_log("[wifi] ligando o adaptador (EHCI: %s, OHCI: %s)", in_ehci ? "sim" : "não", d ? "sim" : "não");
        }
        if (in_ehci || d) g_present = XW_PRESENT_YES;
        /* 1) high speed no EHCI, com os nossos QHs (v0.21.1) */
        if (!g_hs_tried && !d) {
            if (in_ehci) {
                g_hs_tried = 1;
                g_stage = "abrindo em high speed (EHCI)";
                xhs_eps_t e;
                if (xhs_open(&e) == 0) {
                    g_tr = TR_EHCI;
                    start_core(now);
                    goto pub;
                }
                snprintf(g_hs_why, sizeof(g_hs_why), "%s", xhs_why());
                xc_log("[wifi] high speed falhou: %s. Tentando full speed pelo OHCI", g_hs_why);
            } else if (now - g_t_act > HS_FIND_MS && !g_enum_tried) {
                /* v0.21.41: antes de desistir, identifica as portas que o tinyehci
                 * ainda não endereçou (boot sem cabo: o adaptador fica assim) e dá
                 * mais HS_FIND_MS pra ele aparecer */
                g_enum_tried = 1;
                int ne = xw_ehci_enum_unknown();
                xc_log("[wifi] 045e:0765 ainda não apareceu: identifiquei %d porta(s) USB sem endereço, procurando de novo", ne);
                g_t_act = now;
                goto pub;
            } else if (now - g_t_act > HS_FIND_MS) {
                g_hs_tried = 1;
                char pt[140];
                xw_ehci_ports_text(pt, sizeof(pt));   /* v0.21.43: aparece na foto da tela */
                snprintf(g_hs_why, sizeof(g_hs_why), "não está no EHCI; portas %s", pt);
                xc_log("[wifi] 045e:0765 não está no EHCI (portas %s)", pt);
            } else {
                goto pub;
            }
        }
        /* 2) full speed: porta pro OHCI companheiro */
        if (!d && !g_released) {
            int rb = 0, rp = 0;
            if (xw_ehci_release_wifi(&rb, &rp, 1)) {
                g_released = 1;
                g_t_rel = now;
                g_rel_bus = rb;
                g_rel_port = rp;
                g_stage = "esperando o OHCI (full speed)";
                xc_log("[wifi] porta EHCI %d/%d entregue ao OHCI (full speed); porta agora %08x", rb, rp,
                       (unsigned)xw_ehci_port_status(rb, rp));
            } else if (g_hs_tried && !in_ehci) {
                glue_fail(now, "adaptador não está em nenhum controlador USB");
                goto pub;
            }
        }
        if (d) {
            xc_log("[wifi] 045e:0765 no OHCI (barramento %d, endereço %d), %u ms depois de ligar",
                   d->ud_bus ? d->ud_bus->ub_num : -1, d->ud_address, (unsigned)(now - g_t_act));
            g_stage = "abrindo em full speed (OHCI)";
            if (attach(d) != 0) {
                glue_fail(now, "não consegui abrir o adaptador no OHCI (ver as linhas [wifi] endpoint)");
                goto pub;
            }
            g_attached = 1;
            g_tr = TR_OHCI;
            start_core(now);
            goto pub;
        }
        if (g_released) {
            if (now - g_last_diag >= 1000 && now - g_t_rel <= FS_WAIT_MS) {
                g_last_diag = now;
                xc_log("[wifi] OHCI ainda sem o adaptador (%u ms); porta EHCI %08x", (unsigned)(now - g_t_rel),
                       (unsigned)xw_ehci_port_status(g_rel_bus, g_rel_port));
                char o[220];
                for (int i = 0; i < usb_buscnt; i++)
                    if (usb_buses[i] && ohci_debug_hc(usb_buses[i], o, sizeof(o)) > 0) xc_log("[wifi] %s", o);
            }
            if (now - g_t_rel > FS_WAIT_MS)
                glue_fail(now, "o adaptador não apareceu no OHCI (full speed) depois da troca de porta");
        }
        goto pub;
    }
    if (g_dead) {
        if (g_core.dev != XW_DEV_FAILED) glue_fail(now, "o adaptador sumiu do USB");
        goto pub;
    }
    if (held && g_tr == TR_EHCI && xhs_shared()) goto pub;
    crumb("wifi: pedidos");
    take_requests();
    if (g_tr == TR_EHCI) {
        int rl = !held || !xhs_shared();
        crumb("wifi: colhendo o USB (RX + lwIP)");
        xhs_poll2(hs_rx, NULL, rl);
        crumb("wifi: tick do driver");
        xw_tick(&g_core, now);
        crumb("wifi: colhendo o USB de novo");
        xhs_poll2(hs_rx, NULL, rl);
    } else {
        process_rx(now);
        xw_tick(&g_core, now);
        ring_flush(&g_tx_cmd);
        ring_flush(&g_tx_data);
    }
    {   /* v0.21.4: quadros de dados que saíram de verdade pelo USB (EAPOL no handshake) */
        unsigned td = g_tr == TR_EHCI ? xhs_tx_done(2) : g_tx_data.reaped;
        if (td != g_tx_seen) {
            xw_note_data_tx(&g_core, td - g_tx_seen);
            g_tx_seen = td;
        }
    }
    crumb("wifi: portas USB");
    if (g_tr == TR_EHCI) xhs_port_watch();   /* v0.21.20: hotplug no EHCI do Wi-Fi */
    crumb("wifi: vigia do RX");
    if (g_tr == TR_EHCI) rx_watch(now);
    crumb("wifi: diagnóstico");
    /* diagnóstico enquanto não fica pronto; prazo total */
    if (g_core.dev != XW_DEV_READY && g_core.dev != XW_DEV_FAILED) {
        if (now - g_last_diag >= 2000) {
            g_last_diag = now;
            char o[220];
            if (g_tr == TR_EHCI) {
                xhs_stats(o, sizeof(o));
                xc_log("[wifi] etapa %d (%u ms): %s", g_core.dev, (unsigned)(now - g_t_act), o);
                xhs_dump(o, sizeof(o));
                xc_log("[wifi] %s", o);
            } else {
                xc_log("[wifi] etapa %d (%u ms): OHCI rx_err=%u tx_cmd_err=%u fila=%u", g_core.dev,
                       (unsigned)(now - g_t_act), g_rx_errs, g_tx_cmd.errs, g_tx_cmd.drops);
            }
        }
        if (now - g_t_act > ACT_TIMEOUT_MS)
            glue_fail(now, g_core.dev == XW_DEV_PROBE ? "o firmware não respondeu ao probe"
                         : g_core.dev == XW_DEV_BOOT ? "o firmware da flash não subiu (sem INIT_DONE)"
                         : "o adaptador não terminou a configuração");
    } else if (g_core.dev == XW_DEV_FAILED && !g_why[0]) {
        glue_fail(now, g_core.dev == XW_DEV_FAILED ? "o firmware não respondeu (ver as linhas [wifi] acima)" : "");
    }
    /* v0.21.10: estatísticas do firmware (GET_LOG/RSSI/taxa): a cada 5 s na
     * sessão, 10 s fora; a linha sai 300 ms depois (as respostas chegam) */
    if (g_tr == TR_EHCI && g_core.dev == XW_DEV_READY && g_core.link == XW_LINK_UP) {
        uint32_t every = g_session ? (xw_diag_lite ? 30000u : 5000u) : 10000u;
        if (now - g_fwq_t >= every && xw_fw_query(&g_core)) { g_fwq_t = now; g_fwq_log_at = now + 300u; }
        if (g_fwq_log_at && (int32_t)(now - g_fwq_log_at) >= 0) {
            char o[320];
            g_fwq_log_at = 0;
            xw_fwstats_text(&g_core, o, sizeof(o));
            xc_log("[wifi] firmware: %s", o);
        }
    }
    if (g_tr == TR_EHCI && g_core.dev == XW_DEV_READY && now - g_last_hslog >= (g_session ? (xw_diag_lite ? 30000u : 5000u) : 10000u)) {
        char o[220];
        xhs_stats(o, sizeof(o));
        if (g_session || strcmp(o, g_last_hs) != 0) {
            g_last_hslog = now;
            snprintf(g_last_hs, sizeof(g_last_hs), "%s", o);
            xc_log("[wifi] %s", o);
            /* v0.21.9: o RX degradando antes de parar (buraco entre quadros com
             * o link de pé), eventos/comandos do firmware, hw1 lenta */
            xc_log("[wifi] RX buraco máx %u ms, buracos >=0,5 s %u; eventos %u; comandos %u (lentos %u, atrasados %u); "
                   "ARP de prova %u; recuperações %u/%u/%u; anéis ressincronizados %u; hw1 passo mais lento %s %u ms",
                   g_gap_max, g_gaps_slow, (unsigned)g_core.ev_total, (unsigned)g_core.cmd_total,
                   (unsigned)g_core.cmd_slow, (unsigned)g_core.cmd_late, g_probes, g_rw_reassoc, g_fw_resets, g_rec_ok,
                   xhs_resyncs(), g_slow_name ? g_slow_name : "-", g_slow_max);
            g_gap_max = 0;
            g_slow_max = 0;
            g_slow_name = NULL;
            if (g_session) {
                unsigned w, f, us;
                xc_netlog_stats(&w, &f, &us);
                xc_log("[wifi] sessão: link %d, DHCP %d, quadros rx=%u tx=%u descartes rx=%u tx=%u, NETLOG %u setores "
                       "(%u falhas, pior %u us); reentradas passo %u poll %u, orçamento %u; pilha hw1 pico %u KB de %u%s",
                       g_core.link, g_dhcp_ok, (unsigned)g_core.rx_frames,
                       (unsigned)g_core.tx_frames, (unsigned)g_core.rx_drop, (unsigned)g_core.tx_drop, w, f, us,
                       g_step_reenter, xhs_poll_reenters(), xhs_poll_budget_hits(), stream_net_stack_peak() / 1024,
                       stream_net_stack_size() / 1024, stream_net_stack_canary_ok() ? "" : " ESTOUROU");
                xc_log("[wifi] locks: ordem invertida %s; tentativas desistidas %u (log beta %u); travas velhas poll %u passo %u, "
                       "pisadas %u (canários do g_core/RX %u); polls completos %u; portas: mudanças %u, pro OHCI %u, "
                       "high speed bloqueado %u", xlk_inversion_text(), xlk_try_fails(),
                       xlu_line_drops(), xhs_guard_stale(), g_step_stale, xhs_guard_trampled(), g_canary_hits,
                       xhs_poll_runs(), xhs_port_changes(), xhs_port_handoffs(), xhs_port_hs_blocked());
            }
        }
    }
    crumb("wifi: netif/DHCP");
    watch_netif(now);
    crumb("wifi: ARP");
    /* ARP: o libxenon nunca chama o etharp_tmr (o enet comentou). Com o Wi-Fi
     * no ar ele roda: um pedido ARP perdido não deixa o gateway "pendente"
     * pra sempre (no 1.4.1 só entrada nova manda pedido). */
    if (g_wl_added && now - g_last_arp >= ARP_TMR_INTERVAL) {
        g_last_arp = now;
        etharp_tmr();
    }
pub:
    crumb("wifi: publicando");
    if (now - g_last_pub >= PUB_EVERY_MS) {
        g_last_pub = now;
        publish(now);
    }
    crumb(NULL);   /* fora do passo não conta */
}

/* ------------------------------------------------------------- main */

void xw_sys_init(int cable) {
    canary_fill(g_corebox.can_a, g_corebox.can_b);   /* v0.21.18 */
    canary_fill(g_rxbox.can_a, g_rxbox.can_b);
    memset(&g_pub, 0, sizeof(g_pub));
    g_pub.present = XW_PRESENT_UNKNOWN;
    g_cable = cable;
    g_pub.cable = cable;
    g_t0_tb = mftb();
    char txt[256];
    int n = xc_store_load(XW_STORE_FILE, txt, sizeof(txt) - 1);
    if (n > 0) {
        xw_saved_t s;
        if (xw_store_parse(txt, (size_t)n, &s) == 0) {
            g_saved = s;
            g_have_saved = 1;
            g_pub.saved = 1;
            g_pub.saved_secure = s.secure;
            memcpy(g_pub.saved_ssid, s.ssid, sizeof(g_pub.saved_ssid));
            xc_log("[wifi] rede salva: \"%s\" (%s)", s.ssid, s.secure ? "WPA2" : "aberta");
        } else {
            xc_log("[wifi] %s inválido, ignorado", XW_STORE_FILE);
        }
        memset(&s, 0, sizeof(s));
    }
    memset(txt, 0, sizeof(txt));
    mbedtls_entropy_init(&g_ent);
    mbedtls_ctr_drbg_init(&g_drbg);
    static const unsigned char pers[] = "orbita-wifi-wpa2";
    g_drbg_ok = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent, pers, sizeof(pers)) == 0;
    if (!g_drbg_ok) xc_log("[wifi] DRBG falhou: WPA2 indisponível");
    g_activate = !cable;   /* v0.21.5: sem cabo liga já (rede salva ou a lista do boot); com cabo, nunca sozinho */
    if (cable) { g_off_req = 1; g_core.dev = XW_DEV_OFF; }   /* v0.21.29: cabo no boot: Wi-Fi nem começa */
    xc_log("[wifi] iniciado (cabo %s)", cable ? "com DHCP: Wi-Fi não liga sozinho" : "sem DHCP: ligando o adaptador");
    __asm__ __volatile__("sync" ::: "memory");
    g_inited = 1;
}

void xw_sys_status(xw_status_t *o) {
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    *o = g_pub;
    o->saved = g_have_saved;
    o->saved_secure = g_saved.secure;
    memcpy(o->saved_ssid, g_saved.ssid, sizeof(o->saved_ssid));
    if (!g_have_saved) o->saved_ssid[0] = 0;
    xlk_unlock(&g_lk);
}

int xw_sys_scan(void) {
    g_activate = 1;
    g_wake_req = 1;   /* v0.21.43 */
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    r_scan = 1;
    xlk_unlock(&g_lk);
    return 0;
}

int xw_sys_connect(const char *ssid, int ssid_len, const uint8_t *pmk) {
    if (ssid_len < 1 || ssid_len > MWX_SSID_MAX) return -1;
    g_activate = 1;
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    memset(&r_target, 0, sizeof(r_target));
    memcpy(r_target.ssid, ssid, (size_t)ssid_len);
    r_target.ssid_len = ssid_len;
    g_wake_req = 1;   /* v0.21.43 */
    r_target.secure = pmk != NULL;
    if (pmk) memcpy(r_target.pmk, pmk, 32);
    r_connect = 1;
    xlk_unlock(&g_lk);
    return 0;
}

int xw_sys_connect_saved(void) {
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    int ok = g_have_saved;
    if (ok) { r_connect_saved = 1; g_activate = 1; g_wake_req = 1; }   /* v0.21.43: + acorda */
    xlk_unlock(&g_lk);
    return ok ? 0 : -1;
}

int xw_sys_forget(void) {
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    r_forget = 1;
    g_have_saved = 0;
    memset(&g_saved, 0, sizeof(g_saved));
    g_save_ready = 0;
    memset(&g_save, 0, sizeof(g_save));
    xlk_unlock(&g_lk);
    int r = xc_store_remove(XW_STORE_FILE);
    xc_log("[wifi] rede esquecida (%s removido: %d)", XW_STORE_FILE, r);
    return 0;
}

void xw_sys_main_poll(void) {
    xw_saved_t s;
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    int ready = g_save_ready;
    if (ready) { s = g_save; g_save_ready = 0; memset(&g_save, 0, sizeof(g_save)); }
    xlk_unlock(&g_lk);
    if (!ready) return;
    char txt[256];
    size_t n = xw_store_format(txt, sizeof(txt), &s);
    int r = n ? xc_store_save(XW_STORE_FILE, txt, n) : -1;
    memset(txt, 0, sizeof(txt));
    xc_log("[wifi] rede \"%s\" salva em %s: %s", s.ssid, XW_STORE_FILE, r == 0 ? "ok" : "falhou (sem pendrive?)");
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    g_saved = s;   /* mesmo sem pendrive: vale até reiniciar */
    g_have_saved = 1;
    xlk_unlock(&g_lk);
    memset(&s, 0, sizeof(s));
}

int xw_sys_boot_wait(void) {
    if (!g_inited) return 0;
    uint32_t since = (uint32_t)((mftb() - g_t0_tb) / (PPC_TIMEBASE_FREQ / 1000));
    if (since > BOOT_WAIT_MS) return 0;
    xlk_lock(&g_lk, "wifi status " XLK_HERE);
    int have = g_have_saved;
    xw_status_t p = g_pub;
    xlk_unlock(&g_lk);
    if (!have || p.cable) return 0;
    if (p.present == XW_PRESENT_NO) return 0;
    if (p.dev == XW_DEV_FAILED) return 0;
    if (p.dhcp_ok) return 0;
    if (p.dev == XW_DEV_READY && !p.want && p.link != XW_LINK_UP) return 0;   /* desistiu (senha/segurança) */
    return 1;
}

/* ---------------------------------------------------------------------
 * v0.21.5: XC360/WIFILOG.TXT — as linhas [wifi] (e [net] cabo) também no
 * pendrive, pra ler pelo FTP quando não houve internet nenhuma. Qualquer
 * thread junta num anel em RAM (32 KB, cheio = descarta as mais antigas);
 * xw_logfile_flush grava (128 KB por arquivo, o anterior vira WIFILOG.OLD).
 * Nada de senha/PMK/chave nessas linhas (ver WIFI.md).
 * ------------------------------------------------------------------- */
#define WLOG_RING   (32 * 1024)
#define WLOG_CAP    (128 * 1024)
static char         g_wl_ring[WLOG_RING];
static size_t       g_wl_len = 0;
static unsigned     g_wl_dropped = 0;
static xlk_t        g_wl_lk = XLK_INIT("WIFILOG");
static volatile int g_wl_flushing = 0;
static char         g_wl_ver[48] = "";
static uint64_t     g_wl_t0 = 0;

void xw_logfile_boot(const char *version) {
    snprintf(g_wl_ver, sizeof(g_wl_ver), "%s", version ? version : "?");
    g_wl_t0 = mftb();
    char l[96];
    snprintf(l, sizeof(l), "===== boot orbita %s =====", g_wl_ver);
    xw_logfile_line(l);
}

void xw_logfile_line(const char *text) {
    if (!text) return;
    char l[320];
    uint64_t t = g_wl_t0 ? (mftb() - g_wl_t0) / (PPC_TIMEBASE_FREQ / 10) : 0;
    int n = snprintf(l, sizeof(l), "t=%u.%u %s\n", (unsigned)(t / 10), (unsigned)(t % 10), text);
    if (n <= 0) return;
    if (n >= (int)sizeof(l)) { n = (int)sizeof(l) - 1; l[n - 1] = '\n'; }
    if (!xlk_try(&g_wl_lk, "xw_logfile_line", 2000)) { g_wl_dropped++; return; }
    if (g_wl_len + (size_t)n > WLOG_RING) {
        /* descarta linhas inteiras do começo */
        size_t cut = 0;
        while (cut < g_wl_len && g_wl_len - cut + (size_t)n > WLOG_RING) {
            char *e = memchr(g_wl_ring + cut, '\n', g_wl_len - cut);
            cut = e ? (size_t)(e - g_wl_ring) + 1 : g_wl_len;
            g_wl_dropped++;
        }
        memmove(g_wl_ring, g_wl_ring + cut, g_wl_len - cut);
        g_wl_len -= cut;
    }
    memcpy(g_wl_ring + g_wl_len, l, (size_t)n);
    g_wl_len += (size_t)n;
    xlk_unlock(&g_wl_lk);
}

int xw_logfile_pending(void) { return g_wl_len > 0; }

void xw_sys_set_session(int on) { g_session = on ? 1 : 0; }
int  xw_sys_session(void) { return g_session; }

/* v0.21.29: thread de rede (cable_watch): o cabo pegou DHCP e já é a rota.
 * O Wi-Fi não volta a ser rota sozinho nem liga sozinho daqui pra frente. */
void xw_sys_cable_up(void) {
    if (g_cable) return;
    g_cable = 1;
    g_pub.cable = 1;
    g_is_default = 0;
    g_activate = 0;
    g_off_req = 1;
    xc_log("[wifi] cabo assumiu a rota: desligando o Wi-Fi");
}

/* v0.21.10: um setor do anel pro NETLOG.TXT (direto no disco). Com sessão,
 * a cada 2 s (ou já, com 2 KB juntos); sem sessão o anel fica pro FatFs. */
int xw_diag_lite = 0;     /* v0.21.25: DIAG.TXT diag=lite -> estatísticas do firmware/HS a cada 30 s no jogo */
int xw_diag_netlog = 1;   /* v0.21.16: DIAG.TXT netlog=0 -> sem escrita direta no pendrive na sessão */
int xw_logfile_raw_step(void) {
    static uint64_t last = 0;
    static char out[512];
    if (!xw_diag_netlog) return 0;
    if (!g_session || !g_wl_len || !xc_netlog_ready()) return 0;
    if (g_wl_flushing || xwd_usb_holder() >= 0) return 0;
    uint64_t t = mftb();
    if (g_wl_len < 2048 && last && (t - last) < 2ULL * PPC_TIMEBASE_FREQ) return 0;
    last = t;
    size_t n = 0;
    xlk_lock(&g_wl_lk, "WIFILOG " XLK_HERE);
    n = g_wl_len < 488 ? g_wl_len : 488;
    if (n < g_wl_len) {   /* corta no fim de uma linha quando dá */
        size_t k = n;
        while (k > 0 && g_wl_ring[k - 1] != '\n') k--;
        if (k > 0) n = k;
    }
    memcpy(out, g_wl_ring, n);
    memmove(g_wl_ring, g_wl_ring + n, g_wl_len - n);
    g_wl_len -= n;
    xlk_unlock(&g_wl_lk);
    return xc_netlog_write(out, n) == 0;
}

/* Quem tem o pendrive livre (main sem worker na hw 3, ou o próprio worker). */
int xw_logfile_flush(void) {
    static char buf[WLOG_RING + 128];
    if (__sync_lock_test_and_set(&g_wl_flushing, 1)) return 0;
    size_t n;
    unsigned dropped;
    xlk_lock(&g_wl_lk, "WIFILOG " XLK_HERE);
    n = g_wl_len;
    memcpy(buf, g_wl_ring, n);
    g_wl_len = 0;
    dropped = g_wl_dropped;
    g_wl_dropped = 0;
    xlk_unlock(&g_wl_lk);
    int r = 0;
    if (dropped) {
        char d[64];
        int k = snprintf(d, sizeof(d), "(%u linhas perdidas: o anel encheu antes de gravar)\n", dropped);
        xc_store_append("WIFILOG.TXT", "WIFILOG.OLD", d, (size_t)k, WLOG_CAP);
    }
    if (n) r = xc_store_append("WIFILOG.TXT", "WIFILOG.OLD", buf, n, WLOG_CAP);
    __sync_lock_release(&g_wl_flushing);
    return r;
}
