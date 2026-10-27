/*
 * XCloud360 — rede do Xbox (thread de rede) + ring de vídeo. Ver tcp_stream.h.
 *
 * Aurora-safe: NUNCA chama network_init() se o dashboard já subiu a rede
 * (netif.ip_addr.addr != 0) — double-init = crash 0x700.
 *
 * v0.14 — thread de rede dedicada (stream_start_thread): uma thread de
 * hardware só faz network_poll() em loop. Regras:
 *   - lwIP só é tocado pela thread de rede depois do start;
 *   - ring SPSC: thread de rede escreve head, main escreve tail, com lwsync.
 * v0.20.0: o modo PC saiu (cliente TCP :9000 do capture-rtp, conexão de
 * controle :9002, controle/KFRQ por UDP :9001). Sobrou a thread de rede
 * (canos HTTPS/DNS do login, WebRTC) e o ring, alimentado só pelo WebRTC
 * (stream_inject_au). Cada AU é publicado de uma vez (cabeçalho + dados,
 * um único avanço do head): o stream_discard do main nunca deixa o ring
 * desalinhado no meio de um AU.
 */

#include "tcp_stream.h"
#include "xc_wdog.h"               /* v0.21.7: vigia */
#include "login/net_pipe.h"        /* v0.17: canos HTTPS/DNS do login */
#include "wifi/xw_xenon.h"           /* v0.21.29: cabo assume a rota */
#include "webrtc/xc_udp_xenon.h"   /* stage 5: UDP do WebRTC nesta thread */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <network/network.h>
#include <lwip/init.h>
#include <lwip/dhcp.h>
#include <lwip/ip.h>
#include <netif/etharp.h>

/* v0.9.5: 512KB = ~0.5-0.7s max de fila (latência de input). */
#define RING_CAPACITY         (512 * 1024)
/* v0.14.3: 1ms entre polls (a thread de rede é irmã SMT de outras). */
#define NET_POLL_PAUSE_US     1000

typedef struct {
    uint8_t *buf;
    size_t   cap;
    volatile size_t head;   /* só o produtor (rede) escreve */
    volatile size_t tail;   /* só o consumidor (main) escreve */
} ring_t;

static ring_t       g_ring;
static volatile int g_threaded = 0;

/* stage 5: the WebRTC stack runs inside net_step (stack high-water 12.6 KB
 * on ppc32, plus the DTLS/TLS paths): WEBRTC-STAGE4.md asks for 128 KB. */
/* v0.21.13: 256 KB (era 128): WebRTC (DTLS/SCTP/usrsctp) + a bomba do
 * xcw_idle + o passo do Wi-Fi + lwIP na mesma pilha. Pintada antes de subir:
 * o pico de uso e o canário do fundo vão pra linha da sessão e pro vigia. */
#define NET_STACK_BYTES (256 * 1024)
#define NET_STACK_PAINT 0x5Au
#define NET_STACK_CANARY 1024
static uint8_t g_net_stack[NET_STACK_BYTES] __attribute__((aligned(128)));
unsigned stream_net_stack_size(void) { return NET_STACK_BYTES; }
unsigned stream_net_stack_peak(void) {
    unsigned i = 0;
    while (i < NET_STACK_BYTES && g_net_stack[i] == NET_STACK_PAINT) i++;
    return NET_STACK_BYTES - i;
}
int stream_net_stack_canary_ok(void) {
    for (unsigned i = 0; i < NET_STACK_CANARY; i++)
        if (g_net_stack[i] != NET_STACK_PAINT) return 0;
    return 1;
}
static void (*volatile g_net_extra)(void) = NULL;
static int g_in_extra = 0;
static volatile uint32_t g_inject_full = 0;
/* stage 5: hw1 load = time in net_step + in hook calls that did work. */
static volatile uint64_t g_busy_tb = 0;
static uint64_t g_load_t0_tb = 0;

static inline void lwsync(void) { __asm__ __volatile__("lwsync" ::: "memory"); }

static int ring_init(ring_t *r, size_t cap) {
    r->buf = (uint8_t *)malloc(cap);
    if (!r->buf) return -1;
    r->cap = cap; r->head = r->tail = 0;
    return 0;
}
static inline size_t ring_used(const ring_t *r) {
    size_t h = r->head, t = r->tail;
    return (h >= t) ? (h - t) : (r->cap - (t - h));
}
static inline size_t ring_free_(const ring_t *r) {
    return r->cap - ring_used(r) - 1;
}
/* Copia em h (sem publicar); devolve a nova posição. */
static size_t ring_put(ring_t *r, size_t h, const uint8_t *src, size_t n) {
    size_t first = (h + n <= r->cap) ? n : (r->cap - h);
    memcpy(r->buf + h, src, first);
    if (n > first) memcpy(r->buf, src + first, n - first);
    return (h + n) % r->cap;
}
static size_t ring_read_(ring_t *r, uint8_t *dst, size_t n) {
    size_t used = ring_used(r);
    if (n > used) n = used;
    if (!n) return 0;
    lwsync();                       /* lê head antes dos dados */
    size_t t = r->tail;
    size_t first = (t + n <= r->cap) ? n : (r->cap - t);
    memcpy(dst, r->buf + t, first);
    if (n > first) memcpy(dst + first, r->buf, n - first);
    lwsync();                       /* terminou de ler antes de liberar */
    r->tail = (t + n) % r->cap;
    return n;
}

/* v0.21.0: 1 = o cabo pegou DHCP no boot (ou o dashboard já tinha subido a
 * rede): o Wi-Fi interno não liga sozinho (WIFI.md).
 * v0.21.5: também vira 1 se o DHCP do cabo fechar depois (cabo plugado com o
 * app aberto, ou "Tentar de novo" na tela "Sem internet"). */
static volatile int g_cable = 0;
static volatile int g_cable_retry = 0;
int stream_net_cable(void) { return g_cable; }
int stream_net_link(void) { return network_link_up() ? 1 : 0; }
void stream_cable_retry(void) { g_cable_retry = 1; }

extern err_t enet_init(struct netif *nf);

/* v0.21.5: o network_init do libxenon com a espera do DHCP curta: sem link
 * no cabo, nenhuma (antes 2,5 s); com link, até 10 s (antes 15 s; o DHCP
 * normal responde em < 2 s). O enet_init (reset do PHY + até 5 s esperando o
 * link) continua igual: depois do reset do PHY o link de quem tem cabo leva
 * uns segundos pra voltar. Sem DHCP fica o IP fixo de sempre (192.168.1.99) e
 * o DHCP do cabo segue tentando em segundo plano. */
static int net_init_fast(void) {
    ip_addr_t ip, mask, gw;
    IP4_ADDR(&mask, 255, 255, 255, 255);
    IP4_ADDR(&gw, 0, 0, 0, 0);
    IP4_ADDR(&ip, 0, 0, 0, 0);
    printf(" * initializing lwip 1.4.1 (orbita: espera curta sem cabo)...\n");
    lwip_init();
    uint64_t t0 = mftb();
    if (!netif_add(&netif, &ip, &mask, &gw, NULL, enet_init, ip_input)) {
        printf(" ! netif_add failed!\n");
        return NETWORK_INIT_FAILURE;
    }
    netif_set_default(&netif);
    dhcp_start(&netif);
#ifdef ORBITA_TEST_PHYWAIT
    /* build de teste: espera o link do cabo como nos consoles que sobem o PHY na hora */
    while (!network_link_up() && tb_diff_msec(mftb(), t0) < ORBITA_TEST_PHYWAIT) network_poll();
#endif
    int link = network_link_up() ? 1 : 0;
    unsigned link_ms = (unsigned)tb_diff_msec(mftb(), t0);
    /* v0.21.29: o PHY do Corona diz "sem link" mesmo com cabo (o DHCP fecha
     * ~2 s depois). Sem link: espera 3 s pelo DHCP mesmo assim, pra o cabo
     * valer já no boot e o Wi-Fi nem ligar. */
    unsigned wait_ms = link ? 10000 : 3000;
    uint64_t t1 = mftb();
    while (netif.ip_addr.addr == 0 && tb_diff_msec(mftb(), t1) < wait_ms) network_poll();
    unsigned dhcp_ms = (unsigned)tb_diff_msec(mftb(), t1);
    char cl[120];
    snprintf(cl, sizeof(cl), "[net] cabo: link %s (PHY %u ms), DHCP %s em %u ms%s", link ? "sim" : "não", link_ms,
             netif.ip_addr.addr ? "ok" : "sem resposta", dhcp_ms, link ? "" : " (PHY sem link: esperou 3 s)");
    printf("%s\n", cl);
    np_log_line(cl);
    if (netif.ip_addr.addr) return NETWORK_INIT_SUCCESS;
    IP4_ADDR(&ip, 192, 168, 1, 99);
    IP4_ADDR(&gw, 192, 168, 1, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    netif_set_addr(&netif, &ip, &mask, &gw);
    netif_set_up(&netif);
    return NETWORK_INIT_DHCP_FAILURE;
}

/* thread de rede: DHCP do cabo que fechou depois do boot / "Tentar de novo" */
static void cable_watch(void) {
    static uint64_t last = 0;
    uint64_t now = mftb();
    if (g_cable_retry) {
        g_cable_retry = 0;
        printf("[net] cabo: tentando DHCP de novo (link %s)\n", network_link_up() ? "sim" : "não");
        np_log_line(network_link_up() ? "[net] cabo: tentando DHCP de novo (link sim)"
                                      : "[net] cabo: tentando DHCP de novo (link não)");
        dhcp_start(&netif);
    }
    if (g_cable || tb_diff_msec(now, last) < 500) return;
    last = now;
    if (netif.dhcp && netif.dhcp->state == DHCP_BOUND && netif.ip_addr.addr) {
        /* v0.21.29: o cabo é a "caixinha" principal: com DHCP ele vira a rota
         * e o Wi-Fi sai da frente. No meio do jogo não troca (as conexões
         * cairiam); troca quando a sessão fechar. */
        if (netif_default != &netif) {
            if (xw_sys_session()) return;
            netif_set_default(&netif);
            np_log_line("[net] cabo pegou DHCP: agora é a rota padrão (Wi-Fi sai da frente)");
        }
        xw_sys_cable_up();
        g_cable = 1;
        char l[96];
        snprintf(l, sizeof(l), "[net] cabo pegou DHCP depois do boot: %u.%u.%u.%u", ip4_addr1(&netif.ip_addr),
                 ip4_addr2(&netif.ip_addr), ip4_addr3(&netif.ip_addr), ip4_addr4(&netif.ip_addr));
        np_log_line(l);
        np_dns_reload(((uint32_t)ip4_addr1(&netif.gw) << 24) | ((uint32_t)ip4_addr2(&netif.gw) << 16) |
                          ((uint32_t)ip4_addr3(&netif.gw) << 8) | ip4_addr4(&netif.gw));
    }
}

int stream_init(void) {
    /* CRITICO Aurora-safe: NAO re-init lwip se ja tem netif com IP. */
    if (netif.ip_addr.addr == 0) {
        printf("[stream] no IP, calling network_init...\n");
        int r = net_init_fast();   /* v0.21.5: no lugar do network_init */
        if (r == NETWORK_INIT_FAILURE) {
            printf("[stream] network_init failed\n");
            return -1;
        }
        g_cable = r == NETWORK_INIT_SUCCESS;
    } else {
        g_cable = 1;
        printf("[stream] netif already up (Aurora), skip init. IP=0x%08x\n",
               (unsigned)netif.ip_addr.addr);
    }
    network_print_config();
    return ring_init(&g_ring, RING_CAPACITY);
}

/* Um passo de rede: NIC + timers lwIP + canos do login + WebRTC. */
static void net_step(void) {
    network_poll();
    cable_watch();   /* v0.21.5 */
    np_step();
    /* UDP do WebRTC (abre/fecha socket, envios de outra thread) e depois o
     * WebRTC em si. xcw_idle() (espera dentro do xcw_create_offer) bombeia
     * este mesmo net_step: o g_in_extra evita reentrar no WebRTC. */
    xcw_net_step();
    if (g_net_extra && !g_in_extra) {
        g_in_extra = 1;
        g_net_extra();
        g_in_extra = 0;
    }
}

/* v0.21.10: espera do WebRTC na hw1 (xcw_idle) bombeava só o net_step: o
 * Ethernet entra no network_poll, mas o Wi-Fi (RX, envio dos qTDs, DHCP)
 * só no hook. Sem cabo nada chegava durante essas esperas. */
static void (*volatile g_pump_extra)(void) = NULL;
static int g_in_pump_extra = 0;
void stream_set_pump_extra(void (*fn)(void)) { g_pump_extra = fn; }
void stream_net_pump(void) {
    net_step();
    void (*fn)(void) = g_pump_extra;
    if (fn && !g_in_pump_extra) {
        g_in_pump_extra = 1;
        fn();
        g_in_pump_extra = 0;
    }
}
void stream_set_net_extra(void (*fn)(void)) { g_net_extra = fn; }
uint32_t stream_inject_full(void) { return g_inject_full; }

/* Só a thread de rede (produtor do ring). [u32 BE n][AU], inteiro ou nada,
 * com um único avanço do head (v0.20.0). */
int stream_inject_au(const uint8_t *au, size_t n) {
    if (!g_ring.buf) return -1;
    if (n == 0 || n > STREAM_INJECT_MAX) return -1;
    if (ring_free_(&g_ring) < n + 4) { g_inject_full++; return -1; }
    uint8_t hd[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    size_t h = ring_put(&g_ring, g_ring.head, hd, 4);
    h = ring_put(&g_ring, h, au, n);
    lwsync();                       /* dados visíveis antes do head novo */
    g_ring.head = h;
    return 0;
}

void stream_net_load(uint64_t *busy_us, uint64_t *wall_us) {
    static uint64_t prev_busy, prev_wall;
    uint64_t now = mftb();
    if (!g_load_t0_tb) g_load_t0_tb = now;
    uint64_t busy = g_busy_tb, wall = now - g_load_t0_tb;
    *busy_us = (busy - prev_busy) / (PPC_TIMEBASE_FREQ / 1000000ULL);
    *wall_us = (wall - prev_wall) / (PPC_TIMEBASE_FREQ / 1000000ULL);
    prev_busy = busy;
    prev_wall = wall;
}

static int (*volatile g_idle_hook)(void) = NULL;

void stream_set_idle_hook(int (*fn)(void)) { g_idle_hook = fn; }

/* v0.14.4: entre polls a thread roda o hook (USB, áudio, metade de baixo do
 * convert no caminho de CPU). O hook é checado durante a pausa toda. */
int stream_diag_prio = 0;   /* v0.21.16 */
/* v0.21.25: SMT baixa na pausa entre polls (DIAG.TXT netprio=med desliga):
 * o main (irmão no núcleo 0) apresenta com mais folga */
volatile int stream_net_lowprio = 0;
uint64_t stream_net_busy_tb(void) { return g_busy_tb; }
static void net_thread_main(void) {
    g_load_t0_tb = mftb();
    uint64_t wd_last = 0;
    for (;;) {
        uint64_t s0 = mftb();
        /* v0.21.16: DIAG.TXT prio=1 -> prioridade SMT média explícita a cada volta */
        if (stream_diag_prio) __asm__ __volatile__("or 2,2,2");
        /* v0.21.7: batimento da hw1 e, a cada 0,5 s, a conferência do main e da hw 3 */
        xwd_beat("rede: laço (lwIP)");
        if (tb_diff_msec(s0, wd_last) >= 500) {
            wd_last = s0;
            xwd_check(XWD_NET, (1u << XWD_MAIN) | (1u << XWD_HW3));
        }
        net_step();
        uint64_t t0 = mftb();
        g_busy_tb += t0 - s0;
        int lowp = stream_net_lowprio;
        if (lowp) __asm__ __volatile__("or 1,1,1");
        while (tb_diff_usec(mftb(), t0) < NET_POLL_PAUSE_US) {
            int (*hook)(void) = g_idle_hook;
            if (hook) {
                if (lowp) __asm__ __volatile__("or 2,2,2");
                uint64_t h0 = mftb();
                int w = hook();
                uint64_t h1 = mftb();
                if (lowp) __asm__ __volatile__("or 1,1,1");
                /* chamadas ociosas (só conferem relógios) custam ~ns; conta
                 * as que passaram de 2 us (USB, áudio, metade de baixo) */
                if (h1 - h0 > PPC_TIMEBASE_FREQ / 500000ULL) g_busy_tb += h1 - h0;
                if (w) break;   /* trabalhou ~ms: volta pro poll */
            }
            __asm__ __volatile__("db16cyc" ::: "memory");
        }
        if (lowp) __asm__ __volatile__("or 2,2,2");
    }
}

int stream_start_thread(int hw_thread) {
    memset(g_net_stack, NET_STACK_PAINT, sizeof(g_net_stack) - 0x400);
    g_threaded = 1;
    lwsync();
    int r = xenon_run_thread_task(hw_thread,
                                  g_net_stack + sizeof(g_net_stack) - 0x100,
                                  (void *)net_thread_main);
    if (r != 0) g_threaded = 0;
    return r;
}

/* Consumidor joga fora o que já chegou. Mesmo lado do tail. */
void stream_discard(void) {
    lwsync();
    g_ring.tail = g_ring.head;
}

int stream_net_up(void) { return g_ring.buf != NULL; }

size_t stream_read(uint8_t *dst, size_t n) { return ring_read_(&g_ring, dst, n); }
size_t stream_available(void) { return ring_used(&g_ring); }
