/*
 * XCloud360 v0.17 — xc_port.h no Linux (teste do host). Sockets POSIX no
 * lugar do net_pipe do Xbox; o DNS usa o MESMO dns_msg.c do Xbox, por UDP,
 * no servidor do /etc/resolv.conf (ou XC_DNS=a.b.c.d).
 * Env: XC_TIME_UNKNOWN=1 simula o Xbox sem hora (testa o bootstrap pelo Date);
 *      XC_STORE=<dir> (padrão /tmp/xc360-store) faz o papel do pendrive.
 */
#define _GNU_SOURCE
#include "../../source/login/xc_port.h"
#include "../../source/login/dns_msg.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t xcn_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void xcn_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static int (*g_cancel_hook)(void) = 0;
void xcn_set_cancel_hook(int (*fn)(void)) { g_cancel_hook = fn; }
int  xcn_cancelled(void) { return g_cancel_hook ? g_cancel_hook() : 0; }

void xc_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("LOG ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

static long long g_time_off = 0;
static int g_time_known = -1;

long long xc_time_floor(void) {
#ifdef XC_BUILD_EPOCH
    return XC_BUILD_EPOCH;
#else
    return 1790000000LL;
#endif
}

long long xc_time_now(void) {
    if (g_time_known < 0) g_time_known = getenv("XC_TIME_UNKNOWN") ? 0 : 1;
    if (!g_time_known) return -1;
    return (long long)time(NULL) + g_time_off;
}

void xc_time_set(long long unix_now, const char *source) {
    g_time_off = unix_now - (long long)time(NULL);
    g_time_known = 1;
    xc_log("[time] hora acertada pelo Date de %s (diferenca pro relogio do host: %llds)", source,
           g_time_off);
}

static const char *store_dir(void) {
    const char *d = getenv("XC_STORE");
    return d ? d : "/tmp/xc360-store";
}

int xc_store_load(const char *name, void *buf, size_t cap) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", store_dir(), name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (int)n;
}

int xc_store_save(const char *name, const void *buf, size_t n) {
    char p[512];
    mkdir(store_dir(), 0700);
    snprintf(p, sizeof(p), "%s/%s", store_dir(), name);
    {   /* "COVERS/X.JPG": cria a subpasta, como o xc_port_xenon.c */
        char *sl = strrchr(p, '/');
        if (sl && strchr(name, '/')) { *sl = 0; mkdir(p, 0700); *sl = '/'; }
    }
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

int xc_store_remove(const char *name) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", store_dir(), name);
    return unlink(p);
}

size_t xc_seed_material(uint8_t *out, size_t cap) {
    long long v[3] = { (long long)time(NULL), (long long)getpid(), (long long)xcn_now_ms() };
    size_t n = sizeof(v) < cap ? sizeof(v) : cap;
    memcpy(out, v, n);
    return n;
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    ssize_t r = getrandom(output, len, 0);
    if (r < 0) return -1;
    *olen = (size_t)r;
    return 0;
}

/* ---- DNS (mesmo dns_msg.c do Xbox) ---- */
static uint32_t dns_server(void) {
    const char *env = getenv("XC_DNS");
    char line[256];
    struct in_addr a;
    if (env && inet_aton(env, &a)) return ntohl(a.s_addr);
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char ip[64];
            if (sscanf(line, "nameserver %63s", ip) == 1 && inet_aton(ip, &a)) {
                fclose(f);
                return ntohl(a.s_addr);
            }
        }
        fclose(f);
    }
    return 0x01010101;
}

int xcn_resolve(const char *host, uint32_t *ip, int timeout_ms) {
    struct in_addr a;
    if (inet_aton(host, &a)) { *ip = ntohl(a.s_addr); return 0; }
    uint8_t q[300], resp[1500];
    uint16_t id = (uint16_t)(xcn_now_ms() ^ (uint64_t)getpid());
    int ql = dns_build_query(q, sizeof(q), id, host);
    if (ql < 0) return XCN_EDNS;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return XCN_EDNS;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53) };
    sa.sin_addr.s_addr = htonl(dns_server());
    int rc = XCN_EDNS;
    uint64_t end = xcn_now_ms() + (uint64_t)timeout_ms;
    for (int tries = 0; tries < 3 && rc != 0 && xcn_now_ms() < end; tries++) {
        sendto(s, q, (size_t)ql, 0, (struct sockaddr *)&sa, sizeof(sa));
        struct pollfd pf = { .fd = s, .events = POLLIN };
        while (poll(&pf, 1, 1500) > 0) {
            ssize_t n = recv(s, resp, sizeof(resp), 0);
            if (n <= 0) break;
            int rcode = 0;
            int pr = dns_parse_response(resp, (size_t)n, id, ip, &rcode);
            if (pr == 0) { rc = 0; break; }
            if (pr == 1) { rc = XCN_EDNS; tries = 99; break; }
        }
    }
    close(s);
    return rc;
}

/* ---- TCP ---- */
int xcn_connect(uint32_t ip, int port, int timeout_ms, int *h) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return XCN_ECONN;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    sa.sin_addr.s_addr = htonl(ip);
    fcntl(s, F_SETFL, O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) { close(s); return XCN_ECONN; }
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

/* ---- v0.19.2: não-bloqueantes (thread de rede no Xbox) ---- */
uint64_t xcn_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

int xcn_connect_start(uint32_t ip, int port, int *h) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return XCN_ECONN;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    sa.sin_addr.s_addr = htonl(ip);
    fcntl(s, F_SETFL, O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) { close(s); return XCN_ECONN; }
    *h = s;
    return 0;
}

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
