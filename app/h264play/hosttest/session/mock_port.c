/*
 * XCloud360 stage 3 — host: xc_port.h + xs_port.h + https_request() falsos.
 *   - relógio VIRTUAL (xcn_sleep_ms só avança o relógio; cada pedido HTTPS
 *     "custa" MOCK_RTT_MS): os testes de fila/timeout de 30 min rodam em ms;
 *   - "pendrive" em memória (TOKEN.TXT);
 *   - https_request grava cada pedido (método, host, path, cabeçalhos, corpo)
 *     e pergunta a resposta pro g_mock_handler do teste. Nada de rede.
 *   - xc_log vai pra um buffer (o teste confere que nenhum token vaza).
 */
#include "mock_port.h"
#include "../../source/login/xc_port.h"
#include "../../source/login/https.h"
#include "../../source/session/xs_port.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t g_now_ms = 1000000;
mock_handler_t g_mock_handler = NULL;
void *g_mock_ctx = NULL;
mock_req_t g_reqs[MOCK_MAX_REQ];
int g_nreq = 0;
char *g_logbuf = NULL;
size_t g_loglen = 0, g_logcap = 0;
int g_verbose = 0;

static char g_token[4096];
static int  g_token_len = -1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static char *dupn(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void mock_reset(void) {
    for (int i = 0; i < g_nreq; i++) { free(g_reqs[i].headers); free(g_reqs[i].body); }
    g_nreq = 0;
    g_loglen = 0;
    if (g_logbuf) g_logbuf[0] = 0;
    g_token_len = -1;
    g_mock_handler = NULL;
    g_mock_ctx = NULL;
    g_mock_conns = 0;
}

int g_mock_conns = 0;

void mock_set_token(const char *t) {
    if (!t) { g_token_len = -1; return; }
    g_token_len = (int)strlen(t);
    memcpy(g_token, t, (size_t)g_token_len + 1);
}

const char *mock_get_token(void) { return g_token_len >= 0 ? g_token : NULL; }

/* ------------------------------------------------------------- xc_port */
uint64_t xcn_now_ms(void) { return g_now_ms; }
uint64_t xcn_now_us(void) { return g_now_ms * 1000; }
int      xcn_connect_start(uint32_t ip, int p, int *h) { (void)ip; (void)p; (void)h; return XCN_ECONN; }
int      xcn_connect_poll(int h) { (void)h; return XCN_ECONN; }
int      xcn_try_send(int h, const uint8_t *b, size_t n) { (void)h; (void)b; (void)n; return XCN_EIO; }
int      xcn_try_recv(int h, uint8_t *b, size_t n) { (void)h; (void)b; (void)n; return XCN_EIO; }
void     xcn_close_nb(int h) { (void)h; }
void     xcn_sleep_ms(int ms) { if (ms > 0) g_now_ms += (uint64_t)ms; }
int      xcn_cancelled(void) { return 0; }
int      xcn_resolve(const char *h, uint32_t *ip, int t) { (void)h; (void)t; *ip = 0; return XCN_EDNS; }
int      xcn_connect(uint32_t ip, int p, int t, int *h) { (void)ip; (void)p; (void)t; (void)h; return XCN_ECONN; }
int      xcn_send(int h, const uint8_t *b, size_t n, int t) { (void)h; (void)b; (void)n; (void)t; return XCN_EIO; }
int      xcn_recv(int h, uint8_t *b, size_t n, int t) { (void)h; (void)b; (void)n; (void)t; return XCN_EIO; }
void     xcn_close(int h) { (void)h; }

void xc_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    if (g_loglen + (size_t)n + 2 > g_logcap) {
        g_logcap = (g_logcap + (size_t)n + 2) * 2;
        g_logbuf = (char *)realloc(g_logbuf, g_logcap);
    }
    memcpy(g_logbuf + g_loglen, line, (size_t)n);
    g_loglen += (size_t)n;
    g_logbuf[g_loglen++] = '\n';
    g_logbuf[g_loglen] = 0;
    if (g_verbose) printf("    LOG %s\n", line);
}

long long xc_time_now(void) { return 1793000000LL; }
void      xc_time_set(long long t, const char *s) { (void)t; (void)s; }
long long xc_time_floor(void) { return 1790000000LL; }

int xc_store_load(const char *name, void *buf, size_t cap) {
    if (strcmp(name, "TOKEN.TXT") || g_token_len < 0) return -1;
    size_t n = (size_t)g_token_len < cap ? (size_t)g_token_len : cap;
    memcpy(buf, g_token, n);
    return (int)n;
}

int xc_store_save(const char *name, const void *buf, size_t n) {
    if (strcmp(name, "TOKEN.TXT") || n >= sizeof(g_token)) return -1;
    memcpy(g_token, buf, n);
    g_token[n] = 0;
    g_token_len = (int)n;
    return 0;
}

int xc_store_remove(const char *name) {
    if (strcmp(name, "TOKEN.TXT")) return -1;
    g_token_len = -1;
    return 0;
}

size_t xc_seed_material(uint8_t *out, size_t cap) { (void)out; (void)cap; return 0; }

/* ------------------------------------------------------------- xs_port */
void xs_lock(void) { pthread_mutex_lock(&g_mu); }
void xs_unlock(void) { pthread_mutex_unlock(&g_mu); }

/* --------------------------------------------------------------- https */
int https_init(xc_err_t *e) { (void)e; return 0; }

int https_request(const char *step, const char *method, const char *host, const char *path,
                  const char *headers, const char *body, size_t body_len, size_t max_body,
                  https_resp_t *r, xc_err_t *e) {
    (void)max_body;
    memset(r, 0, sizeof(*r));
    g_now_ms += MOCK_RTT_MS;
    mock_req_t *q = NULL;
    if (g_nreq < MOCK_MAX_REQ) {
        q = &g_reqs[g_nreq++];
        memset(q, 0, sizeof(*q));
        snprintf(q->method, sizeof(q->method), "%s", method);
        snprintf(q->host, sizeof(q->host), "%s", host);
        snprintf(q->path, sizeof(q->path), "%s", path);
        q->headers = dupn(headers ? headers : "", headers ? strlen(headers) : 0);
        q->body = body ? dupn(body, body_len) : NULL;
        q->has_body = body != NULL || strcmp(method, "POST") == 0;
        q->t = g_now_ms;
    }
    int status = 500;
    char *resp = NULL;
    int terr = g_mock_handler ? g_mock_handler(g_mock_ctx, q, &status, &resp) : 0;
    if (terr != 0) {
        free(resp);
        e->code = terr;
        e->tls = 0;
        snprintf(e->msg, sizeof(e->msg), "%s em %s", terr == HTTPS_ETIMEOUT ? "timeout" : "rede", step);
        return terr;
    }
    r->status = status;
    if (!resp) resp = dupn("", 0);
    r->body = resp;
    r->len = strlen(resp);
    if (q) q->status = status;
    return 0;
}

void https_free(https_resp_t *r) {
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

int https_random(unsigned char *out, size_t n) { memset(out, 7, n); return 0; }
int https_probe(const char *host, xc_err_t *e) { (void)host; (void)e; return -1; }

/* ------------------------------------------------ v0.19.2: keep-alive
 * Conexão falsa: só conta (g_mock_conns) e marca os pedidos com conn_id/nb.
 * O não-bloqueante faz o pedido no start e entrega no 1º poll depois de
 * MOCK_RTT_MS (sem avançar o relógio virtual: a hw1 não espera). */
struct https_conn {
    char host[128];
    int  id;
    int  inflight;
    int  rc;
    uint64_t ready_at;
    https_resp_t r;
    xc_err_t e;
    https_conn_stats_t st;
};

https_conn_t *https_conn_new(const char *host) {
    https_conn_t *c = (https_conn_t *)calloc(1, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->id = ++g_mock_conns;
    return c;
}
void https_conn_free(https_conn_t *c) {
    if (!c) return;
    if (c->inflight) https_free(&c->r);
    free(c);
}
const char *https_conn_host(const https_conn_t *c) { return c->host; }
int  https_conn_is_open(const https_conn_t *c) { return c && !c->inflight; }
int  https_conn_busy(const https_conn_t *c) { return c && c->inflight; }
void https_conn_get_stats(const https_conn_t *c, https_conn_stats_t *s) {
    if (c) *s = c->st;
    else memset(s, 0, sizeof(*s));
}

static void tag_last(https_conn_t *c, int nb) {
    if (g_nreq > 0) { g_reqs[g_nreq - 1].nb = nb; g_reqs[g_nreq - 1].conn_id = c->id; }
}

int https_conn_request(https_conn_t *c, const char *step, const char *method, const char *path,
                       const char *headers, const char *body, size_t body_len, size_t max_body,
                       https_resp_t *r, xc_err_t *e) {
    int rc = https_request(step, method, c->host, path, headers, body, body_len, max_body, r, e);
    tag_last(c, 0);
    c->st.requests++;
    return rc;
}

int https_conn_start(https_conn_t *c, const char *step, const char *method, const char *path,
                     const char *headers, const char *body, size_t body_len, size_t max_body, xc_err_t *e) {
    if (c->inflight) return HTTPS_EBUSY;
    uint64_t now = g_now_ms;
    memset(&c->e, 0, sizeof(c->e));
    c->rc = https_request(step, method, c->host, path, headers, body, body_len, max_body, &c->r, &c->e);
    g_now_ms = now;   /* https_request "gasta" o RTT no relógio; a hw1 não */
    tag_last(c, 1);
    c->inflight = 1;
    c->ready_at = now + MOCK_RTT_MS;
    (void)e;
    return 0;
}

int https_conn_poll(https_conn_t *c, https_resp_t *r, xc_err_t *e) {
    memset(r, 0, sizeof(*r));
    if (!c->inflight) return HTTPS_EPROTO;
    if (g_now_ms < c->ready_at) return HTTPS_PENDING;
    c->inflight = 0;
    c->st.requests++;
    c->st.nb_requests++;
    *e = c->e;
    *r = c->r;
    memset(&c->r, 0, sizeof(c->r));
    return c->rc;
}

void https_conn_abort(https_conn_t *c) {
    if (c && c->inflight) { https_free(&c->r); c->inflight = 0; }
}
