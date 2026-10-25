/*
 * XCloud360 stage 5 — host port for the integrated offline E2E:
 *   - xc_port.h + xs_port.h in REAL time (pthreads), in-memory "pendrive"
 *     (TOKEN.TXT = FAKE-REFRESH-0), xc_log -> stderr + a buffer the test
 *     scans for tokens;
 *   - a fake GSSV (same formats as hosttest/session's mock: /v2/login/user,
 *     /active, /play, /state, /connect, passport, /sdp, /ice, /keepalive,
 *     DELETE) whose /sdp and /ice are BRIDGED to a pion peer
 *     (hosttest/webrtc/peer_pion) through files in the current session's
 *     directory: POST /sdp writes offer.sdp, GET /sdp returns answer.sdp
 *     once pion wrote it, GET /ice returns remote.cands (+ end), DELETE
 *     writes recv.done (pion stops and writes pion.json).
 *   - v0.19.2: that GSSV is served over REAL TLS 1.2 (mbedTLS server, a
 *     throw-away RSA-2048 CA + cert for every host the app talks to, made by
 *     run_standalone.sh) on 127.0.0.1:<ephemeral>; xcn_resolve maps every
 *     host to 127.0.0.1 and xcn_connect ignores :443. So the app's own
 *     https.c runs: one-shot requests for the login hosts, the keep-alive
 *     connection for the region host (blocking on the session thread,
 *     non-blocking on "hw1"), session-ID resumption (server cache,
 *     switchable), Connection: close from the server, forced 401s.
 * Nothing leaves the machine (loopback TCP/UDP only). Every id and token is
 * invented.
 */
#define _GNU_SOURCE
#include "port_rt.h"

#include "../../source/login/cjson/cJSON.h"
#include "../../source/login/https.h"
#include "../../source/login/xc_port.h"
#include "../../source/session/xs_port.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REGION "brs.core.gssv-play-prod.xboxlive.com"

gssv_t g_gssv;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;     /* xs_lock */
static pthread_mutex_t g_logmu = PTHREAD_MUTEX_INITIALIZER;
static char *g_logbuf;
static size_t g_loglen, g_logcap;
static char g_token[4096] = "FAKE-REFRESH-0-xxxxxxxxxxxxxxxxxxxx";
static int g_token_len = 35;
static uint64_t g_t0;

static char *sdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }
static char *fmt(const char *f, ...) {
    char b[8192];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return sdup(b);
}

static char *read_file(const char *dir, const char *name) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    b[got] = 0;
    fclose(f);
    return b;
}

static void write_file(const char *dir, const char *name, const char *data) {
    char p[600], t[620];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    snprintf(t, sizeof(t), "%s.tmp", p);
    FILE *f = fopen(t, "wb");
    if (!f) return;
    fputs(data, f);
    fclose(f);
    rename(t, p);
}

/* ------------------------------------------------------------- xc_port */
uint64_t xcn_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
uint64_t xcn_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}
void xcn_sleep_ms(int ms) { if (ms > 0) usleep((useconds_t)ms * 1000); }
int  xcn_cancelled(void) { return 0; }

/* every host is the local TLS server */
static volatile int g_srv_port;
int xcn_resolve(const char *h, uint32_t *ip, int t) {
    (void)h; (void)t;
    *ip = 0x7f000001u;
    return 0;
}

static int sock_to(uint32_t ip, int *fd) {
    (void)ip;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return XCN_ECONN;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_srv_port) };
    sa.sin_addr.s_addr = htonl(0x7f000001u);
    fcntl(s, F_SETFL, O_NONBLOCK);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int r = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) { close(s); return XCN_ECONN; }
    *fd = s;
    return 0;
}

int xcn_connect(uint32_t ip, int port, int timeout_ms, int *h) {
    (void)port;
    int s;
    if (sock_to(ip, &s)) return XCN_ECONN;
    struct pollfd pf = { .fd = s, .events = POLLOUT };
    if (poll(&pf, 1, timeout_ms) <= 0) { close(s); return XCN_ETIMEOUT; }
    int err = 0;
    socklen_t el = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { close(s); return XCN_ECONN; }
    *h = s;
    return 0;
}

int xcn_send(int h, const uint8_t *b, size_t n, int timeout_ms) {
    size_t off = 0;
    while (off < n) {
        struct pollfd pf = { .fd = h, .events = POLLOUT };
        if (poll(&pf, 1, timeout_ms) <= 0) return XCN_ETIMEOUT;
        ssize_t w = send(h, b + off, n - off, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EAGAIN) continue; return XCN_EIO; }
        off += (size_t)w;
    }
    return (int)n;
}

int xcn_recv(int h, uint8_t *b, size_t n, int timeout_ms) {
    struct pollfd pf = { .fd = h, .events = POLLIN };
    int p = poll(&pf, 1, timeout_ms);
    if (p == 0) return XCN_ETIMEOUT;
    if (p < 0) return XCN_EIO;
    ssize_t r = recv(h, b, n, 0);
    if (r < 0) return errno == EAGAIN ? XCN_ETIMEOUT : XCN_EIO;
    return (int)r;
}

void xcn_close(int h) { close(h); }

int xcn_connect_start(uint32_t ip, int port, int *h) { (void)port; return sock_to(ip, h); }
int xcn_connect_poll(int h) {
    struct pollfd pf = { .fd = h, .events = POLLOUT };
    int p = poll(&pf, 1, 0);
    if (p == 0) return XCN_EAGAIN;
    int err = 0;
    socklen_t el = sizeof(err);
    getsockopt(h, SOL_SOCKET, SO_ERROR, &err, &el);
    return err ? XCN_ECONN : 0;
}
int xcn_try_send(int h, const uint8_t *b, size_t n) {
    ssize_t w = send(h, b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (w < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : XCN_EIO;
    return (int)w;
}
int xcn_try_recv(int h, uint8_t *b, size_t n) {
    ssize_t r = recv(h, b, n, MSG_DONTWAIT);
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? XCN_EAGAIN : XCN_EIO;
    return (int)r;
}
void xcn_close_nb(int h) { close(h); }

/* (mbedtls_hardware_poll: hosttest/webrtc/xc_udp_posix.c) */
size_t xc_seed_material(uint8_t *out, size_t cap) {
    long long v[2] = { (long long)time(NULL), (long long)getpid() };
    size_t n = sizeof(v) < cap ? sizeof(v) : cap;
    memcpy(out, v, n);
    return n;
}

void rt_log_line(const char *line) {
    if (!g_t0) g_t0 = xcn_now_ms();
    size_t n = strlen(line);
    pthread_mutex_lock(&g_logmu);
    if (g_loglen + n + 2 > g_logcap) {
        g_logcap = (g_logcap + n + 2) * 2;
        g_logbuf = realloc(g_logbuf, g_logcap);
    }
    memcpy(g_logbuf + g_loglen, line, n);
    g_loglen += n;
    g_logbuf[g_loglen++] = '\n';
    g_logbuf[g_loglen] = 0;
    fprintf(stderr, "%7.3f LOG %s\n", (xcn_now_ms() - g_t0) / 1000.0, line);
    pthread_mutex_unlock(&g_logmu);
}

void xc_log(const char *f, ...) {
    char line[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(line, sizeof(line), f, ap);
    va_end(ap);
    rt_log_line(line);
}

const char *rt_log_text(void) { return g_logbuf ? g_logbuf : ""; }

long long xc_time_now(void) { return (long long)time(NULL); }
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
int xc_store_remove(const char *name) { (void)name; g_token_len = -1; return 0; }
const char *rt_token(void) { return g_token; }

void xs_lock(void) { pthread_mutex_lock(&g_mu); }
void xs_unlock(void) { pthread_mutex_unlock(&g_mu); }

/* ---------------------------------------------------------- fake GSSV */
static char *exchange(cJSON *inner) {
    char *in = cJSON_PrintUnformatted(inner);
    cJSON_Delete(inner);
    cJSON *o = cJSON_CreateObject();
    cJSON *ed = cJSON_AddObjectToObject(o, "errorDetails");
    cJSON_AddNullToObject(ed, "code");
    cJSON_AddNullToObject(ed, "message");
    cJSON_AddStringToObject(o, "exchangeResponse", in);
    free(in);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static int sess_of(const char *path, const char **suf) {
    for (int i = 0; i < g_gssv.nsess; i++) {
        size_t l = strlen(g_gssv.s[i].path);
        if (!strncmp(path, g_gssv.s[i].path, l) && (path[l] == 0 || path[l] == '/')) {
            *suf = path + l;
            return i;
        }
    }
    return -1;
}

/* /state script per session: Provisioning x2, WaitingForResources x3 (the
 * 2nd and 3rd with a queue position, to exercise the QUEUE line), then
 * ReadyToConnect (-> /connect) and Provisioned forever. */
static const char *state_body(gssv_sess_t *s) {
    int k = s->state_polls++;
    if (k < 2) return "{\"errorDetails\":null,\"serverPlatform\":\"XBOX\",\"state\":\"Provisioning\"}";
    if (k < 3) return "{\"errorDetails\":null,\"serverPlatform\":\"XBOX\",\"state\":\"WaitingForResources\"}";
    if (k < 5 + g_gssv.extra_queue_polls)
        return "{\"errorDetails\":null,\"queueInfo\":{\"positionInQueue\":3,\"estimatedTotalWaitTimeInSeconds\":45},"
               "\"state\":\"WaitingForResources\"}";
    if (k < 6 + g_gssv.extra_queue_polls) return "{\"errorDetails\":null,\"serverPlatform\":\"XBOX\",\"state\":\"ReadyToConnect\"}";
    return "{\"errorDetails\":null,\"serverPlatform\":\"XBOX\",\"state\":\"Provisioned\"}";
}

static int handle(const char *method, const char *host, const char *path, const char *hdr, const char *body,
                  int *status, char **out) {
    *status = 404;
    *out = NULL;
    if (!strcmp(host, "login.microsoftonline.com")) {
        g_gssv.n_refresh++;
        int v = -1;
        const char *rt = body ? strstr(body, "refresh_token=FAKE-REFRESH-") : NULL;
        if (!rt) { *status = 400; return 0; }
        sscanf(rt, "refresh_token=FAKE-REFRESH-%d", &v);
        *status = 200;
        *out = fmt("{\"token_type\":\"Bearer\",\"expires_in\":3600,\"access_token\":\"FAKE-ACCESS\","
                   "\"refresh_token\":\"FAKE-REFRESH-%d-xxxxxxxxxxxxxxxxxxxx\"}", v + 1);
        return 0;
    }
    if (!strcmp(host, "user.auth.xboxlive.com")) {
        *status = 200;
        *out = sdup("{\"Token\":\"FAKE-USERTOKEN\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"1234\"}]}}");
        return 0;
    }
    if (!strcmp(host, "xsts.auth.xboxlive.com")) {
        *status = 200;
        *out = sdup("{\"Token\":\"FAKE-XSTS-GSSV\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"1234\"}]}}");
        return 0;
    }
    if (strstr(host, ".gssv-play-prod.xboxlive.com") && !strcmp(path, "/v2/login/user")) {
        g_gssv.n_login++;
        g_gssv.gs_valid = g_gssv.n_login;
        *status = 200;
        *out = fmt("{\"offeringSettings\":{\"regions\":[{\"name\":\"BrazilSouth\",\"baseUri\":\"https://" REGION
                   "\",\"isDefault\":true}]},\"gsToken\":\"GS-%d\",\"durationInSeconds\":14400}", g_gssv.n_login);
        return 0;
    }
    if (!strcmp(host, "login.live.com")) {
        g_gssv.n_passport++;
        *status = 200;
        *out = sdup("{\"token_type\":\"bearer\",\"expires_in\":86400,\"access_token\":\"PASSPORT-FAKE\"}");
        return 0;
    }
    if (strcmp(host, REGION) != 0) return 0;
    {
        const char *a = strstr(hdr ? hdr : "", "Authorization: Bearer GS-");
        int v = -1;
        if (!a || sscanf(a, "Authorization: Bearer GS-%d", &v) != 1 || v != g_gssv.gs_valid) {
            g_gssv.n_401++;
            *status = 401;
            return 0;
        }
    }

    if (!strcmp(path, "/v5/sessions/cloud/active")) { g_gssv.n_active++; *status = 404; return 0; }
    if (!strcmp(path, "/v5/sessions/cloud/play") && !strcmp(method, "POST")) {
        g_gssv.n_play++;
        if (g_gssv.nsess >= GSSV_MAX_SESS || !g_gssv.next_dir[0]) { *status = 503; return 0; }
        gssv_sess_t *s = &g_gssv.s[g_gssv.nsess++];
        memset(s, 0, sizeof(*s));
        snprintf(s->path, sizeof(s->path), "/v5/sessions/cloud/1A2B3C4D-5E6F-4A7B-8C9D-0000000000%02d", g_gssv.nsess);
        snprintf(s->dir, sizeof(s->dir), "%s", g_gssv.next_dir);
        cJSON *j = body ? cJSON_Parse(body) : NULL;
        cJSON *t = j ? cJSON_GetObjectItem(j, "titleId") : NULL;
        snprintf(s->title, sizeof(s->title), "%s", cJSON_IsString(t) ? t->valuestring : "?");
        cJSON_Delete(j);
        *status = 200;
        *out = fmt("{\"sessionPath\":\"%s\",\"sessionId\":\"%s\",\"state\":\"Provisioning\"}", s->path + 1,
                   strrchr(s->path, '/') + 1);
        return 0;
    }
    if (!strncmp(path, "/v1/waittime/", 13)) {
        *status = 200;
        *out = sdup("{\"estimatedTotalWaitTimeInSeconds\":45}");
        return 0;
    }
    const char *suf = NULL;
    int si = sess_of(path, &suf);
    if (si < 0) return 0;
    gssv_sess_t *s = &g_gssv.s[si];
    if (!strcmp(method, "DELETE") && !*suf) {
        s->n_delete++;
        write_file(s->dir, "recv.done", "1\n");   /* pion: stop media, write pion.json */
        *status = 202;
        return 0;
    }
    if (!strcmp(suf, "/state")) { *status = 200; *out = sdup(state_body(s)); return 0; }
    if (!strcmp(suf, "/connect")) { s->n_connect++; *status = 200; return 0; }
    if (!strcmp(suf, "/sdp") && !strcmp(method, "POST")) {
        s->n_sdp++;
        cJSON *j = body ? cJSON_Parse(body) : NULL;
        cJSON *sdp = j ? cJSON_GetObjectItem(j, "sdp") : NULL;
        if (cJSON_IsString(sdp)) {
            write_file(s->dir, "offer.sdp", sdp->valuestring);   /* -> pion */
            s->offer_written = 1;
        }
        cJSON_Delete(j);
        *status = 202;
        return 0;
    }
    if (!strcmp(suf, "/sdp")) {
        char *a = s->offer_written ? read_file(s->dir, "answer.sdp") : NULL;
        if (!a) { *status = 204; return 0; }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "chat", 1);
        cJSON_AddNumberToObject(o, "control", 2);
        cJSON_AddNumberToObject(o, "input", 9);
        cJSON_AddNumberToObject(o, "message", 1);
        cJSON_AddStringToObject(o, "messageType", "answer");
        cJSON_AddStringToObject(o, "sdp", a);
        cJSON_AddStringToObject(o, "status", "success");
        free(a);
        *status = 200;
        *out = exchange(o);
        return 0;
    }
    if (!strcmp(suf, "/ice") && !strcmp(method, "POST")) {
        s->n_ice_post++;
        *status = 200;
        *out = sdup("{\"errorDetails\":{\"code\":null,\"message\":null},\"exchangeResponse\":null}");
        return 0;
    }
    if (!strcmp(suf, "/ice")) {
        s->n_ice_get++;
        char *rc = read_file(s->dir, "remote.cands");
        if (!rc || s->ice_delivered) { free(rc); *status = 204; return 0; }
        s->ice_delivered = 1;
        cJSON *arr = cJSON_CreateArray();
        for (char *l = strtok(rc, "\n"); l; l = strtok(NULL, "\n")) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "candidate", l);
            cJSON_AddStringToObject(c, "messageType", "iceCandidate");
            cJSON_AddStringToObject(c, "sdpMLineIndex", "0");
            cJSON_AddStringToObject(c, "sdpMid", "0");
            cJSON_AddItemToArray(arr, c);
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "candidate", "a=end-of-candidates");
        cJSON_AddItemToArray(arr, e);
        free(rc);
        *status = 200;
        *out = exchange(arr);
        return 0;
    }
    if (!strcmp(suf, "/keepalive")) {
        if (g_gssv.revoke_next_ka) {   /* gsToken "expired" on the server */
            g_gssv.revoke_next_ka = 0;
            g_gssv.gs_valid = -1;
            g_gssv.n_401++;
            *status = 401;
            return 0;
        }
        s->n_ka++;
        *status = 200;
        *out = sdup("{\"alertTimeInMs\":600000,\"keepAliveInSeconds\":300}");
        return 0;
    }
    return 0;
}

/* ------------------------------------------------- local TLS server */
static mbedtls_x509_crt g_scert;
static mbedtls_pk_context g_skey;
static pthread_mutex_t g_hsmu = PTHREAD_MUTEX_INITIALIZER;   /* RSA key ops (no MBEDTLS_THREADING) */
static int g_lfd = -1;

/* session cache (session-ID resumption), switchable by the test */
#define SCACHE 64
static struct { unsigned char id[32]; size_t idlen; unsigned char buf[4096]; size_t len; } g_sc[SCACHE];
static int g_sc_n, g_sc_next;
static pthread_mutex_t g_scmu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_hit;

static int sc_get(void *d, unsigned char const *id, size_t idlen, mbedtls_ssl_session *sess) {
    (void)d;
    int r = 1;
    pthread_mutex_lock(&g_gssv.mu);
    int ok = g_gssv.resume_ok;
    pthread_mutex_unlock(&g_gssv.mu);
    if (!ok) return 1;
    pthread_mutex_lock(&g_scmu);
    for (int i = 0; i < g_sc_n; i++)
        if (g_sc[i].idlen == idlen && !memcmp(g_sc[i].id, id, idlen)) {
            r = mbedtls_ssl_session_load(sess, g_sc[i].buf, g_sc[i].len) == 0 ? 0 : 1;
            break;
        }
    pthread_mutex_unlock(&g_scmu);
    if (r == 0) t_hit = 1;
    else fprintf(stderr, "        [tls-srv] session cache miss (%d entries)\n", g_sc_n);
    return r;
}

static int sc_set(void *d, unsigned char const *id, size_t idlen, const mbedtls_ssl_session *sess) {
    (void)d;
    if (idlen > 32) return 1;
    pthread_mutex_lock(&g_scmu);
    int k = g_sc_next++ % SCACHE;   /* round robin: deterministic */
    if (g_sc_n < SCACHE) g_sc_n++;
    size_t len = 0;
    int r = mbedtls_ssl_session_save(sess, g_sc[k].buf, sizeof(g_sc[k].buf), &len);
    if (r == 0) { memcpy(g_sc[k].id, id, idlen); g_sc[k].idlen = idlen; g_sc[k].len = len; }
    pthread_mutex_unlock(&g_scmu);
    return r == 0 ? 0 : 1;
}

typedef struct { int fd; char sni[128]; } sconn_t;

static int sni_cb(void *p, mbedtls_ssl_context *ssl, const unsigned char *name, size_t len) {
    (void)ssl;
    sconn_t *c = (sconn_t *)p;
    if (len >= sizeof(c->sni)) len = sizeof(c->sni) - 1;
    memcpy(c->sni, name, len);
    c->sni[len] = 0;
    return 0;
}

static int s_send(void *ctx, const unsigned char *b, size_t n) {
    ssize_t w = send(*(int *)ctx, b, n, MSG_NOSIGNAL);
    return w < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)w;
}
static int s_recv(void *ctx, unsigned char *b, size_t n) {
    ssize_t r = recv(*(int *)ctx, b, n, 0);
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)r;
}

static int s_rng(void *p, unsigned char *out, size_t n) {
    (void)p;
    return getrandom(out, n, 0) == (ssize_t)n ? 0 : -1;
}

static const char *reason(int st) {
    switch (st) {
    case 200: return "OK"; case 202: return "Accepted"; case 204: return "No Content";
    case 401: return "Unauthorized"; case 404: return "Not Found"; case 400: return "Bad Request";
    }
    return "Other";
}

/* One HTTP/1.1 keep-alive connection. */
static void *sconn_main(void *arg) {
    sconn_t *c = (sconn_t *)arg;
    struct timeval tv = { 30, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    int ok = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (ok) {
        mbedtls_ssl_conf_rng(&conf, s_rng, NULL);
        mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_own_cert(&conf, &g_scert, &g_skey);
        mbedtls_ssl_conf_session_cache(&conf, NULL, sc_get, sc_set);
        mbedtls_ssl_conf_sni(&conf, sni_cb, c);
        ok = mbedtls_ssl_setup(&ssl, &conf) == 0;
    }
    mbedtls_ssl_set_bio(&ssl, &c->fd, s_send, s_recv, NULL);
    t_hit = 0;
    if (ok) {
        pthread_mutex_lock(&g_hsmu);
        int r;
        while ((r = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {}
        pthread_mutex_unlock(&g_hsmu);
        ok = r == 0;
        if (!ok) {
            char es[96];
            mbedtls_strerror(r, es, sizeof(es));
            fprintf(stderr, "        [tls-srv] handshake %s: -0x%04x %s\n", c->sni, -r, es);
        }
    }
    int region = !strcmp(c->sni, "brs.core.gssv-play-prod.xboxlive.com");
    if (ok) {
        pthread_mutex_lock(&g_gssv.mu);
        if (region) {
            g_gssv.region_conns++;
            if (t_hit) g_gssv.region_resumed++;
            else g_gssv.region_full++;
        } else {
            if (t_hit) g_gssv.other_resumed++;
            else g_gssv.other_full++;
        }
        pthread_mutex_unlock(&g_gssv.mu);
    }
    char *in = malloc(1 << 20);
    size_t have = 0;
    int served = 0;
    while (ok) {
        /* headers */
        char *eoh = NULL;
        while (!(eoh = (have ? memmem(in, have, "\r\n\r\n", 4) : NULL))) {
            if (have >= (1 << 20) - 1) { ok = 0; break; }
            int n = mbedtls_ssl_read(&ssl, (unsigned char *)in + have, (1 << 20) - 1 - have);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) { ok = 0; break; }
            have += (size_t)n;
        }
        if (!ok) break;
        size_t hlen = (size_t)(eoh - in) + 4;
        char *hdr = malloc(hlen + 1);
        memcpy(hdr, in, hlen);
        hdr[hlen] = 0;
        char method[16] = "", path[600] = "", host[160] = "";
        sscanf(hdr, "%15s %599s", method, path);
        const char *hh = strcasestr(hdr, "\r\nHost: ");
        if (hh) sscanf(hh + 8, "%159[^\r\n]", host);
        long cl = 0;
        const char *clh = strcasestr(hdr, "\r\nContent-Length: ");
        if (clh) cl = atol(clh + 18);
        while (have < hlen + (size_t)cl) {
            int n = mbedtls_ssl_read(&ssl, (unsigned char *)in + have, (1 << 20) - 1 - have);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) { ok = 0; break; }
            have += (size_t)n;
        }
        if (!ok) { free(hdr); break; }
        char *body = NULL;
        if (clh || !strcmp(method, "POST")) {
            body = malloc((size_t)cl + 1);
            memcpy(body, in + hlen, (size_t)cl);
            body[cl] = 0;
        }
        memmove(in, in + hlen + (size_t)cl, have - hlen - (size_t)cl);
        have -= hlen + (size_t)cl;

        usleep(4000);   /* ~ a LAN round trip */
        int status = 500;
        char *resp = NULL;
        int close_now = 0;
        pthread_mutex_lock(&g_gssv.mu);
        g_gssv.nreq++;
        handle(method, host, path, hdr, body, &status, &resp);
        if (region) {
            g_gssv.region_reqs++;
            if (served + 1 > g_gssv.region_max_reqs_per_conn) g_gssv.region_max_reqs_per_conn = served + 1;
            if (strstr(path, "/keepalive") && served > 0) g_gssv.region_ka_on_reused++;
            if (g_gssv.close_next_region) { g_gssv.close_next_region = 0; close_now = 1; }
        }
        pthread_mutex_unlock(&g_gssv.mu);
        served++;
        free(hdr);
        free(body);
        size_t bl = resp ? strlen(resp) : 0;
        char head[512];
        char date[64];
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        int hl = snprintf(head, sizeof(head),
                          "HTTP/1.1 %d %s\r\nDate: %s\r\nContent-Type: application/json\r\n"
                          "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
                          status, reason(status), date, bl, close_now ? "close" : "keep-alive");
        size_t tot = (size_t)hl + bl;
        char *out = malloc(tot);
        memcpy(out, head, (size_t)hl);
        if (bl) memcpy(out + hl, resp, bl);
        free(resp);
        size_t off = 0;
        while (off < tot) {
            int w = mbedtls_ssl_write(&ssl, (const unsigned char *)out + off, tot - off);
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (w <= 0) { ok = 0; break; }
            off += (size_t)w;
        }
        free(out);
        if (close_now) {
            fprintf(stderr, "        [tls-srv] %s: Connection: close after %d requests\n", c->sni, served);
            break;
        }
    }
    free(in);
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    close(c->fd);
    free(c);
    return NULL;
}

static void *accept_main(void *a) {
    (void)a;
    for (;;) {
        int fd = accept(g_lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        sconn_t *c = calloc(1, sizeof(*c));
        c->fd = fd;
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &at, sconn_main, c);
        pthread_attr_destroy(&at);
    }
    return NULL;
}

int rt_tls_server_start(const char *cert_pem, const char *key_pem) {
    mbedtls_x509_crt_init(&g_scert);
    mbedtls_pk_init(&g_skey);
    {   /* MBEDTLS_FS_IO is off: read the PEM ourselves */
        FILE *f = fopen(cert_pem, "rb");
        if (!f) return -1;
        static char b[16384];
        size_t n = fread(b, 1, sizeof(b) - 1, f);
        fclose(f);
        b[n] = 0;
        if (mbedtls_x509_crt_parse(&g_scert, (const unsigned char *)b, n + 1) != 0) return -1;
    }
    {
        FILE *f = fopen(key_pem, "rb");
        if (!f) return -1;
        static char b[16384];
        size_t n = fread(b, 1, sizeof(b) - 1, f);
        fclose(f);
        b[n] = 0;
        if (mbedtls_pk_parse_key(&g_skey, (const unsigned char *)b, n + 1, NULL, 0, s_rng, NULL) != 0) return -1;
    }
    g_gssv.resume_ok = 1;
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = 0 };
    sa.sin_addr.s_addr = htonl(0x7f000001u);
    if (bind(g_lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(g_lfd, 16) != 0) return -1;
    socklen_t sl = sizeof(sa);
    getsockname(g_lfd, (struct sockaddr *)&sa, &sl);
    g_srv_port = ntohs(sa.sin_port);
    pthread_t th;
    pthread_create(&th, NULL, accept_main, NULL);
    pthread_detach(th);
    fprintf(stderr, "        [tls-srv] fake GSSV on 127.0.0.1:%d (TLS 1.2, RSA-2048 test CA)\n", g_srv_port);
    return 0;
}
