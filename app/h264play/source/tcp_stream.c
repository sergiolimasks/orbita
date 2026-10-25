/*
 * XCloud360 — TCP live H.264 stream client Aurora-safe.
 * KEY DIFF from earlier failed attempt:
 *   - NUNCA chama network_init() se Aurora ja subiu (netif.ip_addr.addr != 0).
 *   - Reusa netif existente pra tcp_new/tcp_connect direto.
 *   - Aurora dashboard tem lwip proprio (Freestyle/DashLaunch); double-init = crash 0x700.
 *
 * v0.14 — thread de rede dedicada (stream_start_thread):
 *   Antes network_poll() só rodava uma vez por volta do loop principal
 *   (decode + memcpy de 1.4MB + ...). No combate o buffer RX da NIC enchia
 *   entre polls, pacotes caíam, o TCP retransmitia (medido no devbox: RTT
 *   127ms com mínimo 0.19ms, 570 retransmissões, cwnd colapsado → 1.37Mbps
 *   < 2.3Mbps do vídeo) e a fila do capture-rtp congestionava ~1x/s.
 *   Agora uma thread de hardware só faz network_poll() em loop. Regras:
 *   - lwIP só é tocado pela thread de rede depois do start (o main pede
 *     reconexão via flag; o UDP do controle vai por mailbox, ver input_udp.c);
 *   - ring SPSC: thread de rede escreve head, main escreve tail, com lwsync;
 *   - reconexão incrementa g_gen; só o consumidor descarta o ring (tail=head)
 *     e confirma em g_gen_ack. Até lá on_recv recusa dados (ERR_MEM), então
 *     bytes da conexão nova nunca se misturam com restos da velha.
 */

#include "tcp_stream.h"
#include "input_udp.h"
#ifdef XCLOUD360_MENU
#include "ctrl_conn.h"   /* v0.16: 2o pcb (controle :9002) no mesmo net_step */
#endif
#ifdef XCLOUD360_LOGIN
#include "login/net_pipe.h"   /* v0.17: canos HTTPS/DNS/LOG do login */
#endif
#ifdef XCLOUD360_NATIVE
#include "webrtc/xc_udp_xenon.h"   /* stage 5: UDP do WebRTC nesta thread */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <network/network.h>
#include <lwip/tcp.h>
#include <lwip/ip_addr.h>
#include <lwip/err.h>
#include <lwip/pbuf.h>

/* v0.9.5: ring reduzido pra 512KB. 2MB bufferizava ~2-3s de vídeo (a ~1.5Mbps
 * do xCloud), quer dizer que o frame que responde ao input do gamepad demora
 * 2-3s pra sair. 512KB = ~0.5-0.7s max de fila, latência de input melhora
 * proporcional. Se decode não drena, drop-on-pressure em h264play cuida. */
#define RING_CAPACITY         (512 * 1024)
#define RECONNECT_DELAY_MS    500
/* v0.14.3: 100 -> 1000us. A thread de rede é irmã SMT do worker de convert
 * (núcleo 1) e cada network_poll lê MMIO da NIC, que trava o núcleo: com
 * 100us o convert foi de ~14ms pra ~64ms/frame. 1ms ainda sobra (32KB de
 * janela TCP por ms ≈ 30MB/s). */
#define NET_POLL_PAUSE_US     1000

typedef struct {
    uint8_t *buf;
    size_t   cap;
    volatile size_t head;   /* só o produtor (rede) escreve */
    volatile size_t tail;   /* só o consumidor (main) escreve */
    size_t   dropped;
} ring_t;

typedef enum {
    NET_IDLE = 0, NET_CONNECTING, NET_CONNECTED, NET_ERROR, NET_CLOSED
} net_state_t;

static ring_t          g_ring;
static struct tcp_pcb *g_pcb = NULL;
static volatile net_state_t g_state = NET_IDLE;
static uint64_t        g_last_reconnect_tb = 0;
static char            g_server_ip[32] = "0.0.0.0";
static int             g_server_port = 0;

static volatile unsigned g_gen = 0;        /* produtor: ++ a cada reconexão */
static volatile unsigned g_gen_ack = 0;    /* consumidor: ring já descartado */
static volatile int      g_req_reconnect = 0;
static volatile int      g_threaded = 0;
/* v0.16: reconexões feitas (do_reconnect_now) — o menu espera o pedido dele
 * ser atendido antes de aceitar bytes do ring como "vídeo do jogo novo". */
static volatile unsigned g_reconnects_done = 0;
static uint64_t          g_connect_tb = 0;    /* início do SYN atual */
static volatile int      g_quiet = 0;

#ifdef XCLOUD360_NATIVE
/* stage 5: the WebRTC stack runs inside net_step (stack high-water 12.6 KB
 * on ppc32, plus the DTLS/TLS paths): WEBRTC-STAGE4.md asks for 128 KB. */
static uint8_t g_net_stack[128 * 1024] __attribute__((aligned(128)));
/* Inject mode: the ring is fed by stream_inject_au() (WebRTC on this same
 * thread) instead of the TCP :9000 client, which is closed and not retried.
 * main requests, the network thread applies (it owns lwIP and the head). */
static volatile int g_inject_req = 0;
static volatile int g_inject = 0;
static void (*volatile g_net_extra)(void) = NULL;
static int g_in_extra = 0;
static volatile uint32_t g_inject_full = 0;
#else
static uint8_t g_net_stack[64 * 1024] __attribute__((aligned(128)));
#endif
/* stage 5: hw1 load = time in net_step + in hook calls that did work. */
static volatile uint64_t g_busy_tb = 0;
static uint64_t g_load_t0_tb = 0;

static inline void lwsync(void) { __asm__ __volatile__("lwsync" ::: "memory"); }

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
    lwsync();                       /* dados visíveis antes do head novo */
    r->head = (h + n) % r->cap;
    return n;
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

/* Produtor: invalida o conteúdo do ring (o consumidor descarta). */
static void bump_gen(void) {
    lwsync();
    g_gen++;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (p == NULL) {
        printf("[stream] server closed\n");
        g_state = NET_CLOSED;
        tcp_close(pcb);
        g_pcb = NULL;
        bump_gen();
        return ERR_OK;
    }
    if (err != ERR_OK) { if (p) pbuf_free(p); return err; }

    /* Consumidor ainda não descartou o ring da conexão anterior. lwip guarda
     * o pbuf (refused_data) e tenta de novo depois. */
    if (g_gen != g_gen_ack) return ERR_MEM;

    /* v0.11.2 FIX: ring atomic — só aceita se pbuf inteiro couber.
     * Bug anterior: on_recv chamava ring_write que dropa bytes se cheio, e
     * pbuf_free() jogava fora TUDO. TCP layer não retransmite bytes já
     * entregues à app → bytestream ficava com buracos no meio de AUs →
     * decoder decodava P-frames contra referência corrompida → sopa cíclica.
     * Correção: retornar ERR_MEM se não cabe. lwip re-enfileira o pbuf
     * e chama on_recv de novo quando window aumentar. TCP mantém integridade. */
    if (ring_free_(&g_ring) < p->tot_len) {
        return ERR_MEM;
    }
    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        ring_write(&g_ring, (const uint8_t *)q->payload, q->len);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

/* Consumidor: se a rede reconectou, descarta o que sobrou da conexão velha.
 * Retorna 1 quando descartou (o chamador zera o parsing de AU pela metade). */
int stream_take_reset(void) {
    unsigned gen = g_gen;
    if (gen == g_gen_ack) return 0;
    lwsync();
    g_ring.tail = g_ring.head;
    lwsync();
    g_gen_ack = gen;
    return 1;
}

void stream_reset(void) {
    bump_gen();
    stream_take_reset();
}

static void do_reconnect_now(void) {
    if (g_pcb) {
        tcp_arg(g_pcb, NULL);
        tcp_err(g_pcb, NULL);
        tcp_recv(g_pcb, NULL);
        tcp_abort(g_pcb);
        g_pcb = NULL;
    }
    g_state = NET_ERROR;
    g_last_reconnect_tb = mftb();
    bump_gen();
    lwsync();
    g_reconnects_done++;
}

/* v0.11.1: TCP tear-down explícito. Fecha PCB + zera ring. Marca ERROR pra que
 * stream_poll() reagende reconnect. Capture-rtp trata client_fd_==-1 e o
 * `has_seen_idr_` reseta na próxima accept, então o próximo AU enviado
 * é IDR-fresh (SPS+PPS+IDR concat). v0.14: com a thread de rede ativa, só
 * pede — quem mexe no lwIP é ela. */
void stream_force_reconnect(void) {
    if (g_threaded) { g_req_reconnect = 1; return; }
    do_reconnect_now();
}

static void on_err(void *arg, err_t err) {
    (void)arg;
    /* v0.16: no menu o vídeo tenta a cada 500ms e o PC recusa (RST) até o
     * jogo começar — quieto, só avisa se caiu uma conexão de verdade. */
    if (!g_quiet || g_state == NET_CONNECTED) printf("[stream] tcp err=%d\n", err);
    g_pcb = NULL;
    g_state = NET_ERROR;
    bump_gen();
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)pcb;
    if (err != ERR_OK) {
        printf("[stream] connect failed err=%d\n", err);
        g_state = NET_ERROR;
        return err;
    }
    printf("[stream] connected %s:%d\n", g_server_ip, g_server_port);
    g_state = NET_CONNECTED;
    tcp_write(pcb, "XCLD\n", 5, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

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
    g_connect_tb = mftb();
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

int stream_init(const char *server_ip, int server_port) {
    strncpy(g_server_ip, server_ip, sizeof(g_server_ip) - 1);
    g_server_ip[sizeof(g_server_ip) - 1] = '\0';
    g_server_port = server_port;

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

    if (ring_init(&g_ring, RING_CAPACITY) < 0) return -1;
    g_last_reconnect_tb = mftb();
    return stream_connect();
}

/* Um passo de rede: NIC + timers lwIP + reconexão + UDP pendente. */
static void net_step(void) {
    network_poll();
#ifdef XCLOUD360_NATIVE
    if (g_inject_req != g_inject) {
        int on = g_inject_req;
        if (on && g_pcb) {
            /* fecha o cliente :9000 (sem contar como reconexão pedida) */
            tcp_arg(g_pcb, NULL);
            tcp_err(g_pcb, NULL);
            tcp_recv(g_pcb, NULL);
            tcp_abort(g_pcb);
            g_pcb = NULL;
        }
        g_state = NET_IDLE;
        g_last_reconnect_tb = mftb();
        bump_gen();          /* o consumidor descarta o que havia no ring */
        lwsync();
        g_inject = on;
        printf("[stream] fonte do video: %s\n", on ? "WebRTC no Xbox" : "PC (TCP :9000)");
    }
#endif
    if (g_req_reconnect) {
        g_req_reconnect = 0;
        do_reconnect_now();
    }
#ifdef XCLOUD360_NATIVE
    if (!g_inject)
#endif
    if (g_state == NET_ERROR || g_state == NET_CLOSED || g_state == NET_IDLE) {
        uint64_t now = mftb();
        if (tb_diff_msec(now, g_last_reconnect_tb) > RECONNECT_DELAY_MS) {
#ifdef XCLOUD360_MENU
            /* v0.16: um SYN por vez (ver ctrl_conn.c, bug da v0.8.2); teto
             * de 3s pra um SYN preso do controle não travar o vídeo. */
            int c = ctrl_connecting_ms();
#ifdef XCLOUD360_LOGIN
            /* v0.17: nem com SYN de um cano HTTPS pendente */
            int pc = np_connecting_ms();
            if (pc >= 0 && pc < 3000) c = 0;
#endif
            if (c < 0 || c >= 3000)
#endif
            {
                g_last_reconnect_tb = now;
                if (!g_quiet) printf("[stream] reconnecting...\n");
                stream_connect();
            }
        }
    }
    input_udp_flush();
#ifdef XCLOUD360_MENU
    ctrl_step();
#endif
#ifdef XCLOUD360_LOGIN
    np_step();
#endif
#ifdef XCLOUD360_NATIVE
    /* UDP do WebRTC (abre/fecha socket, envios de outra thread) e depois o
     * WebRTC em si. xcw_idle() (espera dentro do xcw_create_offer) bombeia
     * este mesmo net_step: o g_in_extra evita reentrar no WebRTC. */
    xcw_net_step();
    if (g_net_extra && !g_in_extra) {
        g_in_extra = 1;
        g_net_extra();
        g_in_extra = 0;
    }
#endif
}

#ifdef XCLOUD360_NATIVE
void stream_net_pump(void) { net_step(); }
void stream_set_net_extra(void (*fn)(void)) { g_net_extra = fn; }
void stream_set_inject(int on) { g_inject_req = on ? 1 : 0; }
int  stream_inject_active(void) { return g_inject; }
uint32_t stream_inject_full(void) { return g_inject_full; }

/* Só a thread de rede (produtor do ring, como o on_recv). [u32 BE n][AU],
 * inteiro ou nada: nunca deixa meio AU no ring. */
int stream_inject_au(const uint8_t *au, size_t n) {
    if (!g_inject || !g_ring.buf) return -1;
    if (g_gen != g_gen_ack) return -1;            /* consumidor ainda descartando */
    if (n == 0 || n > STREAM_INJECT_MAX) return -1;
    if (ring_free_(&g_ring) < n + 4) { g_inject_full++; return -1; }
    uint8_t h[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    ring_write(&g_ring, h, 4);
    ring_write(&g_ring, au, n);
    return 0;
}
#endif

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

/* v0.14.4: entre polls a thread roda o hook (metade de baixo do convert no
 * h264play). O hook é checado durante a pausa toda pra começar assim que o
 * frame chega — o worker da metade de cima fica esperando por ele. */
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

void stream_poll(void) {
    if (g_threaded) return;   /* a thread de rede já faz isso */
    net_step();
}

/* v0.16: consumidor joga fora o que já chegou (menu: sobras de um
 * capture-rtp anterior não podem encher o ring). Mesmo lado do tail. */
void stream_discard(void) {
    lwsync();
    g_ring.tail = g_ring.head;
}

unsigned stream_reconnects_done(void) { return g_reconnects_done; }

int stream_connecting_ms(void) {
    if (g_state != NET_CONNECTING) return -1;
    uint64_t now = mftb();
    return now > g_connect_tb ? (int)tb_diff_msec(now, g_connect_tb) : 0;
}

void stream_set_quiet(int on) { g_quiet = on; }

int stream_net_up(void) { return g_ring.buf != NULL; }

size_t stream_read(uint8_t *dst, size_t n) { return ring_read_(&g_ring, dst, n); }
size_t stream_available(void) { return ring_used(&g_ring); }
size_t stream_dropped(void) { return g_ring.dropped; }
