/*
 * XCloud360 stage 4 — WebRTC CPU budget benchmark.
 *
 * Same source for the Xbox (variants/webrtc-bench.elf32, boot from XeLL,
 * results on screen) and the host (hosttest, for the scaling factor). Uses
 * the REAL libraries from deps/webrtc-xenon (libsrtp, libpeer's DtlsSrtp on
 * mbedTLS, usrsctp, the C jitter buffer) — no network needed:
 *
 *   1. SRTP AES_CM_128_HMAC_SHA1_80 protect/unprotect, 1200-byte RTP packets
 *      -> us/packet and % of one hardware thread at 4 Mbps (417 pkt/s);
 *      on the Xbox also with the SMT sibling thread busy (hw0 + hw1 share
 *      one in-order core, which is exactly the hw1 network-thread case).
 *   2. Jitter buffer + FU-A depacketization per RTP packet.
 *   3. DTLS 1.2 handshake, libpeer DtlsSrtp client vs server IN MEMORY
 *      (ECDSA P-256 self-signed certs, ECDHE, use_srtp + key export):
 *      cert/keygen time, client CPU, longest single step (= how long the
 *      thread running WebRTC is blocked), then datachannel-sized records.
 *   4. usrsctp (AF_CONN, no threads) message cost: two associations in
 *      memory, 38-byte input reports and 200-byte JSON messages.
 *   5. ECC primitives + one RSA-1024 keygen (libpeer's default cert type,
 *      why the Xbox build uses ECDSA).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <arpa/inet.h>

#include "dtls_srtp.h"
#include "mbedtls/aes.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/md.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ssl.h"
#include "srtp2/srtp.h"
#include "usrsctp.h"
#include "xc_jitter.h"
#include "xcw_port.h"

#ifndef XCW_SRTP_CRYPTO
#define XCW_SRTP_CRYPTO "?"
#endif

#ifdef __XENON__
#include <console/console.h>
#include <xenon_soc/xenon_power.h>
#include <xenos/xenos.h>
#define PLATFORM "Xenon (Xbox 360, 3.2 GHz PPE, hw0)"
#else
#define PLATFORM "host"
#endif

static char g_report[16384];
static size_t g_rlen;

static void out(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s", line);
    fflush(stdout);
    size_t l = strlen(line);
    if (g_rlen + l < sizeof(g_report)) {
        memcpy(g_report + g_rlen, line, l);
        g_rlen += l;
    }
}

static double us(void) { return (double)xcw_now_us(); }

static size_t heap_used(void) {
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
}

/* ------------------------------------------------------------- 1. SRTP */

#define NPKT 2000
#define PKT 1200
static uint8_t g_pk[NPKT][PKT + SRTP_MAX_TRAILER_LEN];
static int g_pklen[NPKT];

static void fill_rtp(uint8_t *p, int seq) {
    p[0] = 0x80;
    p[1] = 102;
    p[2] = (uint8_t)(seq >> 8);
    p[3] = (uint8_t)seq;
    uint32_t ts = (uint32_t)seq * 3000;
    p[4] = (uint8_t)(ts >> 24);
    p[5] = (uint8_t)(ts >> 16);
    p[6] = (uint8_t)(ts >> 8);
    p[7] = (uint8_t)ts;
    p[8] = 0x11; p[9] = 0x22; p[10] = 0x33; p[11] = 0x44;
    for (int i = 12; i < PKT; i++) p[i] = (uint8_t)(i * 7 + seq);
}

static double g_srtp_unprot_us, g_srtp_unprot_busy_us = -1;

static double srtp_round(const char *tag, int report) {
    uint8_t key[30];
    xcw_random(key, sizeof(key));
    srtp_policy_t ptx, prx;
    memset(&ptx, 0, sizeof(ptx));
    srtp_crypto_policy_set_rtp_default(&ptx.rtp);
    srtp_crypto_policy_set_rtcp_default(&ptx.rtcp);
    ptx.ssrc.type = ssrc_any_outbound;
    ptx.key = key;
    prx = ptx;
    prx.ssrc.type = ssrc_any_inbound;
    srtp_t tx, rx;
    if (srtp_create(&tx, &ptx) || srtp_create(&rx, &prx)) {
        out("  srtp_create failed\n");
        return -1;
    }
    for (int i = 0; i < NPKT; i++) {
        fill_rtp(g_pk[i], i);
        g_pklen[i] = PKT;
    }
    double t0 = us();
    for (int i = 0; i < NPKT; i++) srtp_protect(tx, g_pk[i], &g_pklen[i]);
    double t1 = us();
    int bad = 0;
    for (int i = 0; i < NPKT; i++)
        if (srtp_unprotect(rx, g_pk[i], &g_pklen[i]) != srtp_err_status_ok || g_pklen[i] != PKT) bad++;
    double t2 = us();
    double pro = (t1 - t0) / NPKT, unp = (t2 - t1) / NPKT;
    if (report) {
        double pps = 4e6 / 8 / PKT;
        out("  %-22s protect %6.2f us/pkt  unprotect %6.2f us/pkt (%5.1f Mbit/s)  @4Mbps: %.2f%% of a thread%s\n", tag,
            pro, unp, PKT * 8 / unp, pps * unp / 1e4, bad ? "  AUTH FAILURES!" : "");
    }
    srtp_dealloc(tx);
    srtp_dealloc(rx);
    return unp;
}

#ifdef __XENON__
static volatile int g_busy_run;
static volatile uint32_t g_busy_sink;
static uint8_t g_busy_stack[64 * 1024] __attribute__((aligned(128)));
static uint8_t g_busy_mem[1024 * 1024] __attribute__((aligned(128)));
static void busy_task(void) {   /* integer + memory load, like lwIP/decoder work */
    uint32_t x = 12345;
    while (g_busy_run) {
        for (int i = 0; i < 4096; i++) {
            x = x * 1664525u + 1013904223u;
            g_busy_mem[x & (sizeof(g_busy_mem) - 1)] += (uint8_t)x;
        }
    }
    g_busy_sink = x;
}
#endif

/* Reference: the same two primitives done by mbedTLS (would be libsrtp's
 * cost with its mbedTLS crypto backend instead of the built-in one). */
static void bench_raw_crypto(void) {
    static uint8_t buf[PKT], outb[PKT];
    unsigned char key[16] = {1}, nonce[16] = {0}, stream[16], mac[20];
    size_t off = 0;
    memset(buf, 0x33, sizeof(buf));
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    double t0 = us();
    (void)off;
    for (int i = 0; i < 1000; i++) {   /* CTR by hand (MBEDTLS_CIPHER_MODE_CTR is off in the shared config) */
        for (int b = 0; b < PKT; b += 16) {
            nonce[15] = (unsigned char)(b >> 4);
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, nonce, stream);
            for (int k = 0; k < 16 && b + k < PKT; k++) outb[b + k] = buf[b + k] ^ stream[k];
        }
    }
    double t1 = us();
    const mbedtls_md_info_t *sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    for (int i = 0; i < 1000; i++) mbedtls_md_hmac(sha1, key, 20, buf, PKT, mac);
    double t2 = us();
    out("  reference (mbedTLS): AES-128-CTR %.2f us + HMAC-SHA1 %.2f us per 1200 bytes\n", (t1 - t0) / 1000,
        (t2 - t1) / 1000);
    mbedtls_aes_free(&aes);
}

static void bench_srtp(void) {
    out("[1] SRTP AES_CM_128_HMAC_SHA1_80, %d x %d-byte RTP packets (libsrtp 2.4.2, %s crypto)\n", NPKT, PKT,
        XCW_SRTP_CRYPTO);
    srtp_round("warm-up", 0);
    g_srtp_unprot_us = srtp_round("sibling idle", 1);
    bench_raw_crypto();
#ifdef __XENON__
    g_busy_run = 1;
    if (xenon_run_thread_task(1, g_busy_stack + sizeof(g_busy_stack) - 0x100, (void *)busy_task) == 0) {
        g_srtp_unprot_busy_us = srtp_round("SMT sibling (hw1) busy", 1);
        g_busy_run = 0;
        while (xenon_is_thread_task_running(1)) {}
    } else {
        g_busy_run = 0;
        out("  (could not start hw1 load)\n");
    }
#endif
}

/* -------------------------------------------------------------- 2. jitter */

static int g_aus;
static void j_emit(void *u, const uint8_t *au, size_t n, uint32_t ts) {
    (void)u; (void)au; (void)n; (void)ts;
    g_aus++;
}
static void j_nack(void *u, uint16_t pid, uint16_t blp) { (void)u; (void)pid; (void)blp; }

static double g_jit_us;

static void bench_jitter(void) {
    static xcj_t j;
    if (xcj_init(&j)) {
        out("[2] jitter: alloc failed\n");
        return;
    }
    static uint8_t pk[PKT];
    int npk = 0;
    double t = 0;
    for (int round = 0; round < 2; round++) {
        xcj_reset(&j);
        g_aus = 0;
        npk = 0;
        double t0 = us();
        uint16_t seq = 1000;
        for (int f = 0; f < 300; f++) {   /* 300 frames x 10 FU-A packets (IDR every 60) */
            for (int k = 0; k < 10; k++) {
                fill_rtp(pk, seq);
                pk[2] = (uint8_t)(seq >> 8);
                pk[3] = (uint8_t)seq;
                uint32_t ts = 90000u + (uint32_t)f * 3000u;
                pk[4] = (uint8_t)(ts >> 24); pk[5] = (uint8_t)(ts >> 16); pk[6] = (uint8_t)(ts >> 8); pk[7] = (uint8_t)ts;
                if (k == 9) pk[1] |= 0x80;
                uint8_t type = (f % 60 == 0) ? 5 : 1;
                pk[12] = (uint8_t)(0x60 | 28);
                pk[13] = (uint8_t)((k == 0 ? 0x80 : 0) | (k == 9 ? 0x40 : 0) | type);
                int want = 0;
                xcj_receive(&j, pk, PKT, (uint64_t)f, j_emit, j_nack, NULL, &want);
                npk++;
                seq++;
            }
        }
        t = (us() - t0) / npk;
    }
    g_jit_us = t;
    out("[2] jitter buffer + FU-A depacketize: %.2f us/packet (%d packets -> %d AUs)\n", t, npk, g_aus);
    xcj_free(&j);
}

/* ------------------------------------------------------- 3. DTLS memory */

#define QN 64
typedef struct {
    uint8_t d[QN][1600];
    int len[QN];
    int r, w;
} dq_t;
static dq_t g_to_client, g_to_server;
static DtlsSrtp g_cli, g_srv;

static int q_put(dq_t *q, const uint8_t *b, size_t n) {
    if (q->w - q->r >= QN || n > sizeof(q->d[0])) return (int)n;   /* drop like UDP */
    memcpy(q->d[q->w % QN], b, n);
    q->len[q->w % QN] = (int)n;
    q->w++;
    return (int)n;
}
static int q_get(dq_t *q, uint8_t *b, size_t cap) {
    if (q->r == q->w) return MBEDTLS_ERR_SSL_WANT_READ;
    int n = q->len[q->r % QN];
    if ((size_t)n > cap) n = (int)cap;
    memcpy(b, q->d[q->r % QN], (size_t)n);
    q->r++;
    return n;
}
static int cli_send(void *ctx, const uint8_t *b, size_t n) { (void)ctx; return q_put(&g_to_server, b, n); }
static int cli_recv(void *ctx, uint8_t *b, size_t n) { (void)ctx; return q_get(&g_to_client, b, n); }
static int srv_send(void *ctx, const uint8_t *b, size_t n) { (void)ctx; return q_put(&g_to_client, b, n); }
static int srv_recv(void *ctx, uint8_t *b, size_t n) { (void)ctx; return q_get(&g_to_server, b, n); }

static double g_dtls_cli_ms, g_dtls_step_ms, g_dtls_rec_us, g_cert_ms;

static void bench_dtls(void) {
    out("[3] DTLS 1.2 handshake, libpeer DtlsSrtp client <-> server in memory (cert: %s)\n",
        CONFIG_DTLS_USE_ECDSA ? "ECDSA P-256" : "RSA-1024");
    size_t h0 = heap_used();
    double t0 = us();
    dtls_srtp_init(&g_cli, DTLS_SRTP_ROLE_CLIENT, NULL);
    double t1 = us();
    dtls_srtp_init(&g_srv, DTLS_SRTP_ROLE_SERVER, NULL);
    double t2 = us();
    g_cert_ms = (t1 - t0) / 1000;
    out("  dtls_srtp_init (DRBG seed + keygen + self-signed cert): client %.1f ms, server %.1f ms\n", g_cert_ms,
        (t2 - t1) / 1000);
    g_cli.udp_send = cli_send;
    g_cli.udp_recv = cli_recv;
    g_srv.udp_send = srv_send;
    g_srv.udp_recv = srv_recv;
    strcpy(g_cli.remote_fingerprint, g_srv.local_fingerprint);
    strcpy(g_srv.remote_fingerprint, g_cli.local_fingerprint);
    double tc = 0, ts = 0, maxstep = 0;
    int rc = 1, rs = 1, steps = 0;
    double w0 = us();
    while ((rc != 0 || rs != 0) && steps < 20000) {
        double a = us();
        if (rc != 0) rc = dtls_srtp_handshake(&g_cli, NULL);
        double b = us();
        if (rs != 0) rs = dtls_srtp_handshake(&g_srv, NULL);
        double c = us();
        tc += b - a;
        ts += c - b;
        if (b - a > maxstep) maxstep = b - a;
        if (rc < 0 || rs < 0) break;
        steps++;
    }
    double w1 = us();
    g_dtls_cli_ms = tc / 1000;
    g_dtls_step_ms = maxstep / 1000;
    if (rc != 0 || rs != 0) {
        out("  HANDSHAKE FAILED (client %d, server %d)\n", rc, rs);
        return;
    }
    out("  handshake OK in %.1f ms wall: client CPU %.1f ms, server CPU %.1f ms, longest client step %.1f ms\n",
        (w1 - w0) / 1000, tc / 1000, ts / 1000, maxstep / 1000);
    out("  suite %s, SRTP keys exported: %s, heap for both contexts %u KB\n", mbedtls_ssl_get_ciphersuite(&g_cli.ssl),
        g_cli.state == DTLS_SRTP_STATE_CONNECTED && g_srv.state == DTLS_SRTP_STATE_CONNECTED ? "yes" : "NO",
        (unsigned)((heap_used() - h0) / 1024));
    /* datachannel-sized application records (SCTP packets ride in these) */
    uint8_t msg[96], in[256];
    memset(msg, 0x5a, sizeof(msg));
    int ok = 0;
    double r0 = us();
    for (int i = 0; i < 2000; i++) {
        if (mbedtls_ssl_write(&g_cli.ssl, msg, 64) == 64 && mbedtls_ssl_read(&g_srv.ssl, in, sizeof(in)) == 64) ok++;
    }
    double r1 = us();
    g_dtls_rec_us = (r1 - r0) / 2000;
    out("  64-byte DTLS record (AEAD seal + open): %.2f us each way-pair, %d/2000 ok\n", g_dtls_rec_us, ok);
    dtls_srtp_deinit(&g_cli);
    dtls_srtp_deinit(&g_srv);
}

/* ------------------------------------------------------------ 4. usrsctp */

typedef struct {
    int id;
    int up;
    int msgs;
    size_t bytes;
} ep_t;
static ep_t g_epA = {0}, g_epB = {1};
#define SQN 256
static struct {
    uint8_t d[1500];
    int len;
    ep_t *to;
} g_sq[SQN];
static int g_sqr, g_sqw, g_sq_drop;

static int sctp_out(void *addr, void *buf, size_t len, uint8_t tos, uint8_t set_df) {
    (void)tos; (void)set_df;
    if (g_sqw - g_sqr >= SQN || len > 1500) {
        g_sq_drop++;
        return 0;
    }
    int k = g_sqw % SQN;
    memcpy(g_sq[k].d, buf, len);
    g_sq[k].len = (int)len;
    g_sq[k].to = (addr == &g_epA) ? &g_epB : &g_epA;
    g_sqw++;
    return 0;
}

static int sctp_rx(struct socket *sock, union sctp_sockstore addr, void *data, size_t len, struct sctp_rcvinfo ri,
                   int flags, void *ulp) {
    (void)sock; (void)addr; (void)ri;
    ep_t *e = (ep_t *)ulp;
    if (data) {
        if (flags & MSG_NOTIFICATION) {
            union sctp_notification *n = (union sctp_notification *)data;
            if (n->sn_header.sn_type == SCTP_ASSOC_CHANGE && n->sn_assoc_change.sac_state == SCTP_COMM_UP) e->up = 1;
        } else {
            e->msgs++;
            e->bytes += len;
        }
        free(data);
    }
    return 1;
}

static uint64_t g_sctp_last_ms;
static void sctp_pump(void) {
    while (g_sqr != g_sqw) {
        int k = g_sqr % SQN;
        g_sqr++;
        usrsctp_conninput(g_sq[k].to, g_sq[k].d, (size_t)g_sq[k].len, 0);
    }
    uint64_t now = xcw_now_ms();
    if (now - g_sctp_last_ms >= 10) {
        usrsctp_handle_timers((uint32_t)(now - g_sctp_last_ms));
        g_sctp_last_ms = now;
    }
}

static struct socket *sctp_ep(ep_t *e) {
    struct socket *s = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, sctp_rx, NULL, 0, e);
    if (!s) return NULL;
    usrsctp_set_non_blocking(s, 1);
    uint32_t nodelay = 1;
    usrsctp_setsockopt(s, IPPROTO_SCTP, SCTP_NODELAY, &nodelay, sizeof(nodelay));
    struct sctp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.se_assoc_id = SCTP_ALL_ASSOC;
    ev.se_on = 1;
    ev.se_type = SCTP_ASSOC_CHANGE;
    usrsctp_setsockopt(s, IPPROTO_SCTP, SCTP_EVENT, &ev, sizeof(ev));
    struct sctp_initmsg im;
    memset(&im, 0, sizeof(im));
    im.sinit_num_ostreams = 300;
    im.sinit_max_instreams = 300;
    usrsctp_setsockopt(s, IPPROTO_SCTP, SCTP_INITMSG, &im, sizeof(im));
    struct sockaddr_conn sc;
    memset(&sc, 0, sizeof(sc));
    sc.sconn_family = AF_CONN;
    sc.sconn_port = htons(5000);
    sc.sconn_addr = e;
    usrsctp_bind(s, (struct sockaddr *)&sc, sizeof(sc));
    usrsctp_connect(s, (struct sockaddr *)&sc, sizeof(sc));   /* libpeer: simultaneous open */
    return s;
}

static double sctp_send_n(struct socket *s, ep_t *dst, int n, size_t len, uint16_t sid, uint32_t ppid) {
    static uint8_t buf[512];
    memset(buf, 0x42, sizeof(buf));
    int before = dst->msgs;
    double t0 = us();
    for (int i = 0; i < n; i++) {
        struct sctp_sendv_spa spa;
        memset(&spa, 0, sizeof(spa));
        spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
        spa.sendv_sndinfo.snd_sid = sid;
        spa.sendv_sndinfo.snd_flags = SCTP_EOR;
        spa.sendv_sndinfo.snd_ppid = htonl(ppid);
        while (usrsctp_sendv(s, buf, len, NULL, 0, &spa, sizeof(spa), SCTP_SENDV_SPA, 0) < 0) sctp_pump();
        sctp_pump();
    }
    for (int k = 0; k < 100 && dst->msgs - before < n; k++) sctp_pump();
    double t1 = us();
    if (dst->msgs - before != n) out("  (only %d/%d delivered)\n", dst->msgs - before, n);
    return (t1 - t0) / n;
}

static double g_sctp_in_us, g_sctp_json_us;

static void bench_sctp(void) {
    out("[4] usrsctp 0.9.5 (AF_CONN, no threads), two associations in memory\n");
    size_t h0 = heap_used();
    usrsctp_init_nothreads(0, sctp_out, NULL);
    usrsctp_sysctl_set_sctp_ecn_enable(0);
    usrsctp_register_address(&g_epA);
    usrsctp_register_address(&g_epB);
    g_sctp_last_ms = xcw_now_ms();
    double t0 = us();
    struct socket *a = sctp_ep(&g_epA), *b = sctp_ep(&g_epB);
    if (!a || !b) {
        out("  usrsctp_socket failed\n");
        return;
    }
    while ((!g_epA.up || !g_epB.up) && us() - t0 < 3e6) sctp_pump();
    double t1 = us();
    if (!g_epA.up || !g_epB.up) {
        out("  association did not come up\n");
        return;
    }
    out("  association up in %.2f ms (INIT/INIT-ACK/COOKIE x2), usrsctp heap %u KB\n", (t1 - t0) / 1000,
        (unsigned)((heap_used() - h0) / 1024));
    g_sctp_in_us = sctp_send_n(a, &g_epB, 5000, 38, 2, 53);
    out("  38-byte input report  A->B: %.2f us/message (send + receive + SACKs, both stacks)\n", g_sctp_in_us);
    g_sctp_json_us = sctp_send_n(b, &g_epA, 2000, 200, 4, 51);
    out("  200-byte JSON message B->A: %.2f us/message\n", g_sctp_json_us);
    usrsctp_close(a);
    usrsctp_close(b);
    for (int i = 0; i < 50; i++) sctp_pump();
}

/* ------------------------------------------------------ 5. primitives */

static void bench_primitives(void) {
    out("[5] mbedTLS primitives\n");
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    double t0 = us();
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"xc", 2);
    double t1 = us();
    out("  CTR-DRBG seed (entropy poll): %.1f ms\n", (t1 - t0) / 1000);
    mbedtls_ecdh_context a, b;
    mbedtls_ecdh_init(&a);
    mbedtls_ecdh_init(&b);
    unsigned char pub[200], sec[64];
    size_t ol;
    mbedtls_ecdh_setup(&a, MBEDTLS_ECP_DP_SECP256R1);
    mbedtls_ecdh_setup(&b, MBEDTLS_ECP_DP_SECP256R1);
    t0 = us();
    mbedtls_ecdh_make_public(&a, &ol, pub, sizeof(pub), mbedtls_ctr_drbg_random, &drbg);
    t1 = us();
    mbedtls_ecdh_read_public(&b, pub, ol);
    mbedtls_ecdh_make_public(&b, &ol, pub, sizeof(pub), mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ecdh_read_public(&a, pub, ol);
    double t2 = us();
    mbedtls_ecdh_calc_secret(&a, &ol, sec, sizeof(sec), mbedtls_ctr_drbg_random, &drbg);
    double t3 = us();
    out("  ECDH P-256: keygen %.1f ms, shared secret %.1f ms\n", (t1 - t0) / 1000, (t3 - t2) / 1000);
    mbedtls_ecdh_free(&a);
    mbedtls_ecdh_free(&b);
    mbedtls_ecdsa_context k;
    mbedtls_ecdsa_init(&k);
    t0 = us();
    mbedtls_ecdsa_genkey(&k, MBEDTLS_ECP_DP_SECP256R1, mbedtls_ctr_drbg_random, &drbg);
    t1 = us();
    unsigned char hash[32] = {1, 2, 3}, sig[MBEDTLS_ECDSA_MAX_LEN];
    size_t sl;
    mbedtls_ecdsa_write_signature(&k, MBEDTLS_MD_SHA256, hash, 32, sig, sizeof(sig), &sl, mbedtls_ctr_drbg_random,
                                  &drbg);
    t2 = us();
    int v = mbedtls_ecdsa_read_signature(&k, hash, 32, sig, sl);
    t3 = us();
    out("  ECDSA P-256: keygen %.1f ms, sign %.1f ms, verify %.1f ms (%s)\n", (t1 - t0) / 1000, (t2 - t1) / 1000,
        (t3 - t2) / 1000, v == 0 ? "ok" : "BAD");
    mbedtls_ecdsa_free(&k);
    mbedtls_rsa_context r;
    mbedtls_rsa_init(&r);
    t0 = us();
    int rr = mbedtls_rsa_gen_key(&r, mbedtls_ctr_drbg_random, &drbg, 1024, 65537);
    t1 = us();
    out("  RSA-1024 keygen (libpeer's default cert, NOT used here): %.0f ms%s\n", (t1 - t0) / 1000, rr ? " FAILED" : "");
    mbedtls_rsa_free(&r);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
}

/* -------------------------------------------------------------- budget */

static void budget(void) {
    double pps = 4e6 / 8 / PKT;   /* 417 video packets/s */
    double audio_pps = 50, input_pps = 125;
    double unp = g_srtp_unprot_busy_us > 0 ? g_srtp_unprot_busy_us : g_srtp_unprot_us;
    /* per video packet: SRTP + jitter/depacketize + RX ring memcpy (~= 1/4 of the jitter cost) */
    double video = pps * (unp + g_jit_us * 1.25);
    double audio = audio_pps * (unp * 0.25 + 2);   /* ~200-byte Opus */
    double input = input_pps * (g_sctp_in_us / 2 + 2 * g_dtls_rec_us);
    double rtcp = 4 * (unp * 0.1 + 20);
    double total = video + audio + input + rtcp;
    out("[budget] WebRTC media path at 4 Mbps 1200-byte packets (+50 Opus/s, 125 input/s):\n");
    out("  video %.1f ms/s + audio %.1f + input %.1f + RTCP %.1f = %.1f ms per second = %.1f%% of one hw thread\n",
        video / 1000, audio / 1000, input / 1000, rtcp / 1000, total / 1000, total / 1e4);
    out("  (lwIP RX + NIC MMIO not included: measure with STAT on hardware; DTLS handshake is one-time:\n"
        "   client CPU %.0f ms, longest blocking step %.0f ms, cert %.0f ms)\n",
        g_dtls_cli_ms, g_dtls_step_ms, g_cert_ms);
}

int main(void) {
#ifdef __XENON__
    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);
    xenon_thread_startup();
#endif
    out("XCloud360 stage 4 WebRTC bench — %s\n", PLATFORM);
    out("libs: libsrtp 2.4.2, mbedTLS 3.4.0 (DTLS-SRTP), usrsctp 0.9.5, libpeer (xcw port); no AltiVec\n\n");
    size_t h0 = heap_used();
    if (srtp_init() != srtp_err_status_ok) out("srtp_init failed\n");
    bench_srtp();
    bench_jitter();
    bench_dtls();
    bench_sctp();
    bench_primitives();
    budget();
    out("\nheap in use at end: %u KB above start. DONE.\n", (unsigned)((heap_used() - h0) / 1024));
#ifdef __XENON__
    for (;;) {
    }
#endif
    return 0;
}
