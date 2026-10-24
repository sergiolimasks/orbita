/* -----------------------------------------------------------------------------
 * XCloud360 — TCP live H.264 stream client for Xbox 360 (libxenon + lwip 1.4.1)
 *
 * Substitui o stream embedded no ELF por live stream sobre LAN.
 *
 * ARQUITETURA (obrigatoria pelo libxenon):
 *   lwipopts.h do libxenon fixa NO_SYS=1 e LWIP_SOCKET=0.
 *   NAO existe lwip_socket()/lwip_recv() nesse port. So RAW API (callbacks).
 *   Todos os callbacks TCP rodam no MESMO thread do main loop, disparados
 *   sincronamente por network_poll() -> enet_poll() -> ip_input().
 *
 *   MODELO:  single-thread, cooperativo
 *     - network_poll() drena Ethernet RX -> lwip -> chama nosso recv_cb
 *     - recv_cb copia pbuf pra ring buffer LOCK-FREE (mesmo thread = safe)
 *     - decoder loop consome do ring como [u32_be size][AU bytes], feed direto
 *       no codec (sem parser — 1 AU/msg garantido pelo feeder)
 *
 *   Chame network_poll() **entre cada frame decodado**. Nao ha timer callback,
 *   nao ha ISR. Tudo cooperative.
 *
 * DEPS: libxenon (network/network.h, lwip/tcp.h), ppc/timebase.h,
 *       ffmpeg libavcodec (av_parser_parse2, avcodec_send_packet).
 * -----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <ppc/timebase.h>
#include <network/network.h>
#include <lwip/tcp.h>
#include <lwip/ip_addr.h>
#include <lwip/err.h>
#include <lwip/pbuf.h>

#include <libavcodec/avcodec.h>

/* --- CONFIG --------------------------------------------------------------- */
#define STREAM_SERVER_IP      "192.168.15.64"
#define STREAM_SERVER_PORT    9000
#define RING_CAPACITY         (1024 * 1024)    /* 1 MB — IDR 720p60 pode chegar ~200KB */
#define RECONNECT_DELAY_MS    500
#define POLL_STALL_WARN_MS    200              /* alerta se poll ficar sem chamar */

/* --- STATE ---------------------------------------------------------------- */
typedef struct {
    uint8_t  *buf;          /* backing storage */
    size_t    cap;
    volatile size_t head;   /* producer (recv_cb) */
    volatile size_t tail;   /* consumer (decoder) */
    size_t    dropped;      /* stat: bytes descartados por overflow */
} ring_t;

typedef enum {
    NET_IDLE = 0,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_ERROR,
    NET_CLOSED
} net_state_t;

static ring_t          g_ring;
static struct tcp_pcb *g_pcb = NULL;
static net_state_t     g_state = NET_IDLE;
static uint64_t        g_last_reconnect_tb = 0;
static uint64_t        g_last_poll_tb = 0;

/* --- RING (SPSC, single-thread cooperative = seguro sem lock) ------------- */
static int ring_init(ring_t *r, size_t cap) {
    r->buf = malloc(cap);
    if (!r->buf) return -1;
    r->cap = cap; r->head = r->tail = r->dropped = 0;
    return 0;
}

static inline size_t ring_used(const ring_t *r) {
    size_t h = r->head, t = r->tail;
    return (h >= t) ? (h - t) : (r->cap - (t - h));
}

static inline size_t ring_free(const ring_t *r) {
    return r->cap - ring_used(r) - 1;  /* -1 pra distinguir cheio de vazio */
}

static size_t ring_write(ring_t *r, const uint8_t *src, size_t n) {
    size_t avail = ring_free(r);
    if (n > avail) {
        r->dropped += (n - avail);
        n = avail;
    }
    if (!n) return 0;
    size_t h = r->head;
    size_t first = (h + n <= r->cap) ? n : (r->cap - h);
    memcpy(r->buf + h, src, first);
    if (n > first) memcpy(r->buf, src + first, n - first);
    r->head = (h + n) % r->cap;
    return n;
}

static size_t ring_read(ring_t *r, uint8_t *dst, size_t n) {
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

/* Chamado quando dados chegam. Rodando no MESMO thread do main loop. */
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;

    /* peer fechou conexao */
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

    /* Copia pbuf chain pro ring. Nao segura pbuf: libera imediato pra
     * nao esgotar PBUF_POOL_SIZE (512 no libxenon). */
    u16_t total = p->tot_len;
    struct pbuf *q;
    for (q = p; q != NULL; q = q->next) {
        ring_write(&g_ring, (const uint8_t *)q->payload, q->len);
    }

    /* CRITICO: avisa lwip que consumimos, senao a janela nao abre e o
     * throughput cai pra ~1/N do TCP_WND. */
    tcp_recved(pcb, total);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)arg;
    printf("[stream] tcp err=%d\n", err);
    g_pcb = NULL;                /* lwip ja liberou o PCB */
    g_state = NET_ERROR;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)pcb;
    if (err != ERR_OK) {
        printf("[stream] connect failed err=%d\n", err);
        g_state = NET_ERROR;
        return err;
    }
    printf("[stream] connected to %s:%d\n", STREAM_SERVER_IP, STREAM_SERVER_PORT);
    g_state = NET_CONNECTED;

    /* Opcional: manda um handshake pro server saber que estamos prontos.
     * Ex: "GET /live HTTP/1.0\r\n\r\n"  ou byte magico "XCLD". */
    const char *hello = "XCLD\n";
    tcp_write(pcb, hello, 5, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

/* --- CONNECT LOGIC -------------------------------------------------------- */
static int stream_connect(void) {
    ip_addr_t remote;
    if (!ipaddr_aton(STREAM_SERVER_IP, &remote)) return -1;

    g_pcb = tcp_new();
    if (!g_pcb) { printf("[stream] tcp_new failed\n"); return -1; }

    /* Priority ajuda quando ha outros PCBs (ex: DHCP renew). */
    tcp_setprio(g_pcb, TCP_PRIO_MIN + 5);
    tcp_arg(g_pcb, NULL);
    tcp_err(g_pcb, on_err);
    tcp_recv(g_pcb, on_recv);
    /* tcp_sent nao precisa - nao mandamos backpressure aqui */

    g_state = NET_CONNECTING;
    err_t e = tcp_connect(g_pcb, &remote, STREAM_SERVER_PORT, on_connected);
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

int stream_init(void) {
    /* network_init() sob Aurora: em geral seguro chamar de novo. libxenon
     * faz lwip_init() + netif_add() + dhcp_start() sem verificar se ja
     * existe. Isso VAI vazar o netif anterior. Preferido: pular init se ja
     * up (netif.ip_addr.addr != 0). */
    extern struct netif netif;
    if (netif.ip_addr.addr == 0) {
        int r = network_init();
        if (r == NETWORK_INIT_FAILURE) {
            printf("[stream] network_init hard fail\n");
            return -1;
        }
        /* r == NETWORK_INIT_DHCP_FAILURE: setou IP estatico 192.168.1.99,
         *   nao serve pra rede 192.168.15.x — retorna erro. */
        if (r == NETWORK_INIT_DHCP_FAILURE) {
            printf("[stream] DHCP falhou; ajuste manual necessario\n");
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

/* Deve rodar TODA iteracao do main loop (frame). Cheap: so if/else. */
void stream_poll(void) {
    uint64_t now = mftb();

    /* watchdog: se poll ficar >200ms sem rodar, TCP retx dispara e stream
     * fica jitteroso. tcp_tmr roda a cada 250ms — decodar por 30ms/frame
     * (~33fps) e chamar poll no fim ta OK. Se decode >200ms, precisa
     * chamar network_poll() DENTRO do decode loop tambem. */
    if (g_last_poll_tb && tb_diff_msec(now, g_last_poll_tb) > POLL_STALL_WARN_MS) {
        printf("[stream] warn: poll stalled %llums\n",
               tb_diff_msec(now, g_last_poll_tb));
    }
    g_last_poll_tb = now;

    network_poll();

    if (g_state == NET_ERROR || g_state == NET_CLOSED || g_state == NET_IDLE) {
        if (tb_diff_msec(now, g_last_reconnect_tb) > RECONNECT_DELAY_MS) {
            g_last_reconnect_tb = now;
            printf("[stream] reconnecting...\n");
            stream_connect();
        }
    }
}

/* Le bytes do ring TCP. Formato wire: [u32_be size][annex-b AU]. */
size_t stream_read(uint8_t *dst, size_t n) {
    return ring_read(&g_ring, dst, n);
}

size_t stream_available(void) {
    return ring_used(&g_ring);
}

/* -----------------------------------------------------------------------------
 * DECODER LOOP — length-prefix AU
 * -----------------------------------------------------------------------------
 * Wire: [uint32_be size][annex-b bytes] repetido. Feeder h264_feeder.py e
 * (futuro) VideoJitterBuffer do libpeer entregam AU delimitada. Sem parser
 * ffmpeg no caminho — AU pronta vai direto pro avcodec_send_packet, sem
 * lookahead de 1 frame. Elimina ~16ms @ 60fps vs av_parser_parse2.
 */

static AVCodecContext *g_ctx = NULL;
static AVPacket *g_pkt = NULL;

int decoder_init(void) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return -1;
    g_ctx = avcodec_alloc_context3(codec);
    g_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    g_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(g_ctx, codec, NULL) < 0) return -1;
    /* Sem parser: length-prefix garante 1 AU/pacote (feeder emite assim).
     * Elimina 1 frame de lookahead do av_parser_parse2. */
    g_pkt = av_packet_alloc();
    return 0;
}

/* --- LENGTH-PREFIX AU EXTRACTOR ------------------------------------------
 * Estado persistente entre chamadas: se ring nao tem AU completa, retorna 0
 * e tenta na proxima. Sem parser ffmpeg no caminho.
 */
static uint8_t  g_au_buf[512 * 1024];   /* max AU size aceito (IDR pode ser grande) */
static uint32_t g_pending_au_size = 0;  /* 0 = precisa ler header */

int decoder_pump(void (*on_frame)(AVFrame *)) {
    int frames = 0;

    for (;;) {
        /* Fase 1: ler header 4B BE se ainda nao temos AU pendente */
        if (g_pending_au_size == 0) {
            if (ring_used(&g_ring) < 4) return frames;
            uint8_t hdr[4];
            ring_read(&g_ring, hdr, 4);
            g_pending_au_size = ((uint32_t)hdr[0] << 24) |
                                ((uint32_t)hdr[1] << 16) |
                                ((uint32_t)hdr[2] <<  8) |
                                ((uint32_t)hdr[3]);
            if (g_pending_au_size == 0 || g_pending_au_size > sizeof(g_au_buf)) {
                printf("[stream] bogus au_size=%u — resync via reconnect\n",
                       g_pending_au_size);
                g_pending_au_size = 0;
                return frames;
            }
        }

        /* Fase 2: ler AU completa se ring tem bytes suficientes */
        if (ring_used(&g_ring) < g_pending_au_size) return frames;
        ring_read(&g_ring, g_au_buf, g_pending_au_size);

        g_pkt->data = g_au_buf;
        g_pkt->size = (int)g_pending_au_size;
        g_pending_au_size = 0;

        if (avcodec_send_packet(g_ctx, g_pkt) == 0) {
            AVFrame *frm = av_frame_alloc();
            while (avcodec_receive_frame(g_ctx, frm) == 0) {
                if (on_frame) on_frame(frm);
                frames++;
            }
            av_frame_free(&frm);
        }
    }
}

/* -----------------------------------------------------------------------------
 * MAIN LOOP
 * -----------------------------------------------------------------------------
 */

static void render_frame(AVFrame *f) {
    /* Ligar aqui no seu pipeline YUV->RGB do XCloud360.
     * f->data[0/1/2] = Y/U/V planos, f->linesize[0/1/2]. */
    (void)f;
}

int main(void) {
    if (stream_init() < 0) return 1;
    if (decoder_init() < 0) return 1;

    for (;;) {
        stream_poll();                      /* drena rede + reconnect */
        decoder_pump(render_frame);         /* consume ring, decode */
        /* Aqui: swap framebuffer / vsync. Nao dormir mais que ~10-15ms
         * senao network_poll fica muito tempo sem rodar. Se sua render
         * loop faz vsync 16.6ms (60fps) e decode 20-30ms, considere
         * chamar stream_poll() 2x por loop (antes e depois do decode). */
    }
    return 0;
}
