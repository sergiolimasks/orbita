/*
 * XCloud360 stage 4 — xcw_port.h on POSIX (host x86-64 and ppc32 BE Linux
 * under qemu). Lets the Xbox-side WebRTC code (libpeer port + source/webrtc)
 * run unchanged against a local test peer.
 *
 * Test knobs (environment):
 *   XCW_LOCAL_IP   host candidate address (default 127.0.0.1)
 *   XCW_LOSS       drop this fraction (0..1) of incoming RTP VIDEO packets
 *                  (PT 102) BEFORE SRTP — exercises NACK/retransmission
 *   XCW_LOSS_SEED  PRNG seed for the drops (default 1)
 *   XCW_LOSS_ONCE  1 = never drop the same RTP sequence number twice (the
 *                  NACK retransmission always gets through: isolates "does
 *                  every loss get recovered" from double-loss effects)
 *   XCW_LOSS_AFTER n  start dropping only after n video packets (a lost
 *                  FIRST packet cannot be NACKed: no earlier sequence number)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "xcw_port.h"

#define MAXS 8
static int g_fd[MAXS + 1];
static double g_loss = -1;
static uint32_t g_rng = 1;
static int g_loss_once;
static unsigned long g_loss_after, g_video_seen;
static uint8_t g_dropped_seq[65536 / 8];
static unsigned long g_dropped, g_rx, g_tx;

unsigned long xcw_posix_dropped(void) { return g_dropped; }
unsigned long xcw_posix_rx(void) { return g_rx; }
unsigned long xcw_posix_tx(void) { return g_tx; }

static void loss_init(void) {
    if (g_loss >= 0) return;
    const char *l = getenv("XCW_LOSS");
    g_loss = l ? atof(l) : 0;
    const char *s = getenv("XCW_LOSS_SEED");
    g_rng = s ? (uint32_t)strtoul(s, NULL, 10) : 1;
    g_loss_once = getenv("XCW_LOSS_ONCE") && atoi(getenv("XCW_LOSS_ONCE"));
    g_loss_after = getenv("XCW_LOSS_AFTER") ? strtoul(getenv("XCW_LOSS_AFTER"), NULL, 10) : 0;
    if (!g_rng) g_rng = 1;
}

static double rnd01(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng & 0xffffff) / (double)0x1000000;
}

int xcw_udp_open(uint16_t port, uint16_t *bound_port) {
    loss_init();
    int h;
    for (h = 1; h <= MAXS && g_fd[h] > 0; h++) {}
    if (h > MAXS) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int rcv = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    socklen_t sl = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &sl);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    *bound_port = ntohs(a.sin_port);
    g_fd[h] = fd;
    return h;
}

void xcw_udp_close(int h) {
    if (h >= 1 && h <= MAXS && g_fd[h] > 0) {
        close(g_fd[h]);
        g_fd[h] = 0;
    }
}

int xcw_udp_send(int h, uint32_t ip, uint16_t port, const void *buf, int len) {
    if (h < 1 || h > MAXS || g_fd[h] <= 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    ssize_t w = sendto(g_fd[h], buf, (size_t)len, MSG_DONTWAIT, (struct sockaddr *)&a, sizeof(a));
    if (w < 0) return -1;
    g_tx++;
    return (int)w;
}

int xcw_udp_recv(int h, uint32_t *ip, uint16_t *port, void *buf, int cap) {
    if (h < 1 || h > MAXS || g_fd[h] <= 0) return -1;
    for (;;) {
        struct sockaddr_in a;
        socklen_t sl = sizeof(a);
        ssize_t r = recvfrom(g_fd[h], buf, (size_t)cap, MSG_DONTWAIT, (struct sockaddr *)&a, &sl);
        if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        g_rx++;
        const uint8_t *b = (const uint8_t *)buf;
        /* RTP v2, PT 102 (H.264, cleartext header under SRTP) */
        if (g_loss > 0 && r >= 12 && (b[0] & 0xc0) == 0x80 && (b[1] & 0x7f) == 102) {
            uint16_t seq = (uint16_t)(b[2] << 8 | b[3]);
            int seen = g_dropped_seq[seq >> 3] & (1 << (seq & 7));
            if (++g_video_seen > g_loss_after && !(g_loss_once && seen) && rnd01() < g_loss) {
                g_dropped_seq[seq >> 3] |= (uint8_t)(1 << (seq & 7));
                g_dropped++;
                continue;
            }
        }
        *ip = ntohl(a.sin_addr.s_addr);
        *port = ntohs(a.sin_port);
        return (int)r;
    }
}

int xcw_local_ipv4(uint32_t *ip) {
    const char *s = getenv("XCW_LOCAL_IP");
    struct in_addr a;
    if (!inet_aton(s ? s : "127.0.0.1", &a)) return -1;
    *ip = ntohl(a.s_addr);
    return 0;
}

int xcw_resolve(const char *host, uint32_t *ip) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return -1;
    *ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return 0;
}

uint64_t xcw_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t xcw_now_ms(void) { return xcw_now_us() / 1000u; }

void xcw_idle(int ms) {
    if (ms > 0) usleep((useconds_t)ms * 1000);
}

void xcw_random(void *buf, size_t n) {
    static FILE *f;
    if (!f) f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, n, f) != n) {
        for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = (uint8_t)rand();
    }
}

void xcw_log(const char *line) {
    static uint64_t t0;
    uint64_t now = xcw_now_ms();
    if (!t0) t0 = now;
    fprintf(stderr, "%7.3f %s\n", (now - t0) / 1000.0, line);
}

/* MBEDTLS_ENTROPY_HARDWARE_ALT (shared login/WebRTC mbedTLS config) */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    xcw_random(output, len);
    *olen = len;
    return 0;
}
