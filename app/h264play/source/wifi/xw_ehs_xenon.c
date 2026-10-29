/*
 * orbita v0.21.1 — Wi-Fi interno em HIGH SPEED no EHCI, sem o tinyehci.
 *
 * v0.21.0 entregava a porta ao OHCI (full speed) e no Slim do usuário (Corona)
 * o adaptador nunca apareceu do outro lado. Aqui o 88W8786U fica no EHCI onde
 * o boot o achou (EHCI 1 porta 4 no Corona), com o endereço que o tinyehci já
 * deu (porta+1), e nós mesmos penduramos QHs/qTDs na fila assíncrona daquele
 * controlador:
 *
 *   async (cabeça do tinyehci) -> EP0 -> EP1 IN -> EP1 OUT -> EP2 IN -> EP2 OUT -> async
 *
 * - QH/qTD/buffers na memória NÃO cacheada (alias 0xA0000000 da RAM; DMA =
 *   endereço & 0x1FFFFFFF), escritos em little-endian; o token (Active) é
 *   escrito por último, depois de um sync.
 * - Cada cano é um anel circular de qTDs (no máximo n-1 em voo): o HC para no
 *   primeiro inativo e retoma quando o ativamos (EHCI 4.10.2).
 * - O tinyehci só faz transferência num controlador quando mexe em pendrive
 *   NESSE controlador; ele reescreve async->hw_next e os nossos QHs saem do
 *   anel. xhs_poll (hw 1, nunca dentro do xc_usb_hold) confere e religa.
 * - O tinyehci não toca a porta: o dispositivo está marcado busy (o
 *   USBStorage_Init pula).
 * Só a thread de rede (hw 1). Ver WIFI.md.
 */
#include "mwx_proto.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef XHS_HOSTTEST
/* v0.21.8: hosttest/wifi/stress (controlador EHCI falso numa thread, arena de
 * memória com endereços de DMA de 32 bits, registradores falsos) */
#include "xhs_host.h"
#else
#include <malloc.h>
#include <xetypes.h>
#include <byteswap.h>
#include <pci/io.h>
#include <ppc/cache.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <usb/tinyehci/ehci.h>
#include "../login/xc_port.h"
#endif

#include "xw_ehs_xenon.h"

#ifndef EHCI_HCD_COUNT
#define EHCI_HCD_COUNT 2
#endif
extern struct ehci_hcd ehci_hcds[EHCI_HCD_COUNT];

#ifndef XHS_HOSTTEST
#define UNC(p)  ((void *)((((uint32_t)(p)) & 0x1FFFFFFFu) | 0xA0000000u))
#define DMA(p)  (((uint32_t)(p)) & 0x1FFFFFFFu)
#define REG(base, off) ((u32)(uintptr_t)(base) + (u32)(off))
#endif
#ifndef LE
#define LE(v)   __builtin_bswap32((uint32_t)(v))
#endif

#define TOK_ACTIVE  (1u << 7)
#define TOK_HALT    (1u << 6)
#define TOK_ERRS    (0x3cu)        /* DBE, babble, xact, MMF */
#define TOK_IOC     (1u << 15)
#define TOK_TOGGLE  (1u << 31)
#define PID_OUT 0
#define PID_IN  1
#define PID_SETUP 2

/* qTD: next, alt, token, buf[5], buf_hi[5] = 13 palavras; QH: 4 + overlay(13) */
#define TD_STRIDE 64
#define QH_STRIDE 96
enum { Q_NEXT = 0, Q_INFO1, Q_INFO2, Q_CUR, Q_OV_NEXT, Q_OV_ALT, Q_OV_TOKEN, Q_OV_BUF };
enum { T_NEXT = 0, T_ALT, T_TOKEN, T_BUF };

#define MAXTD 12
enum { S_FREE = 0, S_PENDING, S_ACTIVE };

typedef struct {
    volatile uint32_t *qh;
    volatile uint32_t *td[MAXTD];
    uint8_t *buf[MAXTD];
    int      len[MAXTD], state[MAXTD], zlp[MAXTD];
    int      ntd, cap, in, ep, mps;
    int      prod, cons, sub;       /* prod: próximo a encher; sub: próximo a ativar; cons: próximo a colher */
    unsigned done, errs, halts, drops, rearms;
    const char *name;
    /* v0.21.11: ressincronia do anel de entrada (o HC parado num qTD que não
     * é o "segurador" do software) e um rastro das últimas operações */
    unsigned resyncs, resync_frames;
    uint32_t tr[16];
    unsigned trn;
    /* v0.21.12: suspeita de dessincronia só vira ação se durar >= 100 ms sem
     * NENHUM progresso (mesmo done, mesmo P, HC estacionado nas duas leituras) */
    uint64_t susp_tb;
    unsigned susp_done;
    int      susp_P;
    unsigned susp_seen;
    /* v0.21.15: o overlay do QH (memória que o HC reescreve) só é lido com o
     * cano sem progresso há >= 20 ms, no máximo a cada 10 ms */
    uint64_t prog_tb, ov_tb;
    unsigned ov_reads;
} hpipe_t;

/* v0.21.15: a última leitura de memória do HC que a hw1 ia fazer (o vigia
 * mostra isso se a hw1 parar: endereço físico, o que era e de qual cano).
 * Nenhum endereço escrito pelo HC é desreferenciado neste arquivo: o cur/next
 * do overlay só é COMPARADO com os nossos qTDs (slot_of). Só lemos tokens dos
 * nossos qTDs e o overlay dos nossos QHs, todos alocados aqui. */
static volatile uint32_t    g_last_dma = 0;
static const char *volatile g_last_what = "nada";
static const char *volatile g_last_pipe = "-";
#define PEEK(p, ptr, what) do { g_last_dma = DMA(ptr); g_last_what = (what); g_last_pipe = (p)->name; } while (0)
void xhs_last_access(char *o, size_t n) {
    snprintf(o, n, "última leitura da hw1 na memória do HC: %s do %s em 0x%08x", g_last_what, g_last_pipe, (unsigned)g_last_dma);
}
unsigned xhs_overlay_reads(void);

static const char *st_name(int st);

/* rastro: op (R colheu, A armou, X refez, S ressincronizou) | slot | cons | prod */
static void trace(hpipe_t *p, char op, int slot) {
    p->tr[p->trn++ & 15] = ((uint32_t)(uint8_t)op << 24) | ((uint32_t)(slot & 0xff) << 16) |
                           ((uint32_t)(p->cons & 0xff) << 8) | (uint32_t)(p->prod & 0xff);
}

static int     g_ok = 0;
static struct ehci_hcd *g_h;
static int     g_bus, g_port, g_addr;
static hpipe_t g_ctl, g_cin, g_cout, g_din, g_dout, g_d3;
/* v0.21.4: + EP3 IN (0x83) quando existe: o mwifiex não usa, mas o descritor
 * do 88W8786U tem; fica armado e qualquer coisa que chegar ali é logada e
 * tratada como dados */
static hpipe_t *g_pipes[6] = { &g_ctl, &g_cin, &g_cout, &g_din, &g_dout, &g_d3 };
static int     g_np = 5;
static unsigned g_relinks = 0;
static char    g_why[300];   /* v0.21.45: cabe a lista de endpoints */

/* v0.21.16: a linha de cache do Xenon tem 128 B. Com memalign(64, 64) um qTD
 * dividia a linha com o cabeçalho do malloc / outro bloco do heap: quando a
 * CPU sujava a parte do heap (cacheada) e a linha voltava pra RAM, os 128 B
 * inteiros eram regravados e o nosso qTD/QH (escrito pelo alias não
 * cacheado e pelo HC) voltava a um retrato velho: token Active de novo,
 * overlay antigo. Agora tudo que o HC lê/escreve ocupa linhas inteiras e só
 * dele (alinhamento >= 128 e tamanho múltiplo de 128). DIAG.TXT dma128=0
 * volta ao antigo (só pra bissecção). */
int xhs_dma128 = 1;
int xhs_diag_overlay = 1;   /* DIAG.TXT overlay=0: nenhuma leitura do overlay no poll */
static unsigned g_unhalts = 0;
static void *unc_alloc(size_t n, size_t align) {
    if (xhs_dma128) {
        if (align < 128) align = 128;
        n = (n + 127u) & ~(size_t)127u;
    }
    void *p = memalign(align, n);
    if (!p) return NULL;
    memset(p, 0, n);
    memdcbf(p, (int)n);             /* nenhuma linha suja do alias cacheado */
    return UNC(p);
}

#ifdef XHS_HOSTTEST
static inline void sync_(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void eieio_(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#else
static inline void sync_(void) { __asm__ __volatile__("sync" ::: "memory"); }
static inline void eieio_(void) { __asm__ __volatile__("eieio" ::: "memory"); }
#endif

/* v0.21.19: a hw1 (e o main junto: o núcleo 0 inteiro) parou numa LEITURA
 * não cacheada de um token de qTD nosso, num endereço válido (crumb da
 * v0.21.15: EP1 IN 0x0167e088, EP3 IN 0x0167d508). As mesmas páginas
 * físicas eram do heap: o alias cacheado (0x8...) delas era usado por
 * vizinhos, memcpy/prefetch podiam trazer a linha pro L2, e uma leitura
 * "cache-inhibited" de uma linha válida no L2 é indefinida no PowerPC (no
 * Xenon pode travar o núcleo). Agora:
 *   - DESCRITORES (QH/qTD): num pool ESTÁTICO só deles, com 4 KB de guarda
 *     de cada lado (o heap e os vizinhos nunca chegam perto), limpo do cache
 *     uma vez no começo (memdcbf) e daí só pelo alias não cacheado;
 *   - BUFFERS de dados: só pelo alias CACHEADO, como o tinyehci: dcbst antes
 *     de entregar ao HC (OUT/setup), dcbf depois que ele terminou e antes de
 *     ler (IN). Nenhuma linha é tocada pelos dois aliases.
 * DIAG.TXT dmapool=0 / bufcache=0 voltam ao jeito antigo (bissecção). */
int xhs_dma_pool = 1, xhs_buf_cached = 1, xhs_diag_ep3 = 0;
#define DPOOL_BYTES (32 * 1024)
#define DPOOL_GUARD 4096
#ifdef XHS_HOSTTEST
static uint8_t *g_dpool_raw = NULL;
#else
static uint8_t g_dpool_raw[DPOOL_BYTES + 2 * DPOOL_GUARD] __attribute__((aligned(4096)));
#endif
static size_t g_dpool_off = 0;
static int    g_dpool_ready = 0;

static void *desc_alloc(size_t n, size_t align) {
    if (!xhs_dma_pool) return unc_alloc(n, align);
    if (!g_dpool_ready) {
#ifdef XHS_HOSTTEST
        g_dpool_raw = (uint8_t *)memalign(4096, DPOOL_BYTES + 2 * DPOOL_GUARD);
        if (!g_dpool_raw) return NULL;
#endif
        memset(g_dpool_raw + DPOOL_GUARD, 0, DPOOL_BYTES);
        memdcbf(g_dpool_raw, DPOOL_BYTES + 2 * DPOOL_GUARD);   /* nenhuma linha do pool fica no cache */
        sync_();
        g_dpool_ready = 1;
    }
    if (align < 128) align = 128;
    n = (n + 127u) & ~(size_t)127u;
    size_t off = (g_dpool_off + align - 1) & ~(align - 1);
    if (off + n > DPOOL_BYTES) return NULL;
    g_dpool_off = off + n;
    return UNC(g_dpool_raw + DPOOL_GUARD + off);
}

static void *buf_alloc(size_t n, size_t align) {
    if (!xhs_buf_cached) return unc_alloc(n, align);
    if (align < 128) align = 128;
    n = (n + 127u) & ~(size_t)127u;
    void *p = memalign(align, n);
    if (!p) return NULL;
    memset(p, 0, n);
    memdcbf(p, (int)n);
    return p;   /* alias CACHEADO */
}

/* CPU escreveu (cacheado) -> HC vai ler */
static inline void buf_to_hc(void *b, size_t n) { if (xhs_buf_cached && n) { memdcbst(b, (int)n); sync_(); } }
/* HC escreveu e terminou -> CPU vai ler (descarta o que o cache tinha) */
static inline void buf_from_hc(void *b, size_t n) { if (xhs_buf_cached && n) memdcbf(b, (int)n); }

static int pipe_init(hpipe_t *p, const char *name, int ep, int in, int mps, int ntd, int cap, int dtc) {
    memset(p, 0, sizeof(*p));
    p->name = name;
    p->ep = ep;
    p->in = in;
    p->mps = mps > 0 ? mps : 512;
    p->ntd = ntd;
    p->cap = cap;
    p->qh = (volatile uint32_t *)desc_alloc(QH_STRIDE, 64);
    if (!p->qh) return -1;
    for (int i = 0; i < ntd; i++) {
        p->td[i] = (volatile uint32_t *)desc_alloc(TD_STRIDE, 64);
        p->buf[i] = cap ? (uint8_t *)buf_alloc((size_t)cap, cap >= 4096 ? 4096 : (size_t)cap) : NULL;
        if (!p->td[i] || (cap && !p->buf[i])) return -1;
    }
    for (int i = 0; i < ntd; i++) {
        p->td[i][T_NEXT] = LE(DMA(p->td[(i + 1) % ntd]));
        p->td[i][T_ALT] = LE(1);
        p->td[i][T_TOKEN] = 0;
    }
    uint32_t info1 = (uint32_t)g_addr | ((uint32_t)(ep & 0xf) << 8) | (2u << 12) | (dtc ? (1u << 14) : 0) |
                     ((uint32_t)(p->mps & 0x7ff) << 16) | (4u << 28);
    p->qh[Q_INFO1] = LE(info1);
    p->qh[Q_INFO2] = LE(1u << 30);
    p->qh[Q_CUR] = 0;
    p->qh[Q_OV_NEXT] = LE(DMA(p->td[0]));
    p->qh[Q_OV_ALT] = LE(1);
    p->qh[Q_OV_TOKEN] = 0;
    for (int i = 0; i < 10; i++) p->qh[Q_OV_BUF + i] = 0;
    return 0;
}

/* ativa o qTD i (dados já no buffer) */
static void td_arm(hpipe_t *p, int i, int pid, int len, int toggle, const void *ext_buf) {
    volatile uint32_t *t = p->td[i];
    uint32_t b = DMA(ext_buf ? ext_buf : p->buf[i]);
    t[T_BUF] = LE(b);
    for (int k = 1; k < 5; k++) t[T_BUF + k] = LE((b & ~0xfffu) + 0x1000u * (uint32_t)k);
    for (int k = 0; k < 5; k++) t[T_BUF + 5 + k] = 0;
    t[T_NEXT] = LE(DMA(p->td[(i + 1) % p->ntd]));
    t[T_ALT] = LE(1);
    eieio_();
    sync_();
    uint32_t tok = ((uint32_t)len << 16) | (3u << 10) | ((uint32_t)pid << 8) | TOK_IOC | TOK_ACTIVE |
                   (toggle ? TOK_TOGGLE : 0);
    t[T_TOKEN] = LE(tok);
    sync_();
}

/* ------------------------------------------------- anel assíncrono */

static uint32_t qh_link(volatile uint32_t *qh) { return DMA(qh) | (1u << 1); }   /* tipo QH */

static void chain_ours(void) {
    for (int i = 0; i < g_np; i++) {
        uint32_t next = i < g_np - 1 ? qh_link(g_pipes[i + 1]->qh) : (DMA(g_h->async) | (1u << 1));
        g_pipes[i]->qh[Q_NEXT] = LE(next);
    }
    sync_();
}

/* religa se o tinyehci tirou os nossos QHs do anel. 1 = religou. */
static int relink(void) {
    struct ehci_qh *a = g_h->async;
    memdcbf((void *)&a->hw_next, 4);
    uint32_t cur = LE(a->hw_next);
    uint32_t want = qh_link(g_ctl.qh);
    if ((cur & ~0x1fu) == (want & ~0x1fu)) return 0;
    chain_ours();
    a->hw_next = LE(want);
    memdcbst((void *)&a->hw_next, 4);
    sync_();
    g_relinks++;
    return 1;
}

/* ----------------------------------------------------------- controle */

/* EP0 síncrono (só no open): setup [+ dados IN/OUT] + status. len >= 0. */
static int ctrl(uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, uint8_t *data) {
    hpipe_t *p = &g_ctl;
    int s = p->prod, d = (s + 1) % p->ntd, st = (len ? (s + 2) : (s + 1)) % p->ntd;
    uint8_t *setup = p->buf[s];
    setup[0] = rt; setup[1] = req;
    setup[2] = (uint8_t)val; setup[3] = (uint8_t)(val >> 8);
    setup[4] = (uint8_t)idx; setup[5] = (uint8_t)(idx >> 8);
    setup[6] = (uint8_t)len; setup[7] = (uint8_t)(len >> 8);
    int in = (rt & 0x80) != 0;
    if (len && !in) memcpy(p->buf[d], data, len);
    buf_to_hc(setup, 8);
    if (len && !in) buf_to_hc(p->buf[d], len);
    /* arma de trás pra frente: o HC só vê a cadeia inteira */
    td_arm(p, st, (len && in) ? PID_OUT : PID_IN, 0, 1, NULL);
    if (len) td_arm(p, d, in ? PID_IN : PID_OUT, len, 1, NULL);
    td_arm(p, s, PID_SETUP, 8, 0, NULL);
    p->prod = (st + 1) % p->ntd;
    uint64_t t0 = mftb();
    int r = -1;
    for (;;) {
        relink();
        uint32_t ts = LE(p->td[st][T_TOKEN]), tsu = LE(p->td[s][T_TOKEN]);
        uint32_t td_ = len ? LE(p->td[d][T_TOKEN]) : 0;
        if ((tsu | ts | td_) & TOK_HALT) { r = -2; break; }
        if (!(ts & TOK_ACTIVE)) {
            r = len ? (int)len - (int)((td_ >> 16) & 0x7fff) : 0;
            break;
        }
        if (tb_diff_msec(mftb(), t0) > 600) { r = -3; break; }
        udelay(50);
    }
    if (r < 0) {
        /* desarma tudo e reinicia o overlay do EP0 */
        for (int i = 0; i < p->ntd; i++) p->td[i][T_TOKEN] = 0;
        p->qh[Q_OV_TOKEN] = 0;
        p->qh[Q_OV_NEXT] = LE(DMA(p->td[p->prod]));
        sync_();
    } else if (len && in) {
        buf_from_hc(p->buf[d], (size_t)r);
        memcpy(data, p->buf[d], (size_t)r);
    }
    p->cons = p->sub = p->prod;
    return r;
}

/* -------------------------------------------------------------- open */

static void log_ports(struct ehci_hcd *h, int b) {
    uint32_t hcs = read32(REG(h->caps, offsetof(struct ehci_caps, hcs_params))),
             hcc = read32(REG(h->caps, offsetof(struct ehci_caps, hcs_params) + 4));
    char s[200];
    int n = snprintf(s, sizeof(s), "[wifi] EHCI %d: %d portas, %d companheiros x %d portas, roteamento %s, 64 bits %s, portas:",
                     b, (int)(hcs & 15), (int)((hcs >> 12) & 15), (int)((hcs >> 8) & 15),
                     (hcs & (1u << 7)) ? "por tabela" : "fixo", (hcc & 1) ? "sim" : "não");
    for (int p = 0; p < (int)(hcs & 15) && p < 8 && n < (int)sizeof(s) - 12; p++) {
        n += snprintf(s + n, sizeof(s) - (size_t)n, " %08x", (unsigned)read32(REG(h->regs, offsetof(struct ehci_regs, port_status) + 4u * (unsigned)p)));
    }
    xc_log("%s", s);
}

const char *xhs_why(void) { return g_why; }

int xhs_find(int *bus, int *port, int *addr) {
    for (int b = 0; b < EHCI_HCD_COUNT; b++) {
        struct ehci_hcd *h = &ehci_hcds[b];
        if (!h->regs) continue;
        int np = h->num_port;
        if (np > EHCI_MAX_ROOT_PORTS) np = EHCI_MAX_ROOT_PORTS;
        for (int p = 0; p < np; p++) {
            struct ehci_device *d = &h->devices[p];
            if (!d->id) continue;
            if (!mwx_is_id(bswap_16(d->desc.idVendor), bswap_16(d->desc.idProduct))) continue;
            *bus = b;
            *port = p + 1;
            *addr = d->id;
            return 1;
        }
    }
    return 0;
}

int xhs_open(xhs_eps_t *eps) {
    int b, p, a;
    g_why[0] = 0;
    if (!xhs_find(&b, &p, &a)) { snprintf(g_why, sizeof(g_why), "045e:0765 sumiu do EHCI"); return -1; }
    g_h = &ehci_hcds[b];
    g_bus = b; g_port = p; g_addr = a;
    struct ehci_device *dev = &g_h->devices[p - 1];
    xc_log("[wifi] HS: adaptador %04x:%04x (bcdDevice %04x, classe %02x/%02x)", (unsigned)bswap_16(dev->desc.idVendor),
           (unsigned)bswap_16(dev->desc.idProduct), (unsigned)bswap_16(dev->desc.bcdDevice),
           (unsigned)dev->desc.bDeviceClass, (unsigned)dev->desc.bDeviceSubClass);
    dev->busy = 1;                                  /* o USBStorage_Init não reseta nem testa a porta */
    /* v0.21.20: nenhuma porta deste controlador pro tinyehci (hotplug é nosso) */
    for (int k = 0; k < g_h->num_port && k < EHCI_MAX_ROOT_PORTS; k++) g_h->devices[k].busy = 1;
    log_ports(g_h, b);
    uint32_t ps = read32(REG(g_h->regs, offsetof(struct ehci_regs, port_status) + 4u * (unsigned)(p - 1)));
    uint32_t cmd = read32(REG(g_h->regs, offsetof(struct ehci_regs, command))),
             sts = read32(REG(g_h->regs, offsetof(struct ehci_regs, status)));
    xc_log("[wifi] HS: EHCI %d porta %d endereço %d, porta %08x (%s%s%s), cmd %08x sts %08x", b, p, a, (unsigned)ps,
           (ps & 1) ? "conectado" : "SEM conexão", (ps & 4) ? ", habilitada" : ", desabilitada",
           (ps & (1u << 13)) ? ", DONO=companheiro" : "", (unsigned)cmd, (unsigned)sts);
    if (!(ps & 1) || (ps & (1u << 13)) || !(ps & 4)) {
        snprintf(g_why, sizeof(g_why), "porta EHCI %d/%d não está pronta (%08x)", b, p, (unsigned)ps);
        return -1;
    }
    if (!(cmd & 1) || !(cmd & 0x20)) {
        snprintf(g_why, sizeof(g_why), "fila assíncrona do EHCI %d desligada (cmd %08x)", b, (unsigned)cmd);
        return -1;
    }
    if (pipe_init(&g_ctl, "EP0", 0, 0, 64, 4, 512, 1) != 0) { snprintf(g_why, sizeof(g_why), "sem memória (EP0)"); return -1; }
    /* liga o EP0 no anel (os outros entram depois do descritor) */
    g_ctl.qh[Q_NEXT] = LE(DMA(g_h->async) | (1u << 1));
    sync_();
    {
        struct ehci_qh *as = g_h->async;
        as->hw_next = LE(qh_link(g_ctl.qh));
        memdcbst((void *)&as->hw_next, 4);
        sync_();
    }
    /* descritor de configuração (endpoints) */
    uint8_t cd[512];
    int r = ctrl(0x80, 6, 0x0200, 0, 9, cd);
    xc_log("[wifi] HS: GET_DESCRIPTOR(config, 9) -> %d", r);
    if (r < 9) { snprintf(g_why, sizeof(g_why), "EP0 não respondeu em high speed (%d)", r); return -1; }
    int total = cd[2] | (cd[3] << 8);
    if (total > (int)sizeof(cd)) total = sizeof(cd);
    r = ctrl(0x80, 6, 0x0200, 0, (uint16_t)total, cd);
    xc_log("[wifi] HS: config %d bytes (pedido %d), valor %d, %d interfaces", r, total, cd[5], cd[4]);
    if (r < 9) { snprintf(g_why, sizeof(g_why), "descritor de configuração (%d)", r); return -1; }
    memset(eps, 0, sizeof(*eps));
    /* v0.21.45: todos os endpoints de todas as interfaces, curto, pra foto da tela
     * (outros módulos de Wi-Fi: 045e:02ab/02b8). "if0/0 ff/ff: 81B512" = interface 0
     * alt 0 classe ff/ff, endpoint 0x81 bulk mps 512 (C controle, S iso, B bulk, I int) */
    char epd[200];
    {
        size_t k = 0;
        epd[0] = 0;
        for (int o = 0; o + 2 <= r && cd[o] >= 2 && k + 16 < sizeof(epd); o += cd[o]) {
            if (cd[o + 1] == 4 && o + 7 <= r)
                k += (size_t)snprintf(epd + k, sizeof(epd) - k, "%sif%d/%d %02x/%02x:", k ? " " : "", cd[o + 2], cd[o + 3],
                                      cd[o + 5], cd[o + 6]);
            else if (cd[o + 1] == 5 && o + 7 <= r)
                k += (size_t)snprintf(epd + k, sizeof(epd) - k, " %02x%c%d", cd[o + 2], "CSBI"[cd[o + 3] & 3],
                                      (cd[o + 4] | (cd[o + 5] << 8)) & 0x7ff);
        }
        xc_log("[wifi] HS: endpoints %s", epd);
    }
    int iface = -1, alt = 0;
    for (int o = 0; o + 2 <= r && cd[o] >= 2; o += cd[o]) {
        if (cd[o + 1] == 4 && o + 4 <= r) { iface = cd[o + 2]; alt = cd[o + 3]; }
        if (cd[o + 1] == 5 && o + 7 <= r && iface == 0 && alt == 0) {
            int ad = cd[o + 2], ty = cd[o + 3] & 3, mps = (cd[o + 4] | (cd[o + 5] << 8)) & 0x7ff, iv = cd[o + 6];
            xc_log("[wifi] HS: endpoint 0x%02x tipo %d mps %d intervalo %d", ad, ty, mps, iv);
            int num = ad & 15, in = (ad & 0x80) != 0;
            if (num == 1 && in) { eps->cin = ad; eps->cin_mps = mps; eps->cin_type = ty; }
            if (num == 1 && !in) { eps->cout = ad; eps->cout_mps = mps; }
            if (num == 2 && in && ty == 2) { eps->din = ad; eps->din_mps = mps; }
            if (num == 2 && !in && ty == 2) { eps->dout = ad; eps->dout_mps = mps; }
            if (num == 3 && in && ty == 2) { eps->d3in = ad; eps->d3in_mps = mps; }
        }
    }
    if (!eps->cin || !eps->cout || !eps->din || !eps->dout) {
        snprintf(g_why, sizeof(g_why), "faltam endpoints. %04x:%04x config %d: %s", (unsigned)bswap_16(dev->desc.idVendor),
                 (unsigned)bswap_16(dev->desc.idProduct), cd[5], epd);
        return -1;
    }
    r = ctrl(0x00, 9, cd[5], 0, 0, NULL);
    xc_log("[wifi] HS: SET_CONFIGURATION %d -> %d", cd[5], r);
    if (r < 0) { snprintf(g_why, sizeof(g_why), "SET_CONFIGURATION recusado (%d)", r); return -1; }
    if (pipe_init(&g_cin, "EP1 IN", 1, 1, eps->cin_mps, 4, 2048, 0) || pipe_init(&g_cout, "EP1 OUT", 1, 0, eps->cout_mps, 4, 1024, 0) ||
        pipe_init(&g_din, "EP2 IN", 2, 1, eps->din_mps, 8, 4096, 0) || pipe_init(&g_dout, "EP2 OUT", 2, 0, eps->dout_mps, 10, 2048, 0)) {
        snprintf(g_why, sizeof(g_why), "sem memória pros canos");
        return -1;
    }
    if (eps->d3in && xhs_diag_ep3 && pipe_init(&g_d3, "EP3 IN", 3, 1, eps->d3in_mps, 4, 4096, 0) == 0) {
        g_np = 6;
        for (int i = 0; i < g_d3.ntd - 1; i++) { td_arm(&g_d3, i, PID_IN, g_d3.cap, 0, NULL); g_d3.state[i] = S_ACTIVE; g_d3.len[i] = g_d3.cap; }
        g_d3.prod = g_d3.sub = g_d3.ntd - 1;
    }
    chain_ours();
    {
        struct ehci_qh *as = g_h->async;
        as->hw_next = LE(qh_link(g_ctl.qh));
        memdcbst((void *)&as->hw_next, 4);
        sync_();
    }
    /* RX sempre armados: EP1 IN 3 de 4, EP2 IN 7 de 8 */
    for (int i = 0; i < g_cin.ntd - 1; i++) { td_arm(&g_cin, i, PID_IN, g_cin.cap, 0, NULL); g_cin.state[i] = S_ACTIVE; g_cin.len[i] = g_cin.cap; }
    g_cin.prod = g_cin.sub = g_cin.ntd - 1;
    for (int i = 0; i < g_din.ntd - 1; i++) { td_arm(&g_din, i, PID_IN, g_din.cap, 0, NULL); g_din.state[i] = S_ACTIVE; g_din.len[i] = g_din.cap; }
    g_din.prod = g_din.sub = g_din.ntd - 1;
    g_ok = 1;
    xc_log("[wifi] HS: canos no ar (EP1 IN tipo %d mps %d, EP2 mps %d/%d), anel assíncrono do EHCI %d", eps->cin_type,
           eps->cin_mps, eps->din_mps, eps->dout_mps, b);
    return 0;
}

/* ------------------------------------------------------------- envio */

/* só copia (pode ser chamado de dentro do xc_usb_hold); o xhs_poll ativa */
int xhs_send(int ep, const uint8_t *b, size_t n) {
    if (!g_ok) return -1;
    hpipe_t *p = ep == 1 ? &g_cout : &g_dout;
    int zlp = n > 0 && (n % (size_t)p->mps) == 0;
    int need = zlp ? 2 : 1;
    int free_ = 0;
    for (int k = 0; k < need; k++) if (p->state[(p->prod + k) % p->ntd] == S_FREE) free_++;
    /* nunca encher o anel: o HC tem de parar num inativo */
    int busy = 0;
    for (int i = 0; i < p->ntd; i++) if (p->state[i] != S_FREE) busy++;
    if (free_ < need || (int)n > p->cap || busy + need > p->ntd - 1) { p->drops++; return -1; }
    int i = p->prod;
    memcpy(p->buf[i], b, n);
    buf_to_hc(p->buf[i], n);
    p->len[i] = (int)n;
    p->zlp[i] = 0;
    p->state[i] = S_PENDING;
    p->prod = (p->prod + 1) % p->ntd;
    if (zlp) {
        int z = p->prod;
        p->len[z] = 0;
        p->zlp[z] = 1;
        p->state[z] = S_PENDING;
        p->prod = (p->prod + 1) % p->ntd;
    }
    return 0;
}

/* --------------------------------------------------------------- poll */

static void halt_recover(hpipe_t *p) {
    PEEK(p, &p->qh[Q_OV_TOKEN], "token do overlay (halt)");
    uint32_t ov = LE(p->qh[Q_OV_TOKEN]);
    if (!(ov & TOK_HALT)) return;
    p->halts++;
    if (p->halts <= 5) xc_log("[wifi] HS: %s parou (token %08x), reiniciando o QH", p->name, (unsigned)ov);
    /* o qTD onde parou já voltou inativo com Halt: o colhedor marca como erro.
     * v0.21.9: o overlay vai pro primeiro qTD AINDA ativo depois dele (antes
     * apontava pro próprio qTD parado, inativo: o HC estacionava ali e o cano
     * nunca mais andava; era o EP3 IN desde o boot). Sem nenhum ativo, vai pro
     * qTD livre do fim do anel (o "segurador", rearmado pelo colhedor). */
    int nx = p->prod;
    for (int k = 0; k < p->ntd; k++) {
        int i = (p->cons + k) % p->ntd;
        if (p->state[i] == S_ACTIVE && (LE(p->td[i][T_TOKEN]) & TOK_ACTIVE)) { nx = i; break; }
    }
    p->qh[Q_OV_TOKEN] = LE(ov & TOK_TOGGLE);
    p->qh[Q_OV_NEXT] = LE(DMA(p->td[nx]));
    sync_();
}

/* v0.21.7: outro dispositivo (pendrive) no MESMO EHCI? Aí o tinyehci pode
 * estar mexendo na cabeça do anel durante um xc_usb_hold, e não religamos. */
int xhs_shared(void) {
    if (!g_ok) return 0;
    int np = g_h->num_port;
    if (np > EHCI_MAX_ROOT_PORTS) np = EHCI_MAX_ROOT_PORTS;
    for (int p = 0; p < np; p++)
        if (p != g_port - 1 && g_h->devices[p].id) return 1;
    return 0;
}

/* v0.21.11: o console mostrou o EP2 IN com o HC parado no qTD livre e os
 * outros 7 concluídos COM dados, enquanto o software tinha prod == cons (o
 * anel "vazio"): o colhedor parava (state[cons] livre) e o RX morria sem o
 * firmware ter culpa (os quadros estavam lá). A causa exata ainda não foi
 * achada (o rastro de 16 operações vai no log); aqui o software se acerta
 * com o HC sem mexer no overlay:
 *   - "estacionado": overlay inativo, sem Halt, e o qTD apontado pelo next
 *     (P) inativo. Isso só é normal com P == prod (o segurador);
 *   - fora disso (ou state[cons] livre, ou ativos != n-1) = dessincronia:
 *     entrega na ordem do HC os qTDs concluídos e não colhidos (do P+1 até o
 *     último executado), rearma P+1..P+n-2 de trás pra frente e por último o
 *     próprio P (o HC segue dali), e o livre vira P-1.
 * Sem o HC estacionado não mexe (ele está andando). Devolve quadros
 * entregues (>= 0) ou -1 se nada a fazer. */
static int slot_of(hpipe_t *p, uint32_t dma) {
    dma &= ~0x1fu;
    for (int i = 0; i < p->ntd; i++)
        if (DMA(p->td[i]) == dma) return i;
    return -1;
}

/* HC estacionado: overlay sem Active/Halt e o qTD do next inativo. -1 = andando. */
static int parked_at(hpipe_t *p) {
    PEEK(p, &p->qh[Q_OV_TOKEN], "token do overlay (estacionado?)");
    uint32_t ov = LE(p->qh[Q_OV_TOKEN]);
    if (ov & (TOK_ACTIVE | TOK_HALT)) return -1;
    PEEK(p, &p->qh[Q_OV_NEXT], "next do overlay");
    int P = slot_of(p, LE(p->qh[Q_OV_NEXT]));   /* só compara com os nossos qTDs; nunca desreferencia */
    if (P < 0) return -1;
    PEEK(p, &p->td[P][T_TOKEN], "token do qTD apontado pelo overlay");
    if (LE(p->td[P][T_TOKEN]) & TOK_ACTIVE) return -1;
    return P;
}

static int in_resync(hpipe_t *p, xhs_rx_fn fn, void *u, int force) {
    /* v0.21.12: a v0.21.11 agia na primeira leitura. Com o HC andando rápido
     * (início do stream), "overlay inativo + next inativo" também acontece
     * por um instante (o HC acabou X+1 entre as duas leituras): o software
     * "acertava" um anel que estava certo e rearmava qTDs que o HC estava
     * usando. Agora: a suspeita tem de durar >= 100 ms com o mesmo P, sem
     * nenhum quadro colhido nesse meio-tempo, e é conferida de novo na hora. */
    int P = parked_at(p);
    if (P < 0) { p->susp_tb = 0; return -1; }
    int act = 0;
    for (int i = 0; i < p->ntd; i++) act += p->state[i] == S_ACTIVE;
    int bad = P != p->prod || p->state[p->cons] != S_ACTIVE || act != p->ntd - 1;
    if (!bad) { p->susp_tb = 0; return force ? 0 : -1; }
    uint64_t now = mftb();
    if (!force) {
        if (!p->susp_tb || p->susp_done != p->done || p->susp_P != P) {
            p->susp_tb = now;
            p->susp_done = p->done;
            p->susp_P = P;
            p->susp_seen++;
            return -1;
        }
        if (tb_diff_msec(now, p->susp_tb) < 100) return -1;
        if (parked_at(p) != P) { p->susp_tb = 0; return -1; }   /* conferida de novo */
    }
    p->susp_tb = 0;
    char before[400];
    size_t k = (size_t)snprintf(before, sizeof(before), "prod=%d cons=%d ativos=%d HC em %d |", p->prod, p->cons, act, P);
    for (int i = 0; i < p->ntd && k + 14 < sizeof(before); i++)
        k += (size_t)snprintf(before + k, sizeof(before) - k, " %d%s:%08x", i, st_name(p->state[i]),
                              (unsigned)LE(p->td[i][T_TOKEN]));
    /* entrega os concluídos na ordem do HC: P+1 ... P-1 (o mais velho primeiro) */
    int got_n = 0;
    for (int k2 = 1; k2 < p->ntd; k2++) {
        int i = (P + k2) % p->ntd;
        if (p->state[i] != S_ACTIVE) continue;
        uint32_t tok = LE(p->td[i][T_TOKEN]);
        if (tok & TOK_ACTIVE) continue;
        int got = p->len[i] - (int)((tok >> 16) & 0x7fff);
        int err = (tok & (TOK_HALT | TOK_ERRS)) != 0;
        if (err) p->errs++;
        p->done++;
        p->state[i] = S_FREE;
        if (!err && got > 0 && fn) { buf_from_hc(p->buf[i], (size_t)got); fn(p->ep, p->buf[i], (size_t)got, u); got_n++; }
    }
    /* rearma de trás pra frente; P por último (o HC está esperando nele) */
    for (int k2 = p->ntd - 2; k2 >= 0; k2--) {
        int i = (P + k2) % p->ntd;
        td_arm(p, i, PID_IN, p->cap, 0, NULL);
        p->state[i] = S_ACTIVE;
        p->len[i] = p->cap;
    }
    int fr = (P + p->ntd - 1) % p->ntd;
    p->state[fr] = S_FREE;
    p->cons = P;
    p->prod = p->sub = fr;
    p->resyncs++;
    p->resync_frames += (unsigned)got_n;
    trace(p, 'S', P);
    if (p->resyncs <= 8) {
        char tr[16 * 12];
        size_t t = 0;
        for (unsigned q = 0; q < 16 && t + 12 < sizeof(tr); q++) {
            uint32_t e = p->tr[(p->trn + q) & 15];
            if (!e) continue;
            t += (size_t)snprintf(tr + t, sizeof(tr) - t, " %c%u/%u/%u", (char)(e >> 24), (e >> 16) & 0xff, (e >> 8) & 0xff,
                                  e & 0xff);
        }
        xc_log("[wifi] HS: %s fora de sincronia com o HC (#%u): %s; %d quadros entregues; rastro (op slot/cons/prod):%s",
               p->name, p->resyncs, before, got_n, tr);
    }
    return got_n;
}

int xhs_in_resync(int which, xhs_rx_fn fn, void *u) {
    if (!g_ok || which < 1 || which >= g_np) return -1;
    hpipe_t *p = g_pipes[which];
    if (!p->in) return -1;
    return in_resync(p, fn, u, 1);
}

/* v0.21.16: cada QH/qTD começa numa linha de 128 B (o tamanho já é múltiplo de 128) */
int xhs_dma_layout_ok(char *why, size_t n) {
    for (int k = 0; k < g_np; k++) {
        hpipe_t *p = g_pipes[k];
        if (!p->qh) continue;
        if (DMA(p->qh) & 127u) { snprintf(why, n, "QH do %s em 0x%08x", p->name, (unsigned)DMA(p->qh)); return -1; }
        for (int i = 0; i < p->ntd; i++)
            if (DMA(p->td[i]) & 127u) { snprintf(why, n, "qTD %d do %s em 0x%08x", i, p->name, (unsigned)DMA(p->td[i])); return -1; }
    }
    return 0;
}

unsigned xhs_overlay_reads(void) {
    unsigned n = 0;
    for (int k = 1; k < g_np; k++) n += g_pipes[k]->ov_reads;
    return n;
}

unsigned xhs_resyncs(void) { return g_cin.resyncs + g_din.resyncs + (g_np > 5 ? g_d3.resyncs : 0); }
/* v0.21.12: suspeitas que sumiram sozinhas (o HC estava só andando) */
unsigned xhs_resync_suspects(void) { return g_cin.susp_seen + g_din.susp_seen + (g_np > 5 ? g_d3.susp_seen : 0); }

#ifdef XHS_HOSTTEST
/* testes: a dessincronia vista no console (prod == cons, o colhedor para) */
void xhs_test_desync(int which) {
    hpipe_t *p = g_pipes[which];
    p->cons = p->prod;
}
#endif

int xhs_poll(xhs_rx_fn fn, void *u) { return xhs_poll2(fn, u, 1); }

/* v0.21.13: o poll nunca entra em si mesmo (uma espera do WebRTC dentro do
 * caminho do RX bombearia o Wi-Fi de novo: o colhedor de fora seguiria com
 * cons/prod velhos e o lwIP seria reentrado) e tem orçamento: no máximo
 * XHS_BUDGET_US de colheita por chamada; o resto fica pra próxima. */
#define XHS_BUDGET_US 2000
/* v0.21.17: a trava contra reentrada ficou "ocupada" pra sempre no console
 * (v0.21.16: depois de t=72 s todo poll era recusado; o caminho de dados
 * morreu sem erro nenhum). Pelo código, o set/clear tem uma saída só; o log
 * mostrou, no mesmo instante, o g_crumb_t (outra estática da hw1) com valor
 * absurdo: memória do .bss pisada. Então a trava agora:
 *   - mora numa linha de cache só dela, com canários antes e depois (se
 *     alguém pisar, o log diz o que estava lá);
 *   - guarda o dono (PIR) e o quadro da pilha de quem entrou: reentrada de
 *     verdade é a mesma thread com a pilha MAIS FUNDA que a do dono; fora
 *     disso a trava está velha -> limpa, conta e loga alto;
 *   - outra thread dentro do poll por > 50 ms = morta -> limpa também. */
#define PG_CANARY 0x5ca1ab1eu
typedef struct {
    uint32_t          can_a[4];
    volatile int      busy;
    volatile int      owner;
    volatile uintptr_t frame;
    volatile uint64_t since_tb;
    volatile unsigned runs;        /* corpos do poll que terminaram */
    uint32_t          can_b[4];
} __attribute__((aligned(128))) pguard_t;
static pguard_t g_pg __attribute__((aligned(128))) = {
    { PG_CANARY, PG_CANARY, PG_CANARY, PG_CANARY }, 0, -1, 0, 0, 0, { PG_CANARY, PG_CANARY, PG_CANARY, PG_CANARY } };
static uint8_t  g_pg_pad[128] __attribute__((aligned(128), used));   /* ninguém divide a linha com a trava */
static unsigned g_poll_reenter = 0, g_poll_budget_hits = 0, g_pg_stale = 0, g_pg_trampled = 0;
unsigned xhs_poll_reenters(void) { return g_poll_reenter; }
unsigned xhs_guard_stale(void) { return g_pg_stale; }
unsigned xhs_guard_trampled(void) { return g_pg_trampled; }
unsigned xhs_poll_runs(void) { return g_pg.runs; }

#ifdef XHS_HOSTTEST
extern int xhs_test_pir;
static int pg_pir(void) { return xhs_test_pir; }
/* testes: a trava esquecida como no console (dono = esta thread, quadro raso) */
void xhs_test_stick_guard(void) { g_pg.busy = 1; g_pg.owner = pg_pir(); g_pg.frame = (uintptr_t)1; g_pg.since_tb = mftb(); }
void xhs_test_trample_guard(void) { g_pg.can_a[1] = 0xdeadbeefu; g_pg.busy = 1; g_pg.owner = 7; g_pg.since_tb = mftb(); }
#else
static int pg_pir(void) {
    unsigned v;
    __asm__ __volatile__("mfspr %0, 1023" : "=r"(v));
    return (int)v;
}
#endif

static void pg_check_canaries(void) {
    int bad = 0;
    for (int i = 0; i < 4; i++) bad |= g_pg.can_a[i] != PG_CANARY || g_pg.can_b[i] != PG_CANARY;
    if (!bad) return;
    g_pg_trampled++;
    if (g_pg_trampled <= 5)
        xc_log("[wifi] HS: MEMÓRIA PISADA em volta da trava do poll (#%u): antes %08x %08x %08x %08x, depois %08x %08x %08x %08x; "
               "busy=%d dono=%d", g_pg_trampled, (unsigned)g_pg.can_a[0], (unsigned)g_pg.can_a[1], (unsigned)g_pg.can_a[2],
               (unsigned)g_pg.can_a[3], (unsigned)g_pg.can_b[0], (unsigned)g_pg.can_b[1], (unsigned)g_pg.can_b[2],
               (unsigned)g_pg.can_b[3], g_pg.busy, g_pg.owner);
    for (int i = 0; i < 4; i++) { g_pg.can_a[i] = PG_CANARY; g_pg.can_b[i] = PG_CANARY; }
}
unsigned xhs_poll_budget_hits(void) { return g_poll_budget_hits; }

static int xhs_poll2_body(xhs_rx_fn fn, void *u, int allow_relink);
int xhs_poll2(xhs_rx_fn fn, void *u, int allow_relink) {
    if (!g_ok) return 0;
    uintptr_t fr = (uintptr_t)__builtin_frame_address(0);   /* a pilha de verdade (o ASan move as locais) */
    int me = pg_pir();
    pg_check_canaries();
    if (g_pg.busy) {
        int nested = g_pg.owner == me && fr < g_pg.frame;   /* a pilha desce: mais fundo = dentro do poll */
        uint64_t age_ms = (uint64_t)tb_diff_msec(mftb(), g_pg.since_tb);
        if (nested || (g_pg.owner != me && age_ms < 50)) { g_poll_reenter++; return 0; }
        g_pg_stale++;
        if (g_pg_stale <= 5 || (g_pg_stale % 1000) == 0)
            xc_log("[wifi] HS: trava do poll VELHA (#%u): dono hw%d, quadro 0x%08x (este 0x%08x), há %u ms; limpando", g_pg_stale,
                   g_pg.owner, (unsigned)g_pg.frame, (unsigned)fr, (unsigned)age_ms);
    }
    g_pg.owner = me;
    g_pg.frame = fr;
    g_pg.since_tb = mftb();
    g_pg.busy = 1;
    int w = xhs_poll2_body(fn, u, allow_relink);
    g_pg.busy = 0;
    g_pg.owner = -1;
    g_pg.runs++;
    return w;
}

static int xhs_poll2_body(xhs_rx_fn fn, void *u, int allow_relink) {
    uint64_t t_start = mftb();
    int over = 0;
    int work = allow_relink ? relink() : 0;
    /* ativa os envios pendentes, na ordem */
    hpipe_t *outs[2] = { &g_cout, &g_dout };
    for (int k = 0; k < 2; k++) {
        hpipe_t *p = outs[k];
        while (p->state[p->sub] == S_PENDING) {
            td_arm(p, p->sub, PID_OUT, p->len[p->sub], 0, NULL);
            p->state[p->sub] = S_ACTIVE;
            p->sub = (p->sub + 1) % p->ntd;
            work = 1;
        }
    }
    /* colhe */
    for (int k = 0; k < g_np - 1; k++) {
        hpipe_t *p = g_pipes[k + 1];
        /* v0.21.15: overlay só com o cano parado (sem colher nada há 20 ms) */
        uint64_t tnow = mftb();
        int idle = (tnow - p->prog_tb) > 20ULL * (PPC_TIMEBASE_FREQ / 1000ULL) &&
                   (tnow - p->ov_tb) > 10ULL * (PPC_TIMEBASE_FREQ / 1000ULL);
        if (idle && xhs_diag_overlay) { p->ov_tb = tnow; p->ov_reads++; }
        if (xhs_diag_overlay) halt_recover(p);   /* 1 leitura do token do overlay, em todo poll (como antes) */
        int halted_seen = 0;
        for (int guard = 0; guard < p->ntd && p->state[p->cons] == S_ACTIVE && !over; guard++) {
            int i = p->cons;
            PEEK(p, &p->td[i][T_TOKEN], "token do qTD (colheita)");
            uint32_t tok = LE(p->td[i][T_TOKEN]);
            if (tok & TOK_ACTIVE) break;
            p->prog_tb = mftb();
            int got = p->len[i] - (int)((tok >> 16) & 0x7fff);
            int err = (tok & (TOK_HALT | TOK_ERRS)) != 0;
            if (tok & TOK_HALT) halted_seen = 1;
            if (err) {
                p->errs++;
                if (p->errs <= 5) xc_log("[wifi] HS: %s erro no qTD (token %08x)", p->name, (unsigned)tok);
            }
            p->done++;
            p->state[i] = S_FREE;
            p->cons = (p->cons + 1) % p->ntd;
            trace(p, 'R', i);
            work = 1;
            if (p->in) {
                if (!err && got > 0 && fn) { buf_from_hc(p->buf[i], (size_t)got); fn(p->ep, p->buf[i], (size_t)got, u); }
                /* rearma no fim do anel (o slot livre que sobrou) */
                int j = p->prod;
                td_arm(p, j, PID_IN, p->cap, 0, NULL);
                p->state[j] = S_ACTIVE;
                p->len[j] = p->cap;
                p->prod = p->sub = (p->prod + 1) % p->ntd;
                trace(p, 'A', j);
            }
            if ((mftb() - t_start) > (uint64_t)XHS_BUDGET_US * (PPC_TIMEBASE_FREQ / 1000000ULL)) {
                over = 1;
                g_poll_budget_hits++;
            }
        }
        if (p->in && idle && xhs_diag_overlay && in_resync(p, fn, u, 0) > 0) work = 1;
        if (!xhs_diag_overlay && halted_seen) {
            /* sem ler o overlay: o qTD voltou com Halt -> limpa o overlay
             * (inativo, sem Halt) apontando pro próximo armado, só escrevendo */
            int nx = p->prod;
            for (int k2 = 0; k2 < p->ntd; k2++) {
                int i2 = (p->cons + k2) % p->ntd;
                if (p->state[i2] == S_ACTIVE) { nx = i2; break; }
            }
            p->qh[Q_OV_TOKEN] = 0;
            p->qh[Q_OV_NEXT] = LE(DMA(p->td[nx]));
            sync_();
            p->halts++;
            g_unhalts++;
        }
    }
    return work;
}

void xhs_stats(char *o, size_t n) {
    snprintf(o, n, "HS rx_cmd=%u rx_dados=%u rx_ep3=%u tx_cmd=%u tx_dados=%u erros=%u/%u/%u/%u halts=%u/%u/%u/%u "
             "descartes=%u/%u religa=%u",
             g_cin.done, g_din.done, g_d3.done, g_cout.done, g_dout.done, g_cin.errs, g_cout.errs, g_din.errs,
             g_dout.errs, g_cin.halts, g_cout.halts, g_din.halts, g_dout.halts, g_cout.drops, g_dout.drops, g_relinks);
}

unsigned xhs_tx_done(int ep) { return ep == 1 ? g_cout.done : g_dout.done; }
/* v0.21.8: RX de dados concluídos (EP2 IN + EP3 IN) */
unsigned xhs_rx_count(void) { return g_din.done + g_d3.done; }

/* diagnóstico: tokens do overlay e o próximo qTD de cada cano */
void xhs_dump(char *o, size_t n) {
    size_t k = (size_t)snprintf(o, n, "HS tokens");
    for (int i = 1; i < g_np && k < n; i++) {
        hpipe_t *p = g_pipes[i];
        if (!p->qh) continue;
        k += (size_t)snprintf(o + k, n - k, " %s ov=%08x td=%08x", p->name, (unsigned)LE(p->qh[Q_OV_TOKEN]),
                              (unsigned)LE(p->td[p->cons][T_TOKEN]));
    }
}

/* ------------------------------------------------------------------
 * v0.21.8: diagnóstico e recuperação do RX (anel do EP2 IN / EP1 IN)
 * ---------------------------------------------------------------- */

static const char *st_name(int st) { return st == S_FREE ? "L" : st == S_PENDING ? "P" : "A"; }

/* uma linha por cano: índices, overlay (atual/próximo/token) e o token de cada qTD */
int xhs_dump_pipe(int which, char *o, size_t n) {
    if (!g_ok || which < 1 || which >= g_np) return 0;
    hpipe_t *p = g_pipes[which];
    if (!p->qh) return 0;
    size_t k = (size_t)snprintf(o, n, "%s prod=%d sub=%d cons=%d done=%u ov cur=%08x next=%08x tok=%08x |",
                                p->name, p->prod, p->sub, p->cons, p->done, (unsigned)LE(p->qh[Q_CUR]),
                                (unsigned)LE(p->qh[Q_OV_NEXT]), (unsigned)LE(p->qh[Q_OV_TOKEN]));
    for (int i = 0; i < p->ntd && k + 14 < n; i++)
        k += (size_t)snprintf(o + k, n - k, " %d%s:%08x", i, st_name(p->state[i]), (unsigned)LE(p->td[i][T_TOKEN]));
    return 1;
}

int xhs_npipes(void) { return g_np; }

/* ------------------------------------------------- v0.21.20: portas
 * Um controle 360 COM FIO numa porta da frente (EHCI 1, o mesmo do Wi-Fi)
 * derrubou a rede até reiniciar. O USBStorage_Init do tinyehci roda a cada
 * usb_do_poll (1 kHz, na hw1) e, pra QUALQUER porta conectada sem dono, faz
 * reset de porta com esperas de 10..600 ms (ehci_reset_port2: reset, e se
 * não habilitou, PORT_OWNER; senão GET_DESCRIPTOR/SET_ADDRESS pelo asyncqh
 * dele, que TROCA o async->hw_next e tira os nossos QHs do anel). Com o
 * Wi-Fi aberto, o EHCI 1 é nosso:
 *   - todas as outras portas do EHCI 1 ficam "busy" pro tinyehci (ele nunca
 *     mais mexe nesse controlador);
 *   - a gente trata a conexão sem bloquear (máquina de estados, só registros
 *     da PORTA, nunca USBCMD/ASYNCLISTADDR nem o anel): baixa velocidade (K
 *     no line status) -> PORT_OWNER direto; senão reset de 50 ms; não
 *     habilitou = full speed -> PORT_OWNER (o OHCI enumera, como no boot);
 *     habilitou = high speed (pendrive na frente) -> desabilita a porta e
 *     loga (não dá pra enumerar sem mexer no anel);
 *   - toda mudança de PORTSC nos dois controladores vai pro log, e depois de
 *     cada uma a saúde do anel (o async aponta pros nossos QHs?). */
#define PS_CCS 0x0001u
#define PS_CSC 0x0002u
#define PS_PE  0x0004u
#define PS_PEC 0x0008u
#define PS_OCC 0x0020u
#define PS_PR  0x0100u
#define PS_LS  0x0c00u
#define PS_PP  0x1000u
#define PS_PO  0x2000u
#define PS_RWC (PS_CSC | PS_PEC | PS_OCC)
typedef struct { uint32_t last; int st; uint64_t t; int seen; } pwatch_t;
static pwatch_t g_pw[2][8];
static unsigned g_pw_handoffs = 0, g_pw_hs_blocked = 0, g_pw_changes = 0;
static uint64_t g_pw_last_tb = 0;

static uintptr_t ps_addr(struct ehci_hcd *h, int p) { return (uintptr_t)REG(h->regs, offsetof(struct ehci_regs, port_status) + 4u * (unsigned)p); }
static uint32_t ps_rd(struct ehci_hcd *h, int p) { return read32(ps_addr(h, p)); }
static void ps_wr(struct ehci_hcd *h, int p, uint32_t v) { write32(ps_addr(h, p), v); }

static void ps_text(uint32_t v, char *o, size_t n) {
    static const char *ls[4] = { "SE0", "J", "K", "?" };
    snprintf(o, n, "%s%s%s%s%s, linha %s", (v & PS_CCS) ? "conectado" : "vazio", (v & PS_PE) ? ", habilitada" : "",
             (v & PS_PR) ? ", em reset" : "", (v & PS_PO) ? ", com o OHCI" : "", (v & PS_PP) ? "" : ", sem energia",
             ls[(v >> 10) & 3]);
}

static void ring_health(char *o, size_t n) {
    struct ehci_qh *a = g_h->async;
    memdcbf((void *)&a->hw_next, 4);
    uint32_t cur = LE(a->hw_next) & ~0x1fu;
    snprintf(o, n, "anel do EHCI %d: %s (religados %u)", g_bus, cur == (qh_link(g_ctl.qh) & ~0x1fu) ? "com os nossos QHs" : "SEM os nossos QHs",
             g_relinks);
}

/* v0.21.29: Wi-Fi desligado desde o boot (cabo): o vigia assume o hotplug do
 * EHCI do Wi-Fi do mesmo jeito (controle com fio na frente -> OHCI), sem abrir
 * os canos. Porta já habilitada (pendrive em uso) fica de fora até desconectar. */
static int g_watch_only = 0;
int xhs_watch_claim(void) {
    int b = 1, p = 0, a = 0;
    if (xhs_find(&b, &p, &a) == 0) { b = 1; p = 0; }   /* Fat/sem o adaptador: o EHCI 1 (portas da frente) */
    if (b < 0 || b >= EHCI_HCD_COUNT || !ehci_hcds[b].regs) return -1;
    g_h = &ehci_hcds[b];
    g_bus = b;
    g_port = p;   /* 0 = nenhuma porta reservada */
    int np = g_h->num_port;
    if (np > EHCI_MAX_ROOT_PORTS) np = EHCI_MAX_ROOT_PORTS;
    if (np > 8) np = 8;
    for (int k = 0; k < np; k++) {
        uint32_t v = ps_rd(g_h, k);
        if (v & PS_PE) g_pw[b][k].st = 3;   /* em uso pelo tinyehci (pendrive): não mexe */
        g_h->devices[k].busy = 1;
    }
    g_watch_only = 1;
    xc_log("[usb] Wi-Fi desligado: hotplug do EHCI %d com o vigia (porta reservada %d)", b, p);
    return 0;
}

/* v0.21.43: o usuário ligou o Wi-Fi pelo menu: o vigia devolve o EHCI (o
 * tinyehci e a procura do adaptador voltam a mexer nas portas) */
void xhs_watch_release(void) {
    if (!g_watch_only || !g_h) return;
    int np = g_h->num_port;
    if (np > EHCI_MAX_ROOT_PORTS) np = EHCI_MAX_ROOT_PORTS;
    if (np > 8) np = 8;
    for (int k = 0; k < np; k++) {
        if (k == g_port - 1 || !(ps_rd(g_h, k) & PS_PE)) g_h->devices[k].busy = 0;   /* pendrive em uso fica como está */
        g_pw[g_bus][k].st = 0;
    }
    g_watch_only = 0;
    xc_log("[usb] Wi-Fi ligado pelo menu: o vigia devolveu o EHCI %d", g_bus);
}

unsigned xhs_port_handoffs(void) { return g_pw_handoffs; }
unsigned xhs_port_hs_blocked(void) { return g_pw_hs_blocked; }
unsigned xhs_port_changes(void) { return g_pw_changes; }

int xhs_port_watch(void) {
    uint64_t tb = mftb();
    if (g_pw_last_tb && tb - g_pw_last_tb < 10ULL * (PPC_TIMEBASE_FREQ / 1000ULL)) return 0;   /* a cada 10 ms */
    g_pw_last_tb = tb;
    int acted = 0;
    for (int b = 0; b < EHCI_HCD_COUNT && b < 2; b++) {
        struct ehci_hcd *h = &ehci_hcds[b];
        if (!h->regs) continue;
        int np = h->num_port;
        if (np > EHCI_MAX_ROOT_PORTS) np = EHCI_MAX_ROOT_PORTS;
        if (np > 8) np = 8;
        for (int p = 0; p < np; p++) {
            pwatch_t *w = &g_pw[b][p];
            uint32_t v = ps_rd(h, p);
            if (!w->seen || v != w->last) {
                if (w->seen) {
                    char a[120], c[120], r[100];
                    ps_text(w->last, a, sizeof(a));
                    ps_text(v, c, sizeof(c));
                    g_pw_changes++;
                    if (g_ok && b == g_bus) ring_health(r, sizeof(r));
                    else r[0] = 0;
                    xc_log("[usb] EHCI %d porta %d: %08x -> %08x (%s -> %s)%s%s", b, p + 1, (unsigned)w->last, (unsigned)v, a, c,
                           r[0] ? "; " : "", r);
                }
                w->last = v;
                w->seen = 1;
            }
            /* só o EHCI do Wi-Fi, com os canos abertos, e nunca a porta do Wi-Fi */
            if (!(g_ok || g_watch_only) || b != g_bus || p == g_port - 1) continue;
            h->devices[p].busy = 1;   /* o tinyehci fica fora deste controlador */
            uint64_t ms = (uint64_t)tb_diff_msec(tb, w->t);
            switch (w->st) {
            case 0:
                if (v & PS_CSC) ps_wr(h, p, (v & ~PS_RWC) | PS_CSC);   /* limpa a mudança (RWC) */
                if ((v & PS_CCS) && !(v & PS_PO)) {
                    if ((v & PS_LS) == 0x0400u) {   /* K: baixa velocidade -> companheiro direto */
                        ps_wr(h, p, (v & ~PS_RWC) | PS_PO);
                        g_pw_handoffs++;
                        xc_log("[usb] EHCI %d porta %d: dispositivo de baixa velocidade -> OHCI (sem reset)", b, p + 1);
                        w->st = 3;
                    } else {
                        ps_wr(h, p, ((v & ~PS_RWC) & ~PS_PE) | PS_PR);
                        w->st = 1;
                        w->t = tb;
                    }
                    acted = 1;
                }
                break;
            case 1:   /* reset de 50 ms (EHCI 4.2.2) */
                if (ms >= 50) {
                    ps_wr(h, p, (v & ~PS_RWC) & ~PS_PR);
                    w->st = 2;
                    w->t = tb;
                    acted = 1;
                }
                break;
            case 2:   /* o HC termina o reset; PE diz se é high speed */
                if ((v & PS_PR) && ms < 20) break;
                if (v & PS_PE) {
                    ps_wr(h, p, (v & ~PS_RWC) & ~PS_PE);   /* desabilita: sem enumerar no nosso anel */
                    g_pw_hs_blocked++;
                    xc_log("[usb] EHCI %d porta %d: dispositivo HIGH SPEED com o Wi-Fi no mesmo controlador: porta desabilitada "
                           "(use uma porta de trás pro pendrive)", b, p + 1);
                } else if (v & PS_CCS) {
                    ps_wr(h, p, (v & ~PS_RWC) | PS_PO);
                    g_pw_handoffs++;
                    xc_log("[usb] EHCI %d porta %d: full speed -> OHCI (como no boot)", b, p + 1);
                }
                w->st = 3;
                acted = 1;
                break;
            default:   /* tratado: espera desconectar (ou voltar do OHCI sem dono) */
                if (!(v & PS_CCS) && !(v & PS_PR)) {
                    if (v & PS_CSC) ps_wr(h, p, (v & ~PS_RWC) | PS_CSC);
                    w->st = 0;
                }
                break;
            }
        }
    }
    return acted;
}

/* v0.21.9: zera um anel de OUT (reinício do firmware): nada pendente/ativo,
 * overlay inativo no qTD 0, toggle preservado. Devolve quantos estavam ativos. */
int xhs_out_reset(int which) {
    if (!g_ok || which < 1 || which >= g_np) return -1;
    hpipe_t *p = g_pipes[which];
    if (p->in) return -1;
    int act = 0;
    uint32_t ov = LE(p->qh[Q_OV_TOKEN]);
    for (int i = 0; i < p->ntd; i++) {
        if (LE(p->td[i][T_TOKEN]) & TOK_ACTIVE) act++;
        p->td[i][T_TOKEN] = 0;
        p->state[i] = S_FREE;
        p->zlp[i] = 0;
    }
    sync_();
    p->qh[Q_OV_TOKEN] = LE(ov & TOK_TOGGLE);
    p->qh[Q_OV_NEXT] = LE(DMA(p->td[0]));
    p->qh[Q_OV_ALT] = LE(1);
    sync_();
    p->prod = p->sub = p->cons = 0;
    p->rearms++;
    return act;
}

/* v0.21.9: só leitura, de QUALQUER thread (o vigia do main, com a hw1
 * parada): quantas transferências o HC já concluiu e ninguém colheu, por cano
 * de entrada. Diz se o adaptador continuou mandando durante a parada. */
void xhs_peek(char *o, size_t n) {
    size_t w = 0;
    o[0] = 0;
    if (!g_ok) { snprintf(o, n, "HS fechado"); return; }
    for (int k = 1; k < g_np && w < n; k++) {
        hpipe_t *p = g_pipes[k];
        if (!p->in) continue;
        int ready = 0, act = 0;
        for (int i = 0; i < p->ntd; i++) {
            if (p->state[i] != S_ACTIVE) continue;
            if (LE(p->td[i][T_TOKEN]) & TOK_ACTIVE) act++;
            else ready++;
        }
        int r = snprintf(o + w, n - w, "%s%s: %d prontos sem colher, %d esperando, ov %08x", w ? "; " : "", p->name, ready,
                         act, (unsigned)LE(p->qh[Q_OV_TOKEN]));
        if (r < 0) break;
        w += (size_t)r;
    }
}

/* Reconstrói um anel de IN do zero (todos os qTDs desarmados, overlay
 * apontando pro qTD 0, toggle preservado) e rearma n-1. Usado pelo detector
 * de RX parado do glue; devolve quantos qTDs estavam ativos (presos). */
int xhs_rx_rearm(int which) {
    if (!g_ok || which < 1 || which >= g_np) return -1;
    hpipe_t *p = g_pipes[which];
    if (!p->in) return -1;
    int stuck = 0;
    uint32_t ov = LE(p->qh[Q_OV_TOKEN]);
    for (int i = 0; i < p->ntd; i++) {
        if (LE(p->td[i][T_TOKEN]) & TOK_ACTIVE) stuck++;
        p->td[i][T_TOKEN] = 0;
        p->state[i] = S_FREE;
    }
    sync_();
    p->qh[Q_OV_TOKEN] = LE(ov & TOK_TOGGLE);   /* inativo, sem Halt, mesmo toggle */
    p->qh[Q_OV_NEXT] = LE(DMA(p->td[0]));
    p->qh[Q_OV_ALT] = LE(1);
    sync_();
    for (int i = 0; i < p->ntd - 1; i++) {
        td_arm(p, i, PID_IN, p->cap, 0, NULL);
        p->state[i] = S_ACTIVE;
        p->len[i] = p->cap;
    }
    p->cons = 0;
    p->prod = p->sub = p->ntd - 1;
    p->rearms++;
    trace(p, 'X', 0);
    return stuck;
}

/* conferência de consistência (testes): IN = exatamente n-1 ativos e 1 livre
 * logo depois do último; OUT sem nada ativo quando ocioso. 0 = ok. */
int xhs_check(char *why, size_t n) {
    for (int w = 1; w < g_np; w++) {
        hpipe_t *p = g_pipes[w];
        if (!p->qh) continue;
        int act = 0, fr = 0;
        for (int i = 0; i < p->ntd; i++) {
            if (p->state[i] == S_ACTIVE) act++;
            if (p->state[i] == S_FREE) fr++;
        }
        if (p->in && (act != p->ntd - 1 || fr != 1 || p->state[p->prod] != S_FREE)) {
            snprintf(why, n, "%s: %d ativos, %d livres, prod=%d (%s)", p->name, act, fr, p->prod, st_name(p->state[p->prod]));
            return -1;
        }
    }
    return 0;
}
