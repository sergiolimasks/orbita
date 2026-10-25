/*
 * XCloud360 — conexão de controle com o launcher do PC (v0.16, make MENU=1).
 * Protocolo e regras de thread em ctrl_conn.h.
 *
 * Por que dentro da thread de rede e não um cliente separado:
 *   lwIP (NO_SYS=1) não é thread-safe e desde a v0.14 só a thread de rede
 *   chama network_poll. Um segundo tcp_pcb aqui é só mais um conjunto de
 *   callbacks rodando no mesmo network_poll — nenhuma chamada lwIP sai do
 *   main. O main conversa com isto por dois buffers com lock():
 *     RX (64KB, rede escreve / main lê linhas) e TX (4KB, main escreve
 *     linhas / rede manda com tcp_write).
 *
 * Bug da v0.8.2 (tcp_new logo depois do tcp_connect do vídeo, antes do
 * callback dele, corrompeu tcp_active_pcbs): por precaução nunca abrimos o
 * SYN do controle enquanto o do vídeo está pendente (e o tcp_stream.c faz o
 * contrário), com teto de 3s pra um SYN preso não matar o outro de fome.
 */
#ifdef XCLOUD360_MENU

#include "ctrl_conn.h"
#include "tcp_stream.h"
#ifdef XCLOUD360_LOGIN
#include "login/net_pipe.h"
#endif

#include <stdio.h>
#include <string.h>

#include <ppc/atomic.h>
#include <ppc/timebase.h>
#include <lwip/tcp.h>
#include <lwip/ip_addr.h>
#include <lwip/err.h>
#include <lwip/pbuf.h>

#define CTRL_RX_CAP          (64 * 1024)   /* catálogo inteiro (~500 jogos) cabe */
#define CTRL_TX_CAP          (4 * 1024)
#define CTRL_RECONNECT_MS    1000
#define CTRL_PING_MS         2000
#define CTRL_RX_TIMEOUT_MS   7000          /* 3 PINGs sem nada de volta */
#define CTRL_SYN_TIMEOUT_MS  5000          /* lwIP tentaria o SYN por ~1min */
#define CTRL_SERIALIZE_MS    3000

/* Seam pro teste no host (hosttest/ctrl_test.c): lwsync de verdade no Xbox. */
#ifndef CTRL_LWSYNC
#define CTRL_LWSYNC() __asm__ __volatile__("lwsync" ::: "memory")
#endif

typedef enum { C_IDLE = 0, C_CONNECTING, C_CONNECTED } ctrl_state_t;

static volatile int      g_inited = 0;
static char              g_ip[32];
static int               g_port;
static char              g_hello[96];
static struct tcp_pcb   *g_pcb = NULL;
static volatile int      g_state = C_IDLE;
static volatile unsigned g_gen = 0;
static uint64_t          g_last_attempt_tb = 0;
static uint64_t          g_connect_tb = 0;
static uint64_t          g_last_rx_tb = 0;
static uint64_t          g_last_ping_tb = 0;

/* RX: [g_rx_rd, g_rx_wr) ainda não lido pelo main. */
static unsigned int g_rx_lock = 0;
static char         g_rx[CTRL_RX_CAP];
static size_t       g_rx_rd = 0, g_rx_wr = 0;

static unsigned int g_tx_lock = 0;
static char         g_tx[CTRL_TX_CAP];
static size_t       g_tx_len = 0;

/* mftb lê TBL e TBU separados; um "agora" menor que o carimbo (corrida
 * entre threads ou o glitch de virada do TBL) não pode virar timeout. */
static unsigned long ms_since(uint64_t now, uint64_t then) {
    return now > then ? tb_diff_msec(now, then) : 0;
}

/* Com g_tx_lock na mão. */
static int tx_append(const char *s, size_t n) {
    if (g_tx_len + n > CTRL_TX_CAP) return -1;
    memcpy(g_tx + g_tx_len, s, n);
    g_tx_len += n;
    return 0;
}

static void ctrl_drop(const char *why) {
    if (g_pcb) {
        tcp_arg(g_pcb, NULL);
        tcp_err(g_pcb, NULL);
        tcp_recv(g_pcb, NULL);
        tcp_abort(g_pcb);
        g_pcb = NULL;
    }
    if (why) printf("%s", why);
    g_state = C_IDLE;
    g_last_attempt_tb = mftb();
}

/* Manda o que couber da fila TX. Só a thread de rede. */
static void ctrl_flush(void) {
    if (!g_pcb || g_state != C_CONNECTED) return;
    int sent = 0;
    lock(&g_tx_lock);
    size_t n = g_tx_len;
    size_t room = tcp_sndbuf(g_pcb);
    if (n > room) n = room;
    if (n > 0 && tcp_write(g_pcb, g_tx, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        memmove(g_tx, g_tx + n, g_tx_len - n);
        g_tx_len -= n;
        sent = 1;
    }
    unlock(&g_tx_lock);
    if (sent) tcp_output(g_pcb);
}

static void on_err(void *arg, err_t err) {
    (void)arg;
    /* O pcb já foi liberado pelo lwIP. Recusa (RST) a cada 1s com o launcher
     * fechado é o caso normal: só avisa se caiu uma conexão de verdade. */
    if (g_state == C_CONNECTED) printf("[ctrl] conexao perdida (err=%d)\n", err);
    g_pcb = NULL;
    g_state = C_IDLE;
    g_last_attempt_tb = mftb();
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (p == NULL) {
        printf("[ctrl] PC fechou a conexao\n");
        tcp_arg(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_recv(pcb, NULL);
        g_pcb = NULL;
        g_state = C_IDLE;
        g_last_attempt_tb = mftb();
        if (tcp_close(pcb) != ERR_OK) { tcp_abort(pcb); return ERR_ABRT; }
        return ERR_OK;
    }
    if (err != ERR_OK) { pbuf_free(p); return err; }

    lock(&g_rx_lock);
    if (CTRL_RX_CAP - g_rx_wr < p->tot_len && g_rx_rd > 0) {
        memmove(g_rx, g_rx + g_rx_rd, g_rx_wr - g_rx_rd);   /* compacta */
        g_rx_wr -= g_rx_rd;
        g_rx_rd = 0;
    }
    if (CTRL_RX_CAP - g_rx_wr < p->tot_len) {
        unlock(&g_rx_lock);
        return ERR_MEM;   /* main atrasado: lwIP guarda o pbuf e tenta depois */
    }
    pbuf_copy_partial(p, g_rx + g_rx_wr, p->tot_len, 0);
    g_rx_wr += p->tot_len;
    unlock(&g_rx_lock);

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    g_last_rx_tb = mftb();
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        g_state = C_IDLE;
        return err;
    }
    tcp_nagle_disable(pcb);   /* linhas curtas e interativas */
    /* Conexão nova = sessão nova: sobras da anterior não valem (um PLAY que
     * ficou na fila não deve disparar minutos depois). HELLO sempre primeiro. */
    lock(&g_rx_lock);
    g_rx_rd = g_rx_wr = 0;
    unlock(&g_rx_lock);
    lock(&g_tx_lock);
    g_tx_len = 0;
    tx_append(g_hello, strlen(g_hello));
    unlock(&g_tx_lock);
    uint64_t now = mftb();
    g_last_rx_tb = now;
    g_last_ping_tb = now;
    CTRL_LWSYNC();
    g_state = C_CONNECTED;
    g_gen++;
    printf("[ctrl] conectado %s:%d\n", g_ip, g_port);
    ctrl_flush();
    return ERR_OK;
}

static void ctrl_connect(void) {
    ip_addr_t remote;
    if (!ipaddr_aton(g_ip, &remote)) return;
    g_pcb = tcp_new();
    if (!g_pcb) { printf("[ctrl] tcp_new falhou\n"); return; }
    tcp_arg(g_pcb, NULL);
    tcp_err(g_pcb, on_err);
    tcp_recv(g_pcb, on_recv);
    g_state = C_CONNECTING;
    g_connect_tb = mftb();
    err_t e = tcp_connect(g_pcb, &remote, g_port, on_connected);
    if (e != ERR_OK) {
        tcp_abort(g_pcb);
        g_pcb = NULL;
        g_state = C_IDLE;
    }
}

int ctrl_init(const char *server_ip, int server_port, const char *version) {
    strncpy(g_ip, server_ip, sizeof(g_ip) - 1);
    g_ip[sizeof(g_ip) - 1] = '\0';
    g_port = server_port;
    snprintf(g_hello, sizeof(g_hello), "HELLO\t%s\n", version);
    g_last_attempt_tb = 0;   /* primeira tentativa já no próximo net_step */
    CTRL_LWSYNC();
    g_inited = 1;
    return 0;
}

int ctrl_connecting_ms(void) {
    if (g_state != C_CONNECTING) return -1;
    return (int)ms_since(mftb(), g_connect_tb);
}

void ctrl_step(void) {
    if (!g_inited) return;
    uint64_t now = mftb();
    switch (g_state) {
    case C_CONNECTED:
        if (ms_since(now, g_last_ping_tb) >= CTRL_PING_MS) {
            lock(&g_tx_lock);
            tx_append("PING\n", 5);   /* fila cheia: o próximo sai em 2s */
            unlock(&g_tx_lock);
            g_last_ping_tb = now;
        }
        ctrl_flush();
        /* Conexão meio-aberta (PC reiniciou, cabo): sem bytes nenhum desde 3
         * PINGs atrás. Derruba e reconecta em vez de ficar surdo. */
        if (ms_since(now, g_last_rx_tb) > CTRL_RX_TIMEOUT_MS)
            ctrl_drop("[ctrl] PC sem resposta, reconectando\n");
        break;
    case C_CONNECTING:
        if (ms_since(now, g_connect_tb) > CTRL_SYN_TIMEOUT_MS) ctrl_drop(NULL);
        break;
    default: {
        if (ms_since(now, g_last_attempt_tb) < CTRL_RECONNECT_MS) break;
        int v = stream_connecting_ms();
        if (v >= 0 && v < CTRL_SERIALIZE_MS) break;   /* SYN do vídeo pendente */
#ifdef XCLOUD360_LOGIN
        v = np_connecting_ms();
        if (v >= 0 && v < CTRL_SERIALIZE_MS) break;   /* v0.17: SYN de um cano HTTPS */
#endif
        g_last_attempt_tb = now;
        ctrl_connect();
        break;
    }
    }
}

int ctrl_send_line(const char *line) {
    if (!g_inited || g_state != C_CONNECTED) return -1;
    size_t n = strlen(line);
    int r = -1;
    lock(&g_tx_lock);
    if (g_tx_len + n + 1 <= CTRL_TX_CAP) {
        tx_append(line, n);
        tx_append("\n", 1);
        r = 0;
    }
    unlock(&g_tx_lock);
    return r;
}

int ctrl_recv_line(char *dst, size_t n) {
    if (!g_inited || n == 0) return 0;
    int r = 0;
    lock(&g_rx_lock);
    const char *s = g_rx + g_rx_rd;
    size_t avail = g_rx_wr - g_rx_rd;
    const char *nl = memchr(s, '\n', avail);
    if (nl) {
        size_t len = (size_t)(nl - s);
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(dst, s, c);
        dst[c] = '\0';
        if (c > 0 && dst[c - 1] == '\r') dst[c - 1] = '\0';
        g_rx_rd += len + 1;
        r = 1;
    } else if (avail >= CTRL_RX_CAP / 2) {
        /* 32KB sem "\n" não é protocolo: descarta pra não travar o RX. */
        g_rx_rd = g_rx_wr;
    }
    if (g_rx_rd == g_rx_wr) g_rx_rd = g_rx_wr = 0;
    unlock(&g_rx_lock);
    return r;
}

int ctrl_connected(void) { return g_inited && g_state == C_CONNECTED; }

unsigned ctrl_conn_gen(void) { return g_gen; }

#else
typedef int xcloud360_ctrl_conn_disabled;   /* MENU=0: unidade vazia */
#endif /* XCLOUD360_MENU */
