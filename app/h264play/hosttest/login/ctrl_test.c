/*
 * Host-side (x86) test of app/h264play/source/ctrl_conn.c (v0.16): the
 * control connection to the PC launcher, with lwIP replaced by mocks
 * (hosttest/mock_net/). The test plays the lwIP side: it fires the
 * connected/recv/err callbacks by hand and reads what ctrl_conn wrote.
 *
 * Checks: HELLO first on every connect, queued lines sent in order, a new
 * connection drops stale queued lines, line framing across pbufs/chains and
 * CR stripping, PING every 2 s, 7 s silence -> abort + reconnect, 1 s
 * reconnect pacing after RST, SYN timeout, no SYN while the video SYN is
 * pending (< 3 s), RX full -> ERR_MEM then accepted after the main reads,
 * oversized garbage discarded, PC close, locks balanced.
 * Build/run: hosttest/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/tcp.h"
#include "ppc/timebase.h"
#include "../source/ctrl_conn.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- clock ---- */
static uint64_t g_tb;
uint64_t mock_mftb(void) { return g_tb; }
static void set_ms(uint64_t ms) { g_tb = ms * (PPC_TIMEBASE_FREQ / 1000); }
static uint64_t now_ms(void) { return g_tb / (PPC_TIMEBASE_FREQ / 1000); }

/* ---- locks ---- */
static int lock_depth;
void lock(unsigned int *l)   { if (*l) { fails++; printf("  FAIL: lock taken twice\n"); } *l = 1; lock_depth++; }
void unlock(unsigned int *l) { if (!*l) { fails++; printf("  FAIL: unlock of free lock\n"); } *l = 0; lock_depth--; }

/* ---- the video side of tcp_stream.c ---- */
static int g_video_connecting = -1;
int stream_connecting_ms(void) { return g_video_connecting; }

/* ---- lwIP mock ---- */
static struct tcp_pcb *g_pcb;     /* last pcb handed out */
static int n_new, n_connect, n_abort, n_close, n_output;
static char wire[1 << 16];
static size_t wire_len;
static unsigned recved;

struct tcp_pcb *tcp_new(void) {
    struct tcp_pcb *p = calloc(1, sizeof(*p));
    p->snd_buf = 8192;
    g_pcb = p;
    n_new++;
    return p;
}
void tcp_arg(struct tcp_pcb *pcb, void *arg) { pcb->arg = arg; }
void tcp_err(struct tcp_pcb *pcb, tcp_err_fn f) { pcb->errf = f; }
void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn f) { pcb->recv = f; }
err_t tcp_connect(struct tcp_pcb *pcb, ip_addr_t *ip, u16_t port, tcp_connected_fn f) {
    (void)ip;
    if (port != 9002) { fails++; printf("  FAIL: port %u\n", port); }
    pcb->connected = f;
    n_connect++;
    return ERR_OK;
}
err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t len, u8_t flags) {
    if (pcb->freed) { fails++; printf("  FAIL: write on freed pcb\n"); }
    if (!(flags & TCP_WRITE_FLAG_COPY)) { fails++; printf("  FAIL: write without COPY\n"); }
    if (len > pcb->snd_buf) return ERR_MEM;
    memcpy(wire + wire_len, data, len);
    wire_len += len;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *pcb) { (void)pcb; n_output++; return ERR_OK; }
void tcp_recved(struct tcp_pcb *pcb, u16_t len) { (void)pcb; recved += len; }
err_t tcp_close(struct tcp_pcb *pcb) { pcb->freed = 1; n_close++; return ERR_OK; }
void tcp_abort(struct tcp_pcb *pcb) {
    if (pcb->freed) { fails++; printf("  FAIL: abort of freed pcb\n"); }
    pcb->freed = 1; n_abort++;
}
u8_t pbuf_free(struct pbuf *p) { (void)p; return 1; }
u16_t pbuf_copy_partial(struct pbuf *p, void *dst, u16_t len, u16_t off) {
    u16_t done = 0;
    for (; p && done < len; p = p->next) {
        if (off >= p->len) { off -= p->len; continue; }
        u16_t n = p->len - off;
        if (n > len - done) n = len - done;
        memcpy((char *)dst + done, (char *)p->payload + off, n);
        done += n; off = 0;
    }
    return done;
}
int ipaddr_aton(const char *cp, ip_addr_t *a) { (void)cp; a->addr = 1; return 1; }

/* ---- helpers ---- */
static void wire_clear(void) { wire_len = 0; wire[0] = 0; }
static int wire_is(const char *s) { return wire_len == strlen(s) && memcmp(wire, s, wire_len) == 0; }

static err_t deliver2(const char *a, const char *b) {   /* chain of 1 or 2 pbufs */
    static struct pbuf p1, p2;
    p1.payload = (void *)a; p1.len = (u16_t)strlen(a); p1.next = NULL;
    if (b) { p2.payload = (void *)b; p2.len = (u16_t)strlen(b); p2.next = NULL; p1.next = &p2; }
    p1.tot_len = (u16_t)(p1.len + (b ? p2.len : 0));
    return g_pcb->recv(g_pcb->arg, g_pcb, &p1, ERR_OK);
}
static err_t deliver(const char *a) { return deliver2(a, NULL); }

static void connect_ok(void) {
    struct tcp_pcb *p = g_pcb;
    p->connected(p->arg, p, ERR_OK);
}

static int recv_is(const char *want) {
    char b[256];
    if (!ctrl_recv_line(b, sizeof(b))) { printf("    (no line, wanted '%s')\n", want); return 0; }
    if (strcmp(b, want) != 0) { printf("    (got '%s', wanted '%s')\n", b, want); return 0; }
    return 1;
}

int main(void) {
    printf("ctrl_conn                                   ");
    set_ms(100000);
    CHECK(ctrl_send_line("LIST") == -1, "send before init must fail");
    CHECK(!ctrl_connected(), "not connected before init");
    ctrl_step();
    CHECK(n_connect == 0, "no connect before init");
    ctrl_init("192.168.15.64", 9002, "0.16.0-test");

    /* video SYN pending < 3 s: control waits */
    g_video_connecting = 100;
    ctrl_step();
    CHECK(n_connect == 0, "must not SYN while the video SYN is pending");
    g_video_connecting = 3500;
    ctrl_step();
    CHECK(n_connect == 1 && ctrl_connecting_ms() == 0, "SYN once the video SYN is stale (n=%d)", n_connect);
    g_video_connecting = -1;

    /* refused (RST): next attempt only after 1 s */
    g_pcb->errf(g_pcb->arg, ERR_RST);
    CHECK(ctrl_connecting_ms() == -1 && !ctrl_connected(), "idle after RST");
    set_ms(100500); ctrl_step();
    CHECK(n_connect == 1, "no reconnect before 1 s");
    set_ms(101001); ctrl_step();
    CHECK(n_connect == 2, "reconnect after 1 s (n=%d)", n_connect);

    /* SYN never answered: dropped after 5 s, retried 1 s later */
    set_ms(106002); ctrl_step();
    CHECK(n_abort == 1, "SYN timeout aborts (aborts=%d)", n_abort);
    set_ms(107003); ctrl_step();
    CHECK(n_connect == 3, "retry after SYN timeout");

    /* connected: HELLO first, gen++, queued lines in order */
    unsigned gen0 = ctrl_conn_gen();
    wire_clear();
    connect_ok();
    CHECK(ctrl_connected() && ctrl_conn_gen() == gen0 + 1, "connected + gen");
    CHECK(g_pcb->flags & TF_NODELAY, "nagle off");
    CHECK(wire_is("HELLO\t0.16.0-test\n"), "HELLO on connect (wire='%.*s')", (int)wire_len, wire);
    wire_clear();
    CHECK(ctrl_send_line("LIST") == 0 && ctrl_send_line("PLAY\t9NBLGGH4R315") == 0, "queue");
    ctrl_step();
    CHECK(wire_is("LIST\nPLAY\t9NBLGGH4R315\n"), "lines sent in order (wire='%.*s')", (int)wire_len, wire);

    /* RX framing: split lines, chains, CRLF, partial line */
    CHECK(deliver2("STATE\tIDLE\tok\nGAME\tA1\tAlan", " Wake\t0\nEN") == ERR_OK, "recv ok");
    CHECK(recv_is("STATE\tIDLE\tok"), "line 1");
    CHECK(recv_is("GAME\tA1\tAlan Wake\t0"), "line 2 across pbufs");
    { char b[64]; CHECK(ctrl_recv_line(b, sizeof(b)) == 0, "partial line not returned"); }
    deliver("D\r\nPO");
    CHECK(recv_is("END"), "CRLF stripped");
    deliver("NG\n");
    CHECK(recv_is("PONG"), "line across deliveries");
    { char small[6]; deliver("STATE\tSTREAMING\n");
      CHECK(ctrl_recv_line(small, sizeof(small)) == 1 && strcmp(small, "STATE") == 0, "truncation to dst size"); }

    /* PING every 2 s, no timeout while the PC answers */
    uint64_t t = now_ms();
    wire_clear();
    set_ms(t + 1900); ctrl_step();
    CHECK(wire_len == 0, "no PING before 2 s");
    set_ms(t + 2001); ctrl_step();
    CHECK(wire_is("PING\n"), "PING at 2 s (wire='%.*s')", (int)wire_len, wire);
    deliver("PONG\n"); recv_is("PONG");
    set_ms(t + 8000); ctrl_step();
    CHECK(ctrl_connected(), "still connected: last RX at t+2001");

    /* TX stuck (snd_buf 0): lines stay queued; dropped on the next connection */
    g_pcb->snd_buf = 0;
    wire_clear();
    ctrl_send_line("STOP");
    ctrl_step();
    CHECK(wire_len == 0, "nothing written with snd_buf 0");

    /* 7 s of silence -> abort + reconnect */
    int aborts = n_abort;
    set_ms(t + 2001 + 7001); ctrl_step();
    CHECK(n_abort == aborts + 1 && !ctrl_connected(), "silence timeout aborts");
    CHECK(ctrl_send_line("LIST") == -1, "send while disconnected fails");
    set_ms(now_ms() + 1001); ctrl_step();
    CHECK(n_connect == 4, "reconnect after timeout");
    wire_clear();
    connect_ok();
    CHECK(wire_is("HELLO\t0.16.0-test\n"), "stale STOP dropped, only HELLO (wire='%.*s')", (int)wire_len, wire);
    { char b[64]; CHECK(ctrl_recv_line(b, sizeof(b)) == 0, "RX cleared on new connection"); }

    /* RX full: ERR_MEM until the main reads */
    static char line[101];
    memset(line, 'x', 99); line[99] = '\n'; line[100] = 0;
    int accepted = 0, refused = 0;
    for (int i = 0; i < 700; i++) { if (deliver(line) == ERR_OK) accepted++; else { refused++; break; } }
    CHECK(refused == 1 && accepted == (64 * 1024) / 100, "RX full -> ERR_MEM after %d lines", accepted);
    for (int i = 0; i < 10; i++) recv_is("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    CHECK(deliver(line) == ERR_OK, "accepted again after the main read (compaction)");
    { char b[256]; int n = 0; while (ctrl_recv_line(b, sizeof(b))) n++;
      CHECK(n == accepted - 10 + 1, "all lines intact after compaction (%d)", n); }

    /* 32 KB without a newline is garbage: discarded, RX keeps working */
    static char junk[1001];
    memset(junk, 'j', 1000); junk[1000] = 0;
    for (int i = 0; i < 33; i++) deliver(junk);
    { char b[64]; CHECK(ctrl_recv_line(b, sizeof(b)) == 0, "no line from junk"); }
    deliver("\nSTATE\tIDLE\n");
    { char b[64]; int got = 0;
      while (ctrl_recv_line(b, sizeof(b))) if (strcmp(b, "STATE\tIDLE") == 0) got = 1;
      CHECK(got, "RX recovers after junk"); }

    /* PC closes: tcp_close, idle, reconnect after 1 s */
    int closes = n_close;
    CHECK(g_pcb->recv(g_pcb->arg, g_pcb, NULL, ERR_OK) == ERR_OK, "close handled");
    CHECK(n_close == closes + 1 && !ctrl_connected(), "closed");
    set_ms(now_ms() + 1001); ctrl_step();
    CHECK(n_connect == 5, "reconnect after close");

    CHECK(lock_depth == 0, "locks balanced (%d)", lock_depth);
    printf("%s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
