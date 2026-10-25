/* XCloud360 v0.17 — ver https.h. v0.19.2: conexão persistente (https_conn_*),
 * handshake passo a passo (CPU medida sem as esperas de rede, retomada de
 * sessão detectada), modo não-bloqueante pra thread de rede. */
/* ssl->state (mbedtls_ssl_handshake_step): saber, logo depois do
 * ServerHello, se o servidor retomou a sessão ou vai mandar o certificado. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "https.h"
#include "http_parse.h"
#include "xc_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/net_sockets.h>   /* só as constantes MBEDTLS_ERR_NET_* */

extern const char xc_ca_roots_pem[];

#define CONNECT_TIMEOUT_MS 10000
#define IO_TIMEOUT_MS      20000
#define DNS_TIMEOUT_MS     6000
#define RBUF               16384

static int                      g_inited = 0;
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt         g_roots;

/* ---- hora pro mbedTLS ---- */
static mbedtls_time_t xc_mbedtls_time(mbedtls_time_t *t) {
    long long now = xc_time_now();
    if (now < 0) now = xc_time_floor();
    if (t) *t = (mbedtls_time_t)now;
    return (mbedtls_time_t)now;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf) {
    time_t t = (time_t)*tt;
    return gmtime_r(&t, tm_buf);
}

static void set_err(xc_err_t *e, int code, int tls, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void set_err(xc_err_t *e, int code, int tls, const char *fmt, ...) {
    if (!e) return;
    e->code = code;
    e->tls = tls;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
}

int https_init(xc_err_t *e) {
    if (g_inited) return 0;
    mbedtls_platform_set_time(xc_mbedtls_time);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_x509_crt_init(&g_roots);

    /* Personalização: material da plataforma (fusíveis, MAC, relógio, seed
     * do pendrive) condensado em SHA-256 — a entropia de verdade vem do
     * mbedtls_hardware_poll (fonte "forte" do entropy.c). */
    uint8_t mat[512], pers[32];
    size_t n = xc_seed_material(mat, sizeof(mat));
    mbedtls_sha256(mat, n, pers, 0);
    mbedtls_platform_zeroize(mat, sizeof(mat));
    int r = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, pers, sizeof(pers));
    mbedtls_platform_zeroize(pers, sizeof(pers));
    if (r != 0) {
        set_err(e, HTTPS_EINIT, r, "entropia/DRBG falhou -0x%04x", -r);
        return HTTPS_EINIT;
    }
    r = mbedtls_x509_crt_parse(&g_roots, (const unsigned char *)xc_ca_roots_pem,
                               strlen(xc_ca_roots_pem) + 1);
    if (r != 0) {
        set_err(e, HTTPS_EINIT, r, "raizes CA invalidas (%d)", r);
        return HTTPS_EINIT;
    }
    int nroots = 0;
    for (mbedtls_x509_crt *c = &g_roots; c && c->raw.len; c = c->next) nroots++;
    xc_log("[tls] mbedTLS pronto, %d raizes CA embutidas", nroots);
    g_inited = 1;
    return 0;
}

int https_random(unsigned char *out, size_t n) {
    if (!g_inited) return -1;
    return mbedtls_ctr_drbg_random(&g_drbg, out, n) == 0 ? 0 : -1;
}

/* ---- transporte ----
 * nb = 0: bloqueante (xcn_send/xcn_recv com timeout; wait_us soma o tempo
 * dentro deles pra a CPU medida não contar a rede). nb = 1: thread de rede,
 * nunca espera (WANT_READ / WANT_WRITE). */
typedef struct { int h; int nb; uint64_t wait_us; } bio_t;

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    bio_t *b = (bio_t *)ctx;
    if (xcn_cancelled()) return MBEDTLS_ERR_NET_SEND_FAILED;
    if (b->nb) {
        int r = xcn_try_send(b->h, buf, len);
        if (r == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (r < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
        return r;
    }
    uint64_t t0 = xcn_now_us();
    int r = xcn_send(b->h, buf, len, IO_TIMEOUT_MS);
    b->wait_us += xcn_now_us() - t0;
    if (r < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    return r;
}

static int bio_recv_nb(void *ctx, unsigned char *buf, size_t len) {
    bio_t *b = (bio_t *)ctx;
    if (xcn_cancelled()) return MBEDTLS_ERR_NET_RECV_FAILED;
    int r = xcn_try_recv(b->h, buf, len);
    if (r == XCN_EAGAIN) return MBEDTLS_ERR_SSL_WANT_READ;
    if (r < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    return r;   /* 0 = EOF */
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout) {
    bio_t *b = (bio_t *)ctx;
    if (xcn_cancelled()) return MBEDTLS_ERR_NET_RECV_FAILED;
    uint64_t t0 = xcn_now_us();
    int r = xcn_recv(b->h, buf, len, timeout ? (int)timeout : IO_TIMEOUT_MS);
    b->wait_us += xcn_now_us() - t0;
    if (r == XCN_ETIMEOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (r < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    return r;   /* 0 = EOF */
}

/* Sem hora conhecida: só as flags de validade são toleradas no handshake
 * (conferidas depois contra o Date). Assinaturas/nome/raiz continuam. */
static int verify_cb(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)crt; (void)depth;
    if (*(int *)p) *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

static long long x509_to_unix(const mbedtls_x509_time *t) {
    char buf[40];
    /* Reaproveita o parser de data HTTP: monta "Xxx, DD Mon YYYY HH:MM:SS GMT". */
    static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    if (t->mon < 1 || t->mon > 12) return -1;
    snprintf(buf, sizeof(buf), "Xxx, %02d %s %04d %02d:%02d:%02d GMT", t->day, mon[t->mon - 1],
             t->year, t->hour, t->min, t->sec);
    return hp_parse_http_date(buf);
}

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    bio_t               bio;
    int                 open;          /* TCP aberto (handle válido) */
    int                 inited;        /* ssl/conf inicializados */
    int                 time_unknown;
} tls_t;

static void tls_free_ex(tls_t *t, int nb) {
    if (t->open) {
        if (!nb) mbedtls_ssl_close_notify(&t->ssl);
        if (nb) xcn_close_nb(t->bio.h);
        else xcn_close(t->bio.h);
        t->open = 0;
    }
    if (t->inited) {
        mbedtls_ssl_free(&t->ssl);
        mbedtls_ssl_config_free(&t->conf);
        t->inited = 0;
    }
}

static void tls_free(tls_t *t) { tls_free_ex(t, 0); }

static void tls_set_bio(tls_t *t, int nb) {
    t->bio.nb = nb;
    if (nb) mbedtls_ssl_set_bio(&t->ssl, &t->bio, bio_send, bio_recv_nb, NULL);
    else mbedtls_ssl_set_bio(&t->ssl, &t->bio, bio_send, NULL, bio_recv_timeout);
}

/* Contexto TLS sobre um TCP já aberto em t->bio.h. `resume`: sessão salva
 * (ou NULL). 0 = ok, senão código mbedTLS. */
static int tls_setup(tls_t *t, const char *host, int nb, const mbedtls_ssl_session *resume) {
    int r;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    t->inited = 1;
    t->time_unknown = xc_time_now() < 0;
    if ((r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) return r;
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &g_roots, NULL);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_read_timeout(&t->conf, IO_TIMEOUT_MS);
    mbedtls_ssl_conf_verify(&t->conf, verify_cb, &t->time_unknown);
    if ((r = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0) return r;
    if ((r = mbedtls_ssl_set_hostname(&t->ssl, host)) != 0) return r;
    if (resume && (r = mbedtls_ssl_set_session(&t->ssl, resume)) != 0) return r;
    tls_set_bio(t, nb);
    return 0;
}

/* Um passo do handshake. Mede a CPU (tempo do passo menos as esperas no
 * bio) e anota se o ServerHello retomou a sessão (-1 = ainda não sabe). */
static int hs_step(tls_t *t, uint64_t *cpu_us, int *resumed) {
    int before = t->ssl.state;
    uint64_t a = xcn_now_us(), w0 = t->bio.wait_us;
    int r = mbedtls_ssl_handshake_step(&t->ssl);
    uint64_t spent = xcn_now_us() - a, waited = t->bio.wait_us - w0;
    *cpu_us += spent > waited ? spent - waited : 0;
    if (before == MBEDTLS_SSL_SERVER_HELLO && t->ssl.state != MBEDTLS_SSL_SERVER_HELLO && *resumed < 0)
        *resumed = t->ssl.state == MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC;
    return r;
}

static int tls_fail_code(tls_t *t, int r, const char *host, const char *step, xc_err_t *e) {
    if (xcn_cancelled()) { set_err(e, HTTPS_ECANCEL, r, "cancelado"); return HTTPS_ECANCEL; }
    uint32_t vf = mbedtls_ssl_get_verify_result(&t->ssl);
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        set_err(e, HTTPS_ECERT, r, "certificado invalido em %s (flags 0x%x)", step, (unsigned)vf);
        return HTTPS_ECERT;
    }
    if (r == MBEDTLS_ERR_SSL_TIMEOUT) {
        set_err(e, HTTPS_ETIMEOUT, r, "TLS timeout em %s", step);
        return HTTPS_ETIMEOUT;
    }
    char es[64];
    mbedtls_strerror(r, es, sizeof(es));
    xc_log("[tls] %s: -0x%04x %s", host, -r, es);
    set_err(e, HTTPS_ETLS, r, "TLS -0x%04x em %s", -r, step);
    return HTTPS_ETLS;
}

static void log_hs(const char *host, unsigned tcp_ms, unsigned hs_ms, uint64_t cpu_us, int resumed, tls_t *t) {
    xc_log("[tls] %s ok: tcp %ums, handshake %ums (cpu %u.%ums, %s), %s%s", host, tcp_ms, hs_ms,
           (unsigned)(cpu_us / 1000), (unsigned)(cpu_us % 1000 / 100), resumed > 0 ? "retomado" : "completo",
           mbedtls_ssl_get_ciphersuite(&t->ssl), t->time_unknown ? " (validade conferida pelo Date)" : "");
}

/* Bloqueante: DNS + TCP + handshake. `ip_io`: IP já resolvido (0 = resolver;
 * devolve o usado). */
static int tls_connect(tls_t *t, const char *host, const char *step, xc_err_t *e,
                       const mbedtls_ssl_session *resume, uint32_t *ip_io, uint64_t *cpu_out, int *resumed_out) {
    memset(t, 0, sizeof(*t));
    uint32_t ip = ip_io ? *ip_io : 0;
    uint64_t t0 = xcn_now_ms();
    int r;
    if (!ip) {
        r = xcn_resolve(host, &ip, DNS_TIMEOUT_MS);
        if (r == XCN_ECANCEL) { set_err(e, HTTPS_ECANCEL, 0, "cancelado"); return HTTPS_ECANCEL; }
        if (r != 0) { set_err(e, HTTPS_EDNS, 0, "DNS falhou em %s (%s)", step, host); return HTTPS_EDNS; }
    }
    r = xcn_connect(ip, 443, CONNECT_TIMEOUT_MS, &t->bio.h);
    if (r == XCN_ECANCEL) { set_err(e, HTTPS_ECANCEL, 0, "cancelado"); return HTTPS_ECANCEL; }
    if (r != 0) {
        set_err(e, HTTPS_ETCP, 0, "TCP falhou em %s (%u.%u.%u.%u:443, %d)", step,
                (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 8 & 255),
                (unsigned)(ip & 255), r);
        return HTTPS_ETCP;
    }
    if (ip_io) *ip_io = ip;
    t->open = 1;
    uint64_t t1 = xcn_now_ms();
    if ((r = tls_setup(t, host, 0, resume)) != 0) return tls_fail_code(t, r, host, step, e);
    uint64_t cpu = 0;
    int resumed = -1;
    while (!mbedtls_ssl_is_handshake_over(&t->ssl)) {
        r = hs_step(t, &cpu, &resumed);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r != 0) return tls_fail_code(t, r, host, step, e);
    }
    uint32_t fl = mbedtls_ssl_get_verify_result(&t->ssl);
    if (fl != 0 && resumed <= 0) {
        set_err(e, HTTPS_ECERT, 0, "certificado invalido em %s (flags 0x%x)", step, (unsigned)fl);
        return HTTPS_ECERT;
    }
    if (cpu_out) *cpu_out = cpu;
    if (resumed_out) *resumed_out = resumed > 0;
    log_hs(host, (unsigned)(t1 - t0), (unsigned)(xcn_now_ms() - t1), cpu, resumed, t);
    return 0;
}

/* Primeira conexão sem hora: o Date da resposta acerta o relógio e a
 * validade da cadeia inteira do servidor é conferida contra ele. */
static int check_chain_against(tls_t *t, long long now, const char *step, xc_err_t *e) {
    const mbedtls_x509_crt *c = mbedtls_ssl_get_peer_cert(&t->ssl);
    if (!c) { set_err(e, HTTPS_ECERT, 0, "sem certificado em %s", step); return HTTPS_ECERT; }
    for (; c && c->raw.len; c = c->next) {
        long long from = x509_to_unix(&c->valid_from), to = x509_to_unix(&c->valid_to);
        if (from < 0 || to < 0 || now < from || now > to) {
            set_err(e, HTTPS_ECERT, 0, "certificado fora da validade em %s", step);
            return HTTPS_ECERT;
        }
    }
    return 0;
}

/* Monta "METHOD path HTTP/1.1 ..." + corpo (malloc; tem token: zeroize). */
static char *build_req(const char *method, const char *host, const char *path, const char *headers,
                       const char *body, size_t body_len, int keep, size_t *out_len) {
    size_t hcap = strlen(path) + strlen(host) + (headers ? strlen(headers) : 0) + 512;
    char *req = (char *)malloc(hcap + body_len);
    if (!req) return NULL;
    int hl = snprintf(req, hcap,
                      "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: XCloud360/0.17\r\n"
                      "Accept-Encoding: identity\r\nConnection: %s\r\n%s",
                      method, path, host, keep ? "keep-alive" : "close", headers ? headers : "");
    if (body || strcmp(method, "POST") == 0)
        hl += snprintf(req + hl, hcap - (size_t)hl, "Content-Length: %u\r\n", (unsigned)body_len);
    hl += snprintf(req + hl, hcap - (size_t)hl, "\r\n");
    if (body_len) memcpy(req + hl, body, body_len);
    *out_len = (size_t)hl + body_len;
    return req;
}

/* Resposta completa do parser -> r (corpo passa a ser do chamador). */
static void take_resp(http_parser_t *hp, https_resp_t *r) {
    r->status = hp->status;
    r->body = (char *)hp->body;
    r->len = hp->body_len;
    if (!r->body) r->body = (char *)calloc(1, 1);   /* sem corpo: string vazia */
    hp->body = NULL;
}

/* "Connection: close" na resposta (ou HTTP/1.0 sem keep-alive). */
static int resp_wants_close(const http_parser_t *hp) {
    const char *h = hp->hdr;
    if (!strncmp(h, "HTTP/1.0", 8)) return 1;
    for (const char *p = h; (p = strchr(p, '\n')) != NULL;) {
        p++;
        if (!strncasecmp(p, "connection:", 11)) {
            const char *v = p + 11;
            while (*v == ' ' || *v == '\t') v++;
            if (!strncasecmp(v, "close", 5)) return 1;
        }
    }
    return 0;
}

int https_request(const char *step, const char *method, const char *host,
                  const char *path, const char *headers, const char *body,
                  size_t body_len, size_t max_body, https_resp_t *r, xc_err_t *e) {
    memset(r, 0, sizeof(*r));
    if (!g_inited) { set_err(e, HTTPS_EINIT, 0, "TLS nao iniciado"); return HTTPS_EINIT; }
    tls_t t;
    int rc = tls_connect(&t, host, step, e, NULL, NULL, NULL, NULL);
    if (rc != 0) { tls_free(&t); return rc; }

    size_t total = 0, off = 0;
    char *req = build_req(method, host, path, headers, body, body_len, 0, &total);
    if (!req) { tls_free(&t); set_err(e, HTTPS_ETOOBIG, 0, "sem memoria"); return HTTPS_ETOOBIG; }
    while (off < total) {
        int w = mbedtls_ssl_write(&t.ssl, (const unsigned char *)req + off, total - off);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) {
            mbedtls_platform_zeroize(req, total);
            free(req);
            tls_free(&t);
            set_err(e, HTTPS_ETLS, w, "TLS -0x%04x enviando em %s", -w, step);
            return HTTPS_ETLS;
        }
        off += (size_t)w;
    }
    mbedtls_platform_zeroize(req, total);   /* tinha token */
    free(req);

    http_parser_t *hp = (http_parser_t *)malloc(sizeof(http_parser_t));
    unsigned char *buf = (unsigned char *)malloc(RBUF);
    if (!hp || !buf) {
        free(hp); free(buf); tls_free(&t);
        set_err(e, HTTPS_ETOOBIG, 0, "sem memoria");
        return HTTPS_ETOOBIG;
    }
    hp_init(hp, max_body);
    int res = HP_OK_MORE;
    for (;;) {
        int n = mbedtls_ssl_read(&t.ssl, buf, RBUF);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { res = hp_eof(hp); break; }
        if (n < 0) {
            /* Alguns servidores fecham o TCP sem close_notify. */
            if (n == MBEDTLS_ERR_NET_RECV_FAILED && !xcn_cancelled()) { res = hp_eof(hp); if (res == HP_DONE) break; }
            hp_free(hp); free(hp); free(buf); tls_free(&t);
            if (xcn_cancelled()) { set_err(e, HTTPS_ECANCEL, n, "cancelado"); return HTTPS_ECANCEL; }
            if (n == MBEDTLS_ERR_SSL_TIMEOUT) { set_err(e, HTTPS_ETIMEOUT, n, "timeout em %s", step); return HTTPS_ETIMEOUT; }
            set_err(e, HTTPS_ETLS, n, "TLS -0x%04x lendo em %s", -n, step);
            return HTTPS_ETLS;
        }
        res = hp_feed(hp, buf, (size_t)n);
        if (res != HP_OK_MORE) break;
    }
    free(buf);
    if (res != HP_DONE) {
        hp_free(hp); free(hp); tls_free(&t);
        if (res == HP_ERR_TOOBIG) { set_err(e, HTTPS_ETOOBIG, 0, "resposta grande demais em %s", step); return HTTPS_ETOOBIG; }
        set_err(e, HTTPS_EPROTO, 0, "resposta HTTP invalida em %s", step);
        return HTTPS_EPROTO;
    }
    if (t.time_unknown) {
        long long d = hp->date[0] ? hp_parse_http_date(hp->date) : -1;
        if (d < xc_time_floor() - 86400) {
            hp_free(hp); free(hp); tls_free(&t);
            set_err(e, HTTPS_ETIME, 0, "sem hora valida (Date) em %s", step);
            return HTTPS_ETIME;
        }
        if ((rc = check_chain_against(&t, d, step, e)) != 0) {
            hp_free(hp); free(hp); tls_free(&t);
            return rc;
        }
        xc_time_set(d, host);
    }
    take_resp(hp, r);
    free(hp);
    tls_free(&t);
    return 0;
}

void https_free(https_resp_t *r) {
    if (r->body) {
        mbedtls_platform_zeroize(r->body, r->len);
        free(r->body);
    }
    r->body = NULL;
    r->len = 0;
}

int https_probe(const char *host, xc_err_t *e) {
    if (!g_inited) return HTTPS_EINIT;
    tls_t t;
    int rc = tls_connect(&t, host, host, e, NULL, NULL, NULL, NULL);
    tls_free(&t);
    return rc;
}

/* ================================================== v0.19.2: keep-alive */

enum { CS_CLOSED = 0, CS_TCP, CS_HS, CS_OPEN };
enum { RQ_IDLE = 0, RQ_CONN, RQ_WRITE, RQ_READ };

struct https_conn {
    char     host[128];
    uint32_t ip;                 /* do 1º connect bloqueante (a hw 1 não resolve DNS) */
    tls_t    t;
    int      cs;
    unsigned served;             /* respostas nesta conexão TCP */
    uint64_t t_cs_ms;            /* início do TCP/handshake não-bloqueante */
    uint64_t hs_cpu_us;
    int      hs_resumed;
    mbedtls_ssl_session saved;
    int      have_saved;
    /* pedido não-bloqueante */
    int      rq;
    char     step[24];
    char    *req;
    size_t   req_len, req_off;
    http_parser_t *hp;
    unsigned char *rbuf;
    size_t   max_body;
    int      got_bytes, reused, retried;
    uint64_t t_rq_ms, rq_cpu_us;
    https_conn_stats_t st;
};

https_conn_t *https_conn_new(const char *host) {
    https_conn_t *c = (https_conn_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    snprintf(c->host, sizeof(c->host), "%s", host ? host : "");
    mbedtls_ssl_session_init(&c->saved);
    return c;
}

const char *https_conn_host(const https_conn_t *c) { return c ? c->host : ""; }
int  https_conn_is_open(const https_conn_t *c) { return c && c->cs == CS_OPEN && c->rq == RQ_IDLE; }
int  https_conn_busy(const https_conn_t *c) { return c && c->rq != RQ_IDLE; }
void https_conn_get_stats(const https_conn_t *c, https_conn_stats_t *s) {
    if (c) *s = c->st;
    else memset(s, 0, sizeof(*s));
}

static void rq_clear(https_conn_t *c) {
    if (c->req) { mbedtls_platform_zeroize(c->req, c->req_len); free(c->req); }
    c->req = NULL;
    c->req_len = c->req_off = 0;
    if (c->hp) { hp_free(c->hp); free(c->hp); }
    c->hp = NULL;
    free(c->rbuf);
    c->rbuf = NULL;
    c->rq = RQ_IDLE;
}

static void conn_close(https_conn_t *c, int nb) {
    if (c->cs == CS_TCP) {
        xcn_close_nb(c->t.bio.h);
        c->t.open = 0;
    }
    tls_free_ex(&c->t, nb);
    c->cs = CS_CLOSED;
    c->served = 0;
}

void https_conn_abort(https_conn_t *c) {
    if (!c) return;
    rq_clear(c);
    conn_close(c, 1);
}

void https_conn_free(https_conn_t *c) {
    if (!c) return;
    rq_clear(c);
    if (c->st.requests || c->st.full || c->st.resumed)
        xc_log("[tls] conexao %s: %u pedidos (%u reaproveitando o TLS), handshakes %u completos + %u retomados,"
               " cpu media %u us/pedido (max %u)", c->host, c->st.requests, c->st.reused, c->st.full,
               c->st.resumed, c->st.requests ? (unsigned)(c->st.req_cpu_us / c->st.requests) : 0,
               c->st.max_req_cpu_us);
    conn_close(c, 0);
    mbedtls_ssl_session_free(&c->saved);
    mbedtls_platform_zeroize(c, sizeof(*c));
    free(c);
}

static void save_session(https_conn_t *c) {
    mbedtls_ssl_session_free(&c->saved);
    mbedtls_ssl_session_init(&c->saved);
    c->have_saved = mbedtls_ssl_get_session(&c->t.ssl, &c->saved) == 0;
}

static void hs_done(https_conn_t *c, uint64_t cpu, int resumed) {
    c->cs = CS_OPEN;
    c->served = 0;
    c->hs_cpu_us = cpu;
    c->hs_resumed = resumed;
    if (resumed) c->st.resumed++;
    else c->st.full++;
    c->st.last_hs_cpu_us = (unsigned)cpu;
    if (cpu > c->st.max_hs_cpu_us) c->st.max_hs_cpu_us = (unsigned)cpu;
    save_session(c);
}

/* Bloqueante: abre (retomando a sessão salva, se houver). */
static int conn_open_blocking(https_conn_t *c, const char *step, xc_err_t *e) {
    conn_close(c, 0);
    uint64_t cpu = 0;
    int resumed = 0;
    int rc = tls_connect(&c->t, c->host, step, e, c->have_saved ? &c->saved : NULL, &c->ip, &cpu, &resumed);
    if (rc != 0) {
        tls_free(&c->t);
        c->cs = CS_CLOSED;
        if (c->have_saved && (rc == HTTPS_ETLS)) {   /* sessão salva recusada de um jeito ruim: esquece */
            mbedtls_ssl_session_free(&c->saved);
            mbedtls_ssl_session_init(&c->saved);
            c->have_saved = 0;
        }
        return rc;
    }
    hs_done(c, cpu, resumed);
    return 0;
}

static void req_stats(https_conn_t *c, uint64_t cpu, int nb) {
    c->st.requests++;
    if (c->served > 0) c->st.reused++;
    c->served++;
    c->st.last_req_cpu_us = (unsigned)cpu;
    if (cpu > c->st.max_req_cpu_us) c->st.max_req_cpu_us = (unsigned)cpu;
    c->st.req_cpu_us += cpu;
    if (nb) c->st.nb_requests++;
    if (c->st.requests == 2 || c->st.requests == 20)
        xc_log("[tls] %s: pedido #%u na mesma conexao (cpu %u us, sem handshake)", c->host, c->st.requests,
               (unsigned)cpu);
}

/* Bloqueante: um pedido na conexão aberta. got_bytes: algum byte de resposta. */
static int conn_once(https_conn_t *c, const char *step, const char *method, const char *path,
                     const char *headers, const char *body, size_t body_len, size_t max_body,
                     https_resp_t *r, xc_err_t *e, int *got_bytes) {
    tls_t *t = &c->t;
    tls_set_bio(t, 0);
    uint64_t a = xcn_now_us(), w0 = t->bio.wait_us;
    size_t total = 0, off = 0;
    char *req = build_req(method, c->host, path, headers, body, body_len, 1, &total);
    if (!req) { set_err(e, HTTPS_ETOOBIG, 0, "sem memoria"); return HTTPS_ETOOBIG; }
    while (off < total) {
        int w = mbedtls_ssl_write(&t->ssl, (const unsigned char *)req + off, total - off);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) {
            mbedtls_platform_zeroize(req, total);
            free(req);
            set_err(e, HTTPS_ETLS, w, "TLS -0x%04x enviando em %s", -w, step);
            return HTTPS_ETLS;
        }
        off += (size_t)w;
    }
    mbedtls_platform_zeroize(req, total);
    free(req);
    http_parser_t *hp = (http_parser_t *)malloc(sizeof(http_parser_t));
    unsigned char *buf = (unsigned char *)malloc(RBUF);
    if (!hp || !buf) { free(hp); free(buf); set_err(e, HTTPS_ETOOBIG, 0, "sem memoria"); return HTTPS_ETOOBIG; }
    hp_init(hp, max_body);
    int res = HP_OK_MORE, eof = 0;
    for (;;) {
        int n = mbedtls_ssl_read(&t->ssl, buf, RBUF);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { eof = 1; res = *got_bytes ? hp_eof(hp) : HP_ERR; break; }
        if (n < 0) {
            if (n == MBEDTLS_ERR_NET_RECV_FAILED && !xcn_cancelled() && *got_bytes) {
                eof = 1;
                res = hp_eof(hp);
                if (res == HP_DONE) break;
            }
            hp_free(hp); free(hp); free(buf);
            if (xcn_cancelled()) { set_err(e, HTTPS_ECANCEL, n, "cancelado"); return HTTPS_ECANCEL; }
            if (n == MBEDTLS_ERR_SSL_TIMEOUT) { set_err(e, HTTPS_ETIMEOUT, n, "timeout em %s", step); return HTTPS_ETIMEOUT; }
            set_err(e, HTTPS_ETLS, n, "TLS -0x%04x lendo em %s", -n, step);
            return HTTPS_ETLS;
        }
        *got_bytes = 1;
        res = hp_feed(hp, buf, (size_t)n);
        if (res != HP_OK_MORE) break;
    }
    free(buf);
    if (res != HP_DONE) {
        hp_free(hp); free(hp);
        if (res == HP_ERR_TOOBIG) { set_err(e, HTTPS_ETOOBIG, 0, "resposta grande demais em %s", step); return HTTPS_ETOOBIG; }
        if (!*got_bytes) { set_err(e, HTTPS_ETLS, 0, "conexao fechada sem resposta em %s", step); return HTTPS_ETLS; }
        set_err(e, HTTPS_EPROTO, 0, "resposta HTTP invalida em %s", step);
        return HTTPS_EPROTO;
    }
    if (t->time_unknown) {
        long long d = hp->date[0] ? hp_parse_http_date(hp->date) : -1;
        int rc = 0;
        if (d < xc_time_floor() - 86400) { set_err(e, HTTPS_ETIME, 0, "sem hora valida (Date) em %s", step); rc = HTTPS_ETIME; }
        else if ((rc = check_chain_against(t, d, step, e)) == 0) { xc_time_set(d, c->host); t->time_unknown = 0; }
        if (rc) { hp_free(hp); free(hp); return rc; }
    }
    int close_after = eof || resp_wants_close(hp);
    take_resp(hp, r);
    free(hp);
    uint64_t spent = xcn_now_us() - a, waited = t->bio.wait_us - w0;
    req_stats(c, spent > waited ? spent - waited : 0, 0);
    if (close_after) conn_close(c, 0);
    return 0;
}

int https_conn_request(https_conn_t *c, const char *step, const char *method, const char *path,
                       const char *headers, const char *body, size_t body_len, size_t max_body,
                       https_resp_t *r, xc_err_t *e) {
    memset(r, 0, sizeof(*r));
    if (!g_inited) { set_err(e, HTTPS_EINIT, 0, "TLS nao iniciado"); return HTTPS_EINIT; }
    if (!c) { set_err(e, HTTPS_EINIT, 0, "sem conexao"); return HTTPS_EINIT; }
    if (c->rq != RQ_IDLE) {   /* não-bloqueante largado no meio (troca de dono): recomeça */
        rq_clear(c);
        conn_close(c, 0);
    }
    for (int pass = 0; pass < 2; pass++) {
        int reused = c->cs == CS_OPEN && c->served > 0;
        if (c->cs != CS_OPEN) {
            int rc = conn_open_blocking(c, step, e);
            if (rc != 0) return rc;
        }
        int got = 0;
        int rc = conn_once(c, step, method, path, headers, body, body_len, max_body, r, e, &got);
        if (rc == 0) return 0;
        conn_close(c, 0);
        if (rc == HTTPS_ECANCEL || rc == HTTPS_ETOOBIG || rc == HTTPS_ETIME || rc == HTTPS_ECERT) return rc;
        /* keep-alive velho que o servidor já tinha fechado: refaz uma vez */
        if (pass == 0 && reused && !got) {
            c->st.retries++;
            xc_log("[tls] %s: conexao reaproveitada caiu (%s), refazendo %s", c->host, e->msg, step);
            continue;
        }
        return rc;
    }
    return HTTPS_ETLS;
}

/* ---- não-bloqueante (thread de rede) ---- */

int https_conn_start(https_conn_t *c, const char *step, const char *method, const char *path,
                     const char *headers, const char *body, size_t body_len, size_t max_body, xc_err_t *e) {
    if (!c || !g_inited) { set_err(e, HTTPS_EINIT, 0, "TLS nao iniciado"); return HTTPS_EINIT; }
    if (c->rq != RQ_IDLE) { set_err(e, HTTPS_EBUSY, 0, "pedido em voo em %s", step); return HTTPS_EBUSY; }
    c->req = build_req(method, c->host, path, headers, body, body_len, 1, &c->req_len);
    c->hp = (http_parser_t *)malloc(sizeof(http_parser_t));
    c->rbuf = (unsigned char *)malloc(RBUF);
    if (!c->req || !c->hp || !c->rbuf) {
        rq_clear(c);
        set_err(e, HTTPS_ETOOBIG, 0, "sem memoria");
        return HTTPS_ETOOBIG;
    }
    hp_init(c->hp, max_body);
    snprintf(c->step, sizeof(c->step), "%s", step);
    c->req_off = 0;
    c->max_body = max_body;
    c->got_bytes = 0;
    c->retried = 0;
    c->rq_cpu_us = 0;
    c->t_rq_ms = xcn_now_ms();
    c->rq = c->cs == CS_OPEN ? RQ_WRITE : RQ_CONN;
    c->reused = c->cs == CS_OPEN && c->served > 0;
    return 0;
}

/* Erro: larga o pedido. ENEEDCPU / transporte derrubam a conexão. */
static int nb_fail(https_conn_t *c, int code, int tls, xc_err_t *e, const char *fmt, const char *what) {
    set_err(e, code, tls, fmt, what);
    if (code == HTTPS_ENEEDCPU) c->st.needcpu++;
    rq_clear(c);
    conn_close(c, 1);
    return code;
}

/* Conexão caiu durante o pedido. Reaproveitada e sem byte de resposta:
 * refaz uma vez (reconectando, se der sem handshake completo). */
static int nb_retry_or_fail(https_conn_t *c, int tls, xc_err_t *e) {
    if (c->reused && !c->got_bytes && !c->retried) {
        c->retried = 1;
        c->st.retries++;
        conn_close(c, 1);
        hp_free(c->hp);
        hp_init(c->hp, c->max_body);
        c->req_off = 0;
        c->rq = RQ_CONN;
        c->reused = 0;
        xc_log("[tls] %s: conexao reaproveitada caiu, refazendo %s", c->host, c->step);
        return HTTPS_PENDING;
    }
    return nb_fail(c, HTTPS_ETLS, tls, e, "TLS caiu em %s", c->step);
}

/* Envio + leitura (só CPU: nada espera). 0 = resposta em r. */
static int nb_io(https_conn_t *c, https_resp_t *r, xc_err_t *e, uint64_t now, int *close_after) {
    if (c->rq == RQ_WRITE) {
        tls_set_bio(&c->t, 1);
        while (c->req_off < c->req_len) {
            int w = mbedtls_ssl_write(&c->t.ssl, (const unsigned char *)c->req + c->req_off, c->req_len - c->req_off);
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (now - c->t_rq_ms > IO_TIMEOUT_MS) return nb_fail(c, HTTPS_ETIMEOUT, 0, e, "timeout enviando %s", c->step);
                return HTTPS_PENDING;
            }
            if (w <= 0) return nb_retry_or_fail(c, w, e);
            c->req_off += (size_t)w;
        }
        c->rq = RQ_READ;
    }
    if (c->rq != RQ_READ) return HTTPS_PENDING;
    tls_set_bio(&c->t, 1);
    int res = HP_OK_MORE, eof = 0;
    for (;;) {
        int n = mbedtls_ssl_read(&c->t.ssl, c->rbuf, RBUF);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (now - c->t_rq_ms > IO_TIMEOUT_MS) return nb_fail(c, HTTPS_ETIMEOUT, 0, e, "timeout em %s", c->step);
            return HTTPS_PENDING;
        }
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_NET_RECV_FAILED) {
            if (!c->got_bytes) return nb_retry_or_fail(c, n, e);
            eof = 1;
            res = hp_eof(c->hp);
            break;
        }
        if (n < 0) return nb_retry_or_fail(c, n, e);
        c->got_bytes = 1;
        res = hp_feed(c->hp, c->rbuf, (size_t)n);
        if (res != HP_OK_MORE) break;
    }
    if (res != HP_DONE) {
        int code = res == HP_ERR_TOOBIG ? HTTPS_ETOOBIG : HTTPS_EPROTO;
        return nb_fail(c, code, 0, e, code == HTTPS_ETOOBIG ? "resposta grande demais em %s"
                                                            : "resposta HTTP invalida em %s", c->step);
    }
    *close_after = eof || resp_wants_close(c->hp);
    take_resp(c->hp, r);
    return 0;
}

static int nb_poll(https_conn_t *c, https_resp_t *r, xc_err_t *e) {
    uint64_t now = xcn_now_ms();
    if (c->rq == RQ_CONN) {
        if (c->cs == CS_CLOSED) {
            /* sem IP (nunca conectou bloqueante) ou sem sessão pra retomar:
             * só um handshake completo resolve — não aqui */
            if (!c->ip) return nb_fail(c, HTTPS_ENEEDCPU, 0, e, "sem IP pra %s (DNS)", c->step);
            if (!c->have_saved) return nb_fail(c, HTTPS_ENEEDCPU, 0, e, "handshake completo em %s", c->step);
            memset(&c->t, 0, sizeof(c->t));
            if (xcn_connect_start(c->ip, 443, &c->t.bio.h) != 0)
                return nb_fail(c, HTTPS_ETCP, 0, e, "TCP sem cano livre em %s", c->step);
            c->t.open = 1;
            c->cs = CS_TCP;
            c->t_cs_ms = now;
            c->hs_cpu_us = 0;
            c->hs_resumed = -1;
        }
        if (c->cs == CS_TCP) {
            int k = xcn_connect_poll(c->t.bio.h);
            if (k == XCN_EAGAIN) {
                if (now - c->t_cs_ms > CONNECT_TIMEOUT_MS) return nb_fail(c, HTTPS_ETCP, 0, e, "TCP timeout em %s", c->step);
                return HTTPS_PENDING;
            }
            if (k != 0) return nb_fail(c, HTTPS_ETCP, 0, e, "TCP falhou em %s", c->step);
            int rr = tls_setup(&c->t, c->host, 1, &c->saved);
            if (rr != 0) return nb_fail(c, HTTPS_ETLS, rr, e, "TLS setup em %s", c->step);
            c->cs = CS_HS;
            c->t_cs_ms = now;
        }
        if (c->cs == CS_HS) {
            while (!mbedtls_ssl_is_handshake_over(&c->t.ssl)) {
                /* o servidor não retomou: o próximo passo seria a cadeia de
                 * certificados + ECDHE (dezenas de ms) — não na hw 1 */
                if (c->t.ssl.state == MBEDTLS_SSL_SERVER_CERTIFICATE)
                    return nb_fail(c, HTTPS_ENEEDCPU, 0, e, "servidor nao retomou a sessao TLS em %s", c->step);
                int rr = hs_step(&c->t, &c->hs_cpu_us, &c->hs_resumed);
                if (rr == MBEDTLS_ERR_SSL_WANT_READ || rr == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    if (now - c->t_cs_ms > IO_TIMEOUT_MS) return nb_fail(c, HTTPS_ETIMEOUT, 0, e, "TLS timeout em %s", c->step);
                    return HTTPS_PENDING;
                }
                if (rr != 0) return nb_fail(c, HTTPS_ETLS, rr, e, "handshake falhou em %s", c->step);
            }
            unsigned ms = (unsigned)(now - c->t_cs_ms);
            hs_done(c, c->hs_cpu_us, 1);
            log_hs(c->host, 0, ms, c->hs_cpu_us, 1, &c->t);
            c->rq = RQ_WRITE;
            c->t_rq_ms = now;
        }
    }
    if (c->rq == RQ_WRITE || c->rq == RQ_READ) {
        uint64_t a = xcn_now_us();
        int close_after = 0;
        int rc = nb_io(c, r, e, now, &close_after);
        c->rq_cpu_us += xcn_now_us() - a;
        if (rc == 0) {
            uint64_t cpu = c->rq_cpu_us;
            rq_clear(c);
            req_stats(c, cpu, 1);
            if (close_after) conn_close(c, 1);
        }
        return rc;
    }
    return HTTPS_PENDING;
}

int https_conn_poll(https_conn_t *c, https_resp_t *r, xc_err_t *e) {
    memset(r, 0, sizeof(*r));
    if (!c || c->rq == RQ_IDLE) { set_err(e, HTTPS_EPROTO, 0, "nenhum pedido em voo"); return HTTPS_EPROTO; }
    uint64_t a = xcn_now_us();
    int rc = nb_poll(c, r, e);
    uint64_t d = xcn_now_us() - a;
    if (d > c->st.max_nb_poll_us) c->st.max_nb_poll_us = (unsigned)d;
    return rc;
}
