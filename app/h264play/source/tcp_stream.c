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
#include "login/net_pipe.h"        /* v0.17: canos HTTPS/DNS do login */
#include "webrtc/xc_udp_xenon.h"   /* stage 5: UDP do WebRTC nesta thread */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <network/network.h>

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
static uint8_t g_net_stack[128 * 1024] __attribute__((aligned(128)));
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

int stream_init(void) {
    /* CRITICO Aurora-safe: NAO re-init lwip se ja tem netif com IP. */
    if (netif.ip_addr.addr == 0) {
        printf("[stream] no IP, calling network_init...\n");
        int r = network_init();
        if (r == NETWORK_INIT_FAILURE) {
            printf("[stream] network_init failed\n");
            return -1;
        }
    } else {
        printf("[stream] netif already up (Aurora), skip init. IP=0x%08x\n",
               (unsigned)netif.ip_addr.addr);
    }
    network_print_config();
    return ring_init(&g_ring, RING_CAPACITY);
}

/* Um passo de rede: NIC + timers lwIP + canos do login + WebRTC. */
static void net_step(void) {
    network_poll();
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

void stream_net_pump(void) { net_step(); }
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
static void net_thread_main(void) {
    g_load_t0_tb = mftb();
    for (;;) {
        uint64_t s0 = mftb();
        net_step();
        uint64_t t0 = mftb();
        g_busy_tb += t0 - s0;
        while (tb_diff_usec(mftb(), t0) < NET_POLL_PAUSE_US) {
            int (*hook)(void) = g_idle_hook;
            if (hook) {
                uint64_t h0 = mftb();
                int w = hook();
                uint64_t h1 = mftb();
                /* chamadas ociosas (só conferem relógios) custam ~ns; conta
                 * as que passaram de 2 us (USB, áudio, metade de baixo) */
                if (h1 - h0 > PPC_TIMEBASE_FREQ / 500000ULL) g_busy_tb += h1 - h0;
                if (w) break;   /* trabalhou ~ms: volta pro poll */
            }
            __asm__ __volatile__("db16cyc" ::: "memory");
        }
    }
}

int stream_start_thread(int hw_thread) {
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
