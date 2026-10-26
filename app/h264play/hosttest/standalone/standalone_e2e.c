/*
 * XCloud360 stage 5 — offline end-to-end test of the INTEGRATED native path
 * (source/native/xc_native.c + source/session + source/webrtc), laid out like
 * the Xbox app:
 *   main thread  = the menu: menu_model.c fed by xn_state_line(), PLAY /
 *                  B-cancel / Back+Start, the "decoder" draining the AU ring;
 *   "hw1" thread = the network thread: xn_net_step() every ~0.5 ms + the
 *                  gamepad poll (24-byte frames -> xn_pad_frame, 2 pads) +
 *                  a chat ping (round-trip check);
 *   session      = xs_task_posix.c (the xs_task.c loop on a pthread),
 *                  HTTPS = port_rt.c's fake GSSV, /sdp + /ice bridged to a
 *                  pion "xCloud" (hosttest/webrtc/peer_pion) per session.
 * Scenario: PLAY game 1 -> states -> media -> input -> Back+Start (DELETE,
 * teardown) -> PLAY game 2 cancelled with B while in the queue -> PLAY
 * game 3 -> media -> Back+Start -> (v0.19.3) PLAY game 4 + "Reiniciar"
 * from the Start menu in the queue (DELETE before the mocked SMC) ->
 * (v0.19.3) Back+Start keeps the session open in the menu (keepalive, no
 * DELETE, nothing decoded, no keyframe loop), the same game resumes with a
 * PLI and no new session, another game DELETEs the old one before its
 * /play, the server ends a paused session (keepalive 404) and the state
 * comes back clean for a new PLAY. Each streamed session dir gets the files
 * hosttest/webrtc/check_e2e.py reads (recv.h264/.auidx/.opus/.json).
 * v0.19.2: HTTPS is real TLS to a local server (port_rt.c); keepalive every
 * 1 s; the "decoder" leases hw3 like main.c (4 threads, lends it to the
 * session thread when the machine asks, takes it back). Game 1: a forced
 * 401 on a keepalive mid-stream (hw1 -> session thread -> re-login ->
 * back to hw1). Game 3: the server closes the keep-alive connection twice:
 * once with session resumption (hw1 reconnects alone, abbreviated
 * handshake), once without (needs a full handshake -> session thread).
 * NO real xCloud, no real account: every token is FAKE-*.
 *
 * usage: standalone_e2e --dir D --pion PION --h264 FILE [--seconds N]
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../source/menu_model.h"
#include "../../source/login/https.h"
#include "../../source/native/xc_native.h"
#include "../../source/session/xs_task.h"
#include "../../source/webrtc/xc_webrtc.h"
#include "port_rt.h"
#include "xcw_port.h"

extern char **environ;
unsigned long xcw_posix_dropped(void);
unsigned long xcw_posix_rx(void);
unsigned long xcw_posix_tx(void);

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } else if (getenv("E2E_VERBOSE")) { fprintf(stderr, "  ok: "); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ------------------------------------------------ AU ring (hw1 -> main) */
#define RING (1024 * 1024)
static uint8_t g_ring[RING];
static volatile uint32_t g_rh, g_rt;   /* head: hw1, tail: main */
static volatile uint32_t g_inj_full;
static uint32_t r_used(void) { return (g_rh - g_rt) & (RING - 1); }
static void r_put(const uint8_t *p, uint32_t n) {
    uint32_t h = g_rh;
    for (uint32_t i = 0; i < n; i++) g_ring[(h + i) & (RING - 1)] = p[i];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_rh = (h + n) & (RING - 1);
}
static void r_get(uint8_t *p, uint32_t n) {
    uint32_t t = g_rt;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (uint32_t i = 0; i < n; i++) p[i] = g_ring[(t + i) & (RING - 1)];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_rt = (t + n) & (RING - 1);
}

static int plat_inject(const uint8_t *au, size_t n, int idr, int params) {
    if (RING - 1 - r_used() < n + 6) { g_inj_full++; return -1; }
    uint8_t h[6] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)idr,
                    (uint8_t)params};
    r_put(h, 6);
    r_put(au, (uint32_t)n);
    return 0;
}

/* ---------------------------------------------------- per-phase outputs */
static FILE *volatile g_opus;
static volatile int g_audio_pkts;
static void plat_audio(uint16_t seq, uint32_t ts, const uint8_t *p, size_t n) {
    FILE *f = g_opus;
    g_audio_pkts++;
    if (!f) return;
    uint8_t h[8] = {(uint8_t)(seq >> 8), (uint8_t)seq, (uint8_t)(ts >> 24), (uint8_t)(ts >> 16),
                    (uint8_t)(ts >> 8), (uint8_t)ts, (uint8_t)(n >> 8), (uint8_t)n};
    fwrite(h, 1, 8, f);
    fwrite(p, 1, n, f);
}

#define MAXPING 512
static uint64_t g_ping_us[MAXPING];
static volatile int g_pings, g_pongs;
static double g_rtt_sum, g_rtt_max;
static void plat_msg(int ch, const uint8_t *d, size_t n) {
    if (ch == XCW_CH_CHAT && n > 5 && !memcmp(d, "pong ", 5)) {
        char num[16];
        size_t l = n - 5 < sizeof(num) - 1 ? n - 5 : sizeof(num) - 1;
        memcpy(num, d + 5, l);
        num[l] = 0;
        int k = atoi(num);   /* datachannel payloads are not NUL-terminated */
        if (k >= 0 && k < MAXPING && g_ping_us[k]) {
            double r = (xcw_now_us() - g_ping_us[k]) / 1000.0;
            g_rtt_sum += r;
            if (r > g_rtt_max) g_rtt_max = r;
            g_pongs++;
        }
    }
}

static volatile int g_before_session_calls;
static void plat_before(void) { g_before_session_calls++; }

/* ------------------------------------------------------------ "hw1" */
static volatile int g_run = 1, g_forward, g_neutral_req;
static volatile uint64_t g_fwd_t0;
static xcw_stats_t g_last;            /* hw1 writes, main reads after detach */
static volatile int g_have_last, g_max_pcs_phase;
static pthread_mutex_t g_stmu = PTHREAD_MUTEX_INITIALIZER;

static void pad_frame(uint8_t idx, int a, int b, int16_t lx) {
    uint8_t buf[24];
    memset(buf, 0, sizeof(buf));
    buf[0] = idx;
    buf[1] = (uint8_t)((a ? 1 : 0) | (b ? 2 : 0));
    buf[7] = (uint8_t)(lx & 0xff);
    buf[8] = (uint8_t)((uint16_t)lx >> 8);
    xn_pad_frame(buf, sizeof(buf));
}

static void *hw1(void *a) {
    (void)a;
    uint64_t last_pad = 0, last_ping = 0, last_st = 0;
    xcw_t *prev = NULL;
    while (g_run) {
        xn_net_step();
        uint64_t now = xcw_now_ms();
        xcw_t *x = xn_pc();
        if (x && x != prev) g_max_pcs_phase++;
        prev = x;
        if (g_forward && now != last_pad) {   /* main.c: send_pad at 1 kHz */
            last_pad = now;
            uint64_t t = now - g_fwd_t0;
            int step = (int)(t / 100);
            pad_frame(0, step & 1, 0, (int16_t)(((step % 21) - 10) * 3000));
            if (t >= 2000) pad_frame(1, 0, (step / 2) & 1, 0);   /* 2nd controller joins */
        }
        if (g_neutral_req) { g_neutral_req = 0; xn_pad_neutral(); }
        if (x && xcw_state(x) == XCW_STREAMING && now - last_ping >= 250 && g_pings < MAXPING) {
            last_ping = now;
            char m[32];
            int n = snprintf(m, sizeof(m), "ping %d", g_pings);
            g_ping_us[g_pings] = xcw_now_us();
            if (xcw_send(x, XCW_CH_CHAT, m, (size_t)n, 1) == 0) g_pings++;
        }
        if (x && now - last_st >= 100) {
            last_st = now;
            xcw_stats_t s;
            xcw_get_stats(x, &s);
            pthread_mutex_lock(&g_stmu);
            g_last = s;
            g_have_last = 1;
            pthread_mutex_unlock(&g_stmu);
        }
        usleep(500);
    }
    return NULL;
}

/* ------------------------------------------- "decoder" hw3 lease (main.c) */
static volatile int g_dec_thr;          /* 0 = closed */
static volatile int g_dec_has_hw3;
static int g_dec_reopens, g_dec_to3, g_dec_to4;
static volatile int g_hw3_violations;
extern int (*xst_hw3_busy_hook)(void);
static int hw3_busy(void) {
    if (g_dec_has_hw3) { g_hw3_violations++; return 1; }
    return 0;
}
static void dec_open(void) {
    g_dec_has_hw3 = xn_dec_claim_hw3();
    g_dec_thr = g_dec_has_hw3 ? 4 : 3;
    xn_set_dec_threads(g_dec_thr);
}
static void dec_close(void) {
    if (g_dec_has_hw3) { g_dec_has_hw3 = 0; xn_dec_release_hw3(); }
    g_dec_thr = 0;
    xn_set_dec_threads(0);
}
/* = run_video's 10 ms check + dec_reopen */
static void dec_tick(void) {
    int w = xn_dec_threads_wanted(g_dec_thr, 4);
    if (w == g_dec_thr) return;
    int had = g_dec_thr;
    if (g_dec_has_hw3) { g_dec_has_hw3 = 0; xn_dec_release_hw3(); }
    if (w >= 4) { g_dec_has_hw3 = xn_dec_claim_hw3(); w = g_dec_has_hw3 ? 4 : 3; }
    g_dec_thr = w;
    xn_set_dec_threads(w);
    g_dec_reopens++;
    if (w < had) g_dec_to3++;
    else g_dec_to4++;
    fprintf(stderr, "        [dec] threads %d -> %d\n", had, w);
    xn_request_keyframe(1);
}

static int stat_int(const char *tail, const char *key) {
    const char *p = strstr(tail, key);
    return p ? atoi(p + strlen(key)) : -1;
}
static double stat_dbl(const char *tail, const char *key) {
    const char *p = strstr(tail, key);
    return p ? atof(p + strlen(key)) : -1.0;
}
static int log_count(const char *needle) {
    int n = 0;
    for (const char *p = rt_log_text(); (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

/* ------------------------------------------------------------- "menu" */
static menu_model_t g_mm;
static unsigned g_seen;
static char g_states[64][200];
static int g_nstates;
static int poll_state(void) {
    char l[240], c[240];
    unsigned s = xn_state_line(l, sizeof(l));
    if (!s || s == g_seen) return 0;
    g_seen = s;
    if (g_nstates < 64) snprintf(g_states[g_nstates++], sizeof(g_states[0]), "%.199s", l + 6);
    snprintf(c, sizeof(c), "%s", l);
    mm_handle_line(&g_mm, c);
    fprintf(stderr, "        [menu] %s\n", l + 6);
    return 1;
}

static int saw_state(const char *code, const char *text_part) {
    size_t cl = strlen(code);
    for (int i = 0; i < g_nstates; i++)
        if (!strncmp(g_states[i], code, cl) && g_states[i][cl] == '\t' && (!text_part || strstr(g_states[i], text_part)))
            return 1;
    return 0;
}

static pid_t spawn_pion(const char *pion, const char *dir, const char *h264, int secs) {
    char s[16];
    snprintf(s, sizeof(s), "%d", secs);
    char logp[600];
    snprintf(logp, sizeof(logp), "%s/pion.log", dir);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    char *argv[] = {(char *)pion, "-dir", (char *)dir, "-h264", (char *)h264, "-fps", "30", "-seconds", s, NULL};
    pid_t pid = -1;
    if (posix_spawn(&pid, pion, &fa, NULL, argv, environ) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    return pid;
}

static int wait_pid(pid_t p, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        int st;
        pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p) return 0;
        usleep(10000);
    }
    kill(p, SIGTERM);
    waitpid(p, NULL, 0);
    return -1;
}

static int wait_can_play(int ms) {
    for (int i = 0; i < ms / 10; i++) {
        poll_state();
        if (xn_can_play()) return 0;
        usleep(10000);
    }
    return -1;
}

static void phase_reset(void) {
    g_rh = g_rt = 0;
    g_inj_full = 0;
    g_pings = g_pongs = 0;
    g_rtt_sum = g_rtt_max = 0;
    memset(g_ping_us, 0, sizeof(g_ping_us));
    g_audio_pkts = 0;
    g_max_pcs_phase = 0;
    g_nstates = 0;
    g_seen = 0;
    pthread_mutex_lock(&g_stmu);
    g_have_last = 0;
    memset(&g_last, 0, sizeof(g_last));
    pthread_mutex_unlock(&g_stmu);
}

/* One streamed game: PLAY -> states -> media -> Back+Start. 0 = ok. */
enum { SC_401 = 1, SC_CLOSES = 2 };
static int play_game(const char *base, int k, const char *pion, const char *h264, int secs, const char *title,
                     const char *name, int scenario) {
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/s%d", base, k);
    mkdir(dir, 0755);
    fprintf(stderr, "=== phase %d: PLAY %s (%s), %d s of game -> %s\n", k, title, name, secs, dir);
    phase_reset();
    pthread_mutex_lock(&g_gssv.mu);
    snprintf(g_gssv.next_dir, sizeof(g_gssv.next_dir), "%s", dir);
    g_gssv.extra_queue_polls = 0;
    int sess_before = g_gssv.nsess;
    pthread_mutex_unlock(&g_gssv.mu);
    pid_t pp = spawn_pion(pion, dir, h264, secs + 90);
    CHECK(pp > 0, "pion started");
    char p[600];
    snprintf(p, sizeof(p), "%s/recv.h264", dir);
    FILE *fh = fopen(p, "wb");
    snprintf(p, sizeof(p), "%s/recv.auidx", dir);
    FILE *fi = fopen(p, "w");
    snprintf(p, sizeof(p), "%s/recv.opus", dir);
    g_opus = fopen(p, "wb");

    CHECK(xn_can_play(), "can play before PLAY %d", k);
    int before = g_before_session_calls;
    uint64_t t_play = xcw_now_ms();
    int r = xn_play(title, name, 0);
    CHECK(r == XN_OK, "xn_play %s = %d", title, r);
    CHECK(g_before_session_calls == before + 1, "before_session hook (login cancel reset) called");
    CHECK(xn_play(title, name, 0) == XN_EBUSY, "second PLAY while busy is refused");

    /* menu waiting screen until the first AU */
    uint64_t t_video = 0;
    while (xcw_now_ms() - t_play < 60000) {
        poll_state();
        if (g_mm.state == MM_ST_ERROR) break;
        if (r_used() >= 6) { t_video = xcw_now_ms(); break; }
        usleep(5000);
    }
    CHECK(t_video, "video reached the decoder ring (state %s: %s)", g_mm.state_code, g_mm.state_text);
    /* v0.19.2: main.c waits (<= 2 s) for the session thread to hand the
     * live phases to hw1, then opens the decoder with the hw3 lease. */
    uint64_t t_wait = xcw_now_ms();
    while (xn_session_thread_busy() && xcw_now_ms() - t_wait < 2000) usleep(2000);
    dec_open();
    fprintf(stderr, "    decoder opened with %d threads (waited %u ms for the session thread)\n", g_dec_thr,
            (unsigned)(xcw_now_ms() - t_wait));
    CHECK(g_dec_thr == 4, "decoder gets 4 threads (hw3 free at game entry): %d", g_dec_thr);
    pthread_mutex_lock(&g_gssv.mu);
    int rfull0 = g_gssv.region_full, rres0 = g_gssv.region_resumed, rconn0 = g_gssv.region_conns;
    int login0 = g_gssv.n_login, n401_0 = g_gssv.n_401, kar0 = g_gssv.region_ka_on_reused;
    pthread_mutex_unlock(&g_gssv.mu);
    int reopen0 = g_dec_reopens, to3_0 = g_dec_to3, to4_0 = g_dec_to4;
    int esc0 = log_count("[sess] hw1 devolve a sessao");
    int done401 = 0, close1 = 0, close2 = 0;
    int aus_after_401 = 0, aus_after_close = 0;
    uint64_t t_401 = 0;
    fprintf(stderr, "    first AU %.1f s after PLAY\n", t_video ? (t_video - t_play) / 1000.0 : -1.0);
    CHECK(saw_state("STARTING", "Autenticando"), "STATE STARTING Autenticando");
    CHECK(saw_state("STARTING", "Iniciando"), "STATE STARTING Iniciando <nome>");
    CHECK(saw_state("QUEUE", "Na fila: posicao 3"), "STATE QUEUE Na fila: posicao 3");
    CHECK(saw_state("STARTING", "Conectando video"), "STATE STARTING Conectando video");

    /* in game: drain AUs like pull_next_au, gamepads forwarded, one decoder
     * error -> keyframe request (KFRQ of the PC path) */
    g_fwd_t0 = xcw_now_ms();
    g_forward = 1;
    uint64_t t0 = xcw_now_ms();
    int aus = 0, idrs = 0, kfrq_sent = 0, streaming_seen = 0, err = 0;
    uint64_t t_dec = 0;
    while (xcw_now_ms() - t0 < (uint64_t)secs * 1000) {
        poll_state();
        uint64_t el = xcw_now_ms() - t0;
        if (xcw_now_ms() - t_dec >= 10) { t_dec = xcw_now_ms(); dec_tick(); }
        if (scenario == SC_401 && !done401 && el > 3000) {
            done401 = 1;
            t_401 = xcw_now_ms();
            pthread_mutex_lock(&g_gssv.mu);
            g_gssv.revoke_next_ka = 1;
            pthread_mutex_unlock(&g_gssv.mu);
            fprintf(stderr, "    >>> forcing 401 on the next keepalive\n");
        }
        if (scenario == SC_CLOSES && !close1 && el > 2500) {
            close1 = 1;
            pthread_mutex_lock(&g_gssv.mu);
            g_gssv.resume_ok = 1;
            g_gssv.close_next_region = 1;
            pthread_mutex_unlock(&g_gssv.mu);
            fprintf(stderr, "    >>> server closes the keep-alive connection (resumption allowed)\n");
        }
        if (scenario == SC_CLOSES && close1 && !close2 && el > 5000) {
            close2 = 1;
            pthread_mutex_lock(&g_gssv.mu);
            g_gssv.resume_ok = 0;
            g_gssv.close_next_region = 1;
            pthread_mutex_unlock(&g_gssv.mu);
            fprintf(stderr, "    >>> server closes the keep-alive connection (NO resumption)\n");
        }
        if (g_mm.state == MM_ST_STREAMING) streaming_seen = 1;
        if (g_mm.state == MM_ST_ERROR || g_mm.state == MM_ST_IDLE) { err = 1; break; }
        while (r_used() >= 6) {
            uint8_t h[6];
            uint32_t t = g_rt;
            for (int i = 0; i < 6; i++) h[i] = g_ring[(t + i) & (RING - 1)];
            uint32_t n = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
            if (r_used() < 6 + n) break;
            r_get(h, 6);
            uint8_t *b = malloc(n);
            r_get(b, n);
            fwrite(b, 1, n, fh);
            fprintf(fi, "%u 0 %u %u\n", n, h[4], h[5]);
            free(b);
            aus++;
            idrs += h[4];
            if (t_401 && xcw_now_ms() - t_401 > 3000) aus_after_401++;
            if (close2 && xcw_now_ms() - t0 > 9000) aus_after_close++;
        }
        if (!kfrq_sent && xcw_now_ms() - t0 > 3000) { kfrq_sent = 1; xn_request_keyframe(0); }
        usleep(2000);
    }
    CHECK(!err, "no ERROR/IDLE during the game (%s)", g_mm.state_text);
    CHECK(streaming_seen && saw_state("STREAMING", name), "STATE STREAMING <name>");
    CHECK(aus > secs * 20, "AUs delivered: %d", aus);
    char tail[600];
    xn_stat_tail(tail, sizeof(tail));
    fprintf(stderr, "    STAT tail:%s\n", tail);
    CHECK(strstr(tail, " rtc=ok ") != NULL, "STAT tail says rtc=ok");

    /* ---- v0.19.2: keep-alive, hw1 driving, hw3 lease ---- */
    pthread_mutex_lock(&g_gssv.mu);
    int rfull = g_gssv.region_full - rfull0, rres = g_gssv.region_resumed - rres0;
    int rconn = g_gssv.region_conns - rconn0, logins = g_gssv.n_login - login0, n401 = g_gssv.n_401 - n401_0;
    int kar = g_gssv.region_ka_on_reused - kar0, ka_now = g_gssv.s[sess_before].n_ka;
    int maxreq = g_gssv.region_max_reqs_per_conn;
    pthread_mutex_unlock(&g_gssv.mu);
    int esc = log_count("[sess] hw1 devolve a sessao") - esc0;
    fprintf(stderr, "    tls region during game: conns=%d full=%d resumed=%d, keepalives=%d (%d on a reused conn),"
                    " max reqs on one conn=%d, 401s=%d, re-logins=%d, escalations=%d, dec reopens=%d\n",
            rconn, rfull, rres, ka_now, kar, maxreq, n401, logins, esc, g_dec_reopens - reopen0);
    CHECK(ka_now >= secs - 3, "keepalive every 1 s: %d in %d s", ka_now, secs);
    CHECK(strstr(tail, " sess=net ") != NULL, "STAT: session driven by hw1 at the end of the game");
    CHECK(stat_int(tail, " dec_thr=") == 4, "STAT dec_thr=4 at the end (%d)", stat_int(tail, " dec_thr="));
    CHECK(stat_dbl(tail, " sess_cpu=") >= 0 && stat_dbl(tail, " sess_cpu=") < 20, "STAT sess_cpu=%.2f ms/2s",
          stat_dbl(tail, " sess_cpu="));
    CHECK(g_hw3_violations == 0, "session thread never started while the decoder held hw3 (%d)", g_hw3_violations);
    if (scenario == 0) {
        CHECK(rconn == 0 && rfull == 0 && rres == 0, "no new region TLS connection during the game (%d/%d/%d)", rconn,
              rfull, rres);
        CHECK(kar >= ka_now - 1, "keepalives reuse the session's TLS connection (%d of %d)", kar, ka_now);
        CHECK(esc == 0 && g_dec_reopens == reopen0, "no hw1 -> thread escalation, no decoder reopen (%d, %d)", esc,
              g_dec_reopens - reopen0);
    }
    if (scenario == SC_401) {
        CHECK(n401 >= 2 && logins == 1, "forced 401: hw1 keepalive 401 + thread retry 401, then 1 re-login (%d, %d)",
              n401, logins);
        CHECK(rconn == 0 && rfull == 0, "re-login kept the region keep-alive connection (conns %d, full %d)", rconn, rfull);
        CHECK(esc == 1, "one hw1 -> session thread escalation (%d)", esc);
        CHECK(g_dec_to3 - to3_0 == 1 && g_dec_to4 - to4_0 == 1, "decoder lent hw3 once and took it back (%d/%d)",
              g_dec_to3 - to3_0, g_dec_to4 - to4_0);
        CHECK(aus_after_401 > 30, "video kept flowing after the 401 (%d AUs)", aus_after_401);
        CHECK(kar >= ka_now - 2, "keepalives on the reused connection (%d of %d)", kar, ka_now);
    }
    if (scenario == SC_CLOSES) {
        CHECK(rconn == 2 && rres == 1 && rfull == 1,
              "2 reconnects: one resumed on hw1, one full on the session thread (conns %d, resumed %d, full %d)",
              rconn, rres, rfull);
        CHECK(esc == 1, "only the no-resumption reconnect escalated (%d)", esc);
        CHECK(g_dec_to3 - to3_0 == 1 && g_dec_to4 - to4_0 == 1, "decoder lent hw3 once and took it back (%d/%d)",
              g_dec_to3 - to3_0, g_dec_to4 - to4_0);
        CHECK(aus_after_close > 30, "video kept flowing after the reconnects (%d AUs)", aus_after_close);
    }

    /* Back+Start: neutral pads, STOP (DELETE), full teardown */
    g_forward = 0;
    g_neutral_req = 1;
    usleep(20000);
    xcw_stats_t s;
    pthread_mutex_lock(&g_stmu);
    s = g_last;
    int have = g_have_last;
    pthread_mutex_unlock(&g_stmu);
    uint64_t t_stop = xcw_now_ms();
    xn_stop();
    usleep(30000);   /* main.c: xn_stop, then GPU finish + avcodec_close */
    dec_close();
    int cp = wait_can_play(25000);
    CHECK(cp == 0, "teardown done (session thread out + hw1 detached) in %.1f s", (xcw_now_ms() - t_stop) / 1000.0);
    fprintf(stderr, "    teardown %.2f s\n", (xcw_now_ms() - t_stop) / 1000.0);
    CHECK(xn_pc() == NULL, "no PeerConnection after STOP");
    CHECK(saw_state("IDLE", "Sessao encerrada"), "STATE IDLE Sessao encerrada");
    pthread_mutex_lock(&g_gssv.mu);
    gssv_sess_t gs = g_gssv.s[sess_before];
    int nsess = g_gssv.nsess - sess_before;
    pthread_mutex_unlock(&g_gssv.mu);
    CHECK(nsess == 1, "exactly one xCloud session for this PLAY (%d)", nsess);
    CHECK(!strcmp(gs.title, title), "/play titleId %s", gs.title);
    CHECK(gs.n_delete == 1, "DELETE sent once (%d)", gs.n_delete);
    CHECK(gs.n_connect == 1 && gs.n_sdp == 1, "/connect %d /sdp %d", gs.n_connect, gs.n_sdp);
    CHECK(gs.n_ka >= secs / 5 - 1, "keepalives %d in %d s", gs.n_ka, secs);
    fprintf(stderr, "    gssv: sdp=%d ice_post=%d ice_get=%d connect=%d keepalive=%d delete=%d\n", gs.n_sdp,
            gs.n_ice_post, gs.n_ice_get, gs.n_connect, gs.n_ka, gs.n_delete);
    CHECK(g_max_pcs_phase == 1, "one PeerConnection (%d)", g_max_pcs_phase);
    CHECK(wait_pid(pp, 15000) == 0, "pion finished after DELETE");
    fclose(fh);
    fclose(fi);
    FILE *of = g_opus;
    g_opus = NULL;
    fclose(of);

    snprintf(p, sizeof(p), "%s/recv.json", dir);
    FILE *j = fopen(p, "w");
    fprintf(j,
            "{\"state\":\"%s\",\"fail_reason\":\"%s\",\"t_offer_ms\":0,\"t_ice_ms\":%u,\"t_dtls_ms\":%u,"
            "\"t_sctp_ms\":%u,\"t_handshake_ms\":%u,\"t_first_au_ms\":%u,"
            "\"rtp_packets\":%u,\"aus\":%u,\"idrs\":%u,\"video_bytes\":%llu,\"gated_dropped\":%u,"
            "\"jit_dropped\":%u,\"jit_resyncs\":%u,\"jit_late\":%u,\"jit_dup\":%u,\"jit_pool_full\":%u,"
            "\"nacks_sent\":%u,\"pli_sent\":%u,\"keyframe_requests\":%u,\"remb_sent\":%u,\"rr_sent\":%u,"
            "\"audio_packets\":%u,\"audio_bytes\":%llu,\"srtp_errors\":%u,\"remote_pli\":%u,"
            "\"dc_rx\":[%u,%u,%u,%u],\"dc_tx\":[%u,%u,%u,%u],\"dc_tx_fail\":%u,"
            "\"input_sent\":%u,\"input_fail\":%u,\"input_seq\":%u,\"pads_announced\":%u,"
            "\"rtt_samples\":%u,\"rtt_avg_us\":%u,\"rtp_expected\":%u,\"rtp_lost\":%u,"
            "\"pings\":%d,\"pongs\":%d,\"rtt_avg_ms\":%.3f,\"rtt_max_ms\":%.3f,"
            "\"udp_rx\":%lu,\"udp_tx\":%lu,\"udp_dropped\":%lu,\"ring_full\":%u,\"app_aus\":%d,\"app_idrs\":%d,"
            "\"first_au_after_play_ms\":%llu,\"teardown_ms\":%llu}\n",
            have ? xcw_state_name(s.state) : "none", s.fail_reason ? s.fail_reason : "", s.t_ice_ms, s.t_dtls_ms,
            s.t_sctp_ms, s.t_handshake_ms, s.t_first_au_ms, s.rtp_packets, s.aus, s.idrs,
            (unsigned long long)s.video_bytes, s.gated_dropped, s.jit_dropped, s.jit_resyncs, s.jit_late, s.jit_dup,
            s.jit_pool_full, s.nacks_sent, s.pli_sent, s.keyframe_requests, s.remb_sent, s.rr_sent, s.audio_packets,
            (unsigned long long)s.audio_bytes, s.srtp_errors, s.remote_pli, s.dc_rx[0], s.dc_rx[1], s.dc_rx[2],
            s.dc_rx[3], s.dc_tx[0], s.dc_tx[1], s.dc_tx[2], s.dc_tx[3], s.dc_tx_fail, s.input_sent, s.input_fail,
            s.input_seq, s.pads_announced, s.rtt_samples, s.rtt_avg_us, s.rtp_expected, s.rtp_lost, g_pings,
            g_pongs, g_pongs ? g_rtt_sum / g_pongs : 0.0, g_rtt_max, xcw_posix_rx(), xcw_posix_tx(),
            xcw_posix_dropped(), g_inj_full, aus, idrs, (unsigned long long)(t_video ? t_video - t_play : 0),
            (unsigned long long)(xcw_now_ms() - t_stop));
    fclose(j);
    CHECK(s.pads_announced >= 1, "2nd controller announced with gamepadChanged (%u)", s.pads_announced);
    CHECK(s.srtp_errors == 0, "srtp errors %u", s.srtp_errors);
    return 0;
}

/* PLAY, then B while the session waits in the queue: DELETE, no PC. */
static void cancel_in_queue(const char *base, int k) {
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/s%d", base, k);
    mkdir(dir, 0755);
    fprintf(stderr, "=== phase %d: PLAY then B in the queue\n", k);
    phase_reset();
    pthread_mutex_lock(&g_gssv.mu);
    snprintf(g_gssv.next_dir, sizeof(g_gssv.next_dir), "%s", dir);
    g_gssv.extra_queue_polls = 20;
    int sess_before = g_gssv.nsess;
    pthread_mutex_unlock(&g_gssv.mu);
    CHECK(xn_play("HALO5", "Halo 5", 1) == XN_OK, "xn_play HALO5 (f2p offering)");
    uint64_t t0 = xcw_now_ms();
    while (xcw_now_ms() - t0 < 30000 && g_mm.state != MM_ST_QUEUE) { poll_state(); usleep(5000); }
    CHECK(g_mm.state == MM_ST_QUEUE, "reached QUEUE (%s)", g_mm.state_text);
    fprintf(stderr, "    B pressed in queue: \"%s\"\n", g_mm.state_text);
    xn_stop();
    uint64_t ts = xcw_now_ms();
    CHECK(wait_can_play(25000) == 0, "cancel teardown in %.1f s", (xcw_now_ms() - ts) / 1000.0);
    pthread_mutex_lock(&g_gssv.mu);
    gssv_sess_t gs = g_gssv.s[sess_before];
    int nsess = g_gssv.nsess - sess_before;
    pthread_mutex_unlock(&g_gssv.mu);
    CHECK(nsess == 1 && gs.n_delete == 1, "cancelled session deleted (sessions %d, deletes %d)", nsess, gs.n_delete);
    CHECK(gs.n_sdp == 0 && g_max_pcs_phase == 0, "no SDP / PeerConnection for a cancelled queue (%d/%d)", gs.n_sdp,
          g_max_pcs_phase);
    CHECK(saw_state("IDLE", "Sessao encerrada"), "STATE IDLE after cancel");
}

/* v0.19.3: PLAY, then "Reiniciar o Xbox" (v0.20.0: top button) while the session waits
 * in the queue: mm_power_run (menu_model.c) with main.c's native platform
 * (xn_busy / xn_stop, 3 s timeout) and the SMC mocked. The DELETE must be
 * out BEFORE the reboot; Back+Start / B are not involved. */
static int g_pw_reboots, g_pw_shutdowns, g_pw_sess, g_pw_del_at_smc = -1;
static int pw_open(void *u) { (void)u; return xn_busy(); }
static void pw_stop(void *u) { (void)u; xn_stop(); }
static int pw_done(void *u) { (void)u; poll_state(); return !xn_busy(); }
static unsigned long long pw_now(void *u) { (void)u; return (unsigned long long)xcw_now_ms() * 1000ULL; }
static void pw_wait(void *u) { (void)u; poll_state(); usleep(10000); }
static void pw_show(void *u, const char *l1, const char *l2) { (void)u; fprintf(stderr, "    [power] %s %s\n", l1, l2); }
static void pw_smc_seen(void) {
    pthread_mutex_lock(&g_gssv.mu);
    g_pw_del_at_smc = g_gssv.s[g_pw_sess].n_delete;
    pthread_mutex_unlock(&g_gssv.mu);
}
/* mocks do libxenon (xenon_smc.h): no Xbox não voltam */
static void xenon_smc_power_reboot(void) { g_pw_reboots++; pw_smc_seen(); }
static void xenon_smc_power_shutdown(void) { g_pw_shutdowns++; pw_smc_seen(); }
static void pw_reboot(void *u) { (void)u; xenon_smc_power_reboot(); }
static void pw_shutdown(void *u) { (void)u; xenon_smc_power_shutdown(); }

static void power_in_queue(const char *base, int k) {
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/s%d", base, k);
    mkdir(dir, 0755);
    fprintf(stderr, "=== phase %d: PLAY then Reiniciar (top button) in the queue\n", k);
    phase_reset();
    CHECK(wait_can_play(25000) == 0, "idle before the power phase");
    pthread_mutex_lock(&g_gssv.mu);
    snprintf(g_gssv.next_dir, sizeof(g_gssv.next_dir), "%s", dir);
    g_gssv.extra_queue_polls = 20;
    g_pw_sess = g_gssv.nsess;
    pthread_mutex_unlock(&g_gssv.mu);
    CHECK(xn_play("HALO5", "Halo 5", 1) == XN_OK, "xn_play HALO5");
    uint64_t t0 = xcw_now_ms();
    while (xcw_now_ms() - t0 < 30000 && g_mm.state != MM_ST_QUEUE) { poll_state(); usleep(5000); }
    CHECK(g_mm.state == MM_ST_QUEUE, "reached QUEUE (%s)", g_mm.state_text);
    /* v0.20.0: botões do topo do menu novo (subir além da 1ª fileira ->
     * Reiniciar, A abre a confirmação em Cancelar, direita + A confirma) */
    static menu_model_t pm;
    mm_init(&pm);
    { char g[] = "GAME\tHALO5\tHalo 5\t0", e[] = "END"; mm_handle_line(&pm, g); mm_handle_line(&pm, e); }
    mm_nav(&pm, 0, -1);
    int a1 = mm_press_a(&pm);
    mm_nav(&pm, 1, 0);
    int a2 = mm_press_a(&pm);
    CHECK(a1 == MM_A_CONFIRM && a2 == MM_A_REBOOT, "up, A, right, A = Reiniciar (%d %d)", a1, a2);
    int it = a2 == MM_A_REBOOT ? MM_PWR_REBOOT : MM_PWR_BACK;
    const mm_power_ops_t ops = { NULL, pw_open, pw_stop, pw_done, pw_now, pw_wait, pw_show, pw_reboot, pw_shutdown };
    uint64_t ts = xcw_now_ms();
    int r = mm_power_run(&ops, it, MM_PWR_TIMEOUT_MS);
    uint64_t el = xcw_now_ms() - ts;
    fprintf(stderr, "    power: r=%d in %.2f s, DELETEs at SMC %d\n", r, el / 1000.0, g_pw_del_at_smc);
    CHECK(r == MM_PWR_CLEAN, "session closed before the reboot (r=%d, %.2f s)", r, el / 1000.0);
    CHECK(el <= MM_PWR_TIMEOUT_MS + 200, "within the timeout (%llu ms)", (unsigned long long)el);
    CHECK(g_pw_reboots == 1 && g_pw_shutdowns == 0, "SMC reboot once (%d/%d)", g_pw_reboots, g_pw_shutdowns);
    CHECK(g_pw_del_at_smc == 1, "DELETE already sent when the SMC was called (%d)", g_pw_del_at_smc);
    CHECK(xn_can_play() && xn_pc() == NULL, "nothing left running");
    CHECK(saw_state("IDLE", "Sessao encerrada"), "STATE IDLE Sessao encerrada");
}

/* ---- v0.19.3: Back+Start = menu com a sessão aberta ---- */
static int g_ka_idr_first;
/* = pull_next_au + decode: drain the ring; returns AUs */
static int ka_drain(void) {
    int n = 0;
    while (r_used() >= 6) {
        uint8_t h[6];
        uint32_t t = g_rt;
        for (int i = 0; i < 6; i++) h[i] = g_ring[(t + i) & (RING - 1)];
        uint32_t len = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
        if (r_used() < 6 + len) break;
        g_rt = (t + 6 + len) & (RING - 1);
        n++;
    }
    return n;
}
static void ka_stats(xcw_stats_t *s) {
    usleep(150000);   /* hw1 refreshes g_last every 100 ms */
    pthread_mutex_lock(&g_stmu);
    *s = g_last;
    pthread_mutex_unlock(&g_stmu);
}
static gssv_sess_t ka_sess(int i, int *nsess, int *nplay) {
    pthread_mutex_lock(&g_gssv.mu);
    gssv_sess_t s = g_gssv.s[i];
    if (nsess) *nsess = g_gssv.nsess;
    if (nplay) *nplay = g_gssv.n_play;
    pthread_mutex_unlock(&g_gssv.mu);
    return s;
}
/* PLAY (or resume) -> first AU in the ring. Returns ms + 1 (0 = none). */
static uint64_t ka_wait_video(int ms) {
    uint64_t t0 = xcw_now_ms();
    g_ka_idr_first = -1;
    while (xcw_now_ms() - t0 < (uint64_t)ms) {
        poll_state();
        if (g_mm.state == MM_ST_ERROR) break;
        if (r_used() >= 6) {
            uint8_t h[6];
            for (int i = 0; i < 6; i++) h[i] = g_ring[(g_rt + i) & (RING - 1)];
            g_ka_idr_first = h[4];
            return xcw_now_ms() - t0 + 1;
        }
        usleep(2000);
    }
    return 0;
}
static int ka_game(int ms) {   /* in game: decoder (hw3 lease) drains, pads forwarded */
    uint64_t t_wait = xcw_now_ms();
    while (xn_session_thread_busy() && xcw_now_ms() - t_wait < 2000) usleep(2000);
    dec_open();
    g_fwd_t0 = xcw_now_ms();
    g_forward = 1;
    int aus = 0;
    uint64_t t0 = xcw_now_ms(), t_dec = 0;
    while (xcw_now_ms() - t0 < (uint64_t)ms) {
        poll_state();
        if (xcw_now_ms() - t_dec >= 10) { t_dec = xcw_now_ms(); dec_tick(); }
        aus += ka_drain();
        usleep(2000);
    }
    return aus;
}
/* Back+Start as main.c v0.19.3 does it: pads off, xn_pause (neutral pads on
 * hw1), decoder closed, ring thrown away (menu_loop stream_discard). */
static void ka_back_start(void) {
    g_forward = 0;
    CHECK(xn_pause() == XN_OK, "Back+Start: xn_pause");
    usleep(30000);
    dec_close();
    g_rt = g_rh;
}

static void keep_session(const char *base, int k, const char *pion, const char *h264) {
    char dir[400], dir2[400], dir3[400];
    snprintf(dir, sizeof(dir), "%s/s%d", base, k);
    snprintf(dir2, sizeof(dir2), "%s/s%d", base, k + 1);
    snprintf(dir3, sizeof(dir3), "%s/s%d", base, k + 2);
    mkdir(dir, 0755);
    mkdir(dir2, 0755);
    mkdir(dir3, 0755);
    fprintf(stderr, "=== phase %d: Back+Start keeps the session, same game resumes, other game replaces it\n", k);
    phase_reset();
    CHECK(wait_can_play(25000) == 0, "idle before the keep phase");
    CHECK(xn_session(NULL, 0, NULL, 0) == XN_SESS_NONE, "xn_session NONE when idle");
    int nsess0, nplay0;
    pthread_mutex_lock(&g_gssv.mu);
    snprintf(g_gssv.next_dir, sizeof(g_gssv.next_dir), "%s", dir);
    g_gssv.extra_queue_polls = 0;
    int ia = g_gssv.nsess;
    pthread_mutex_unlock(&g_gssv.mu);
    pid_t pa = spawn_pion(pion, dir, h264, 150);
    CHECK(xn_play("SEAOFTHIEVES", "Sea of Thieves", 0) == XN_OK, "xn_play SEAOFTHIEVES");
    CHECK(xn_session(NULL, 0, NULL, 0) == XN_SESS_STARTING, "xn_session STARTING after PLAY");
    CHECK(ka_wait_video(60000) > 0, "video of game A (%s)", g_mm.state_text);
    int aus = ka_game(4000);
    CHECK(aus > 60, "game A streamed (%d AUs)", aus);
    char tid[48], tname[64];
    CHECK(xn_session(tid, sizeof(tid), tname, sizeof(tname)) == XN_SESS_PLAYING && !strcmp(tid, "SEAOFTHIEVES") &&
              !strcmp(tname, "Sea of Thieves"),
          "xn_session PLAYING SEAOFTHIEVES (%s / %s)", tid, tname);

    /* -- Back+Start: menu, session open -- */
    xcw_stats_t s0, s1;
    ka_back_start();
    ka_stats(&s0);
    gssv_sess_t ga0 = ka_sess(ia, NULL, NULL);
    int apk0 = g_audio_pkts;
    CHECK(xn_session(tid, sizeof(tid), NULL, 0) == XN_SESS_PAUSED && !strcmp(tid, "SEAOFTHIEVES"),
          "xn_session PAUSED SEAOFTHIEVES in the menu");
    CHECK(xn_resume("GEARS5") == XN_EBUSY, "resume of another title refused");
    CHECK(xn_play("GEARS5", "Gears 5", 0) == XN_EBUSY, "PLAY of another title refused while one is open");
    g_forward = 1;   /* a late pad frame from hw1 must be ignored (pads neutral) */
    int ring_in = 0, not_paused = 0;
    uint64_t t0 = xcw_now_ms();
    while (xcw_now_ms() - t0 < 6000) {
        poll_state();
        if (xn_session(NULL, 0, NULL, 0) != XN_SESS_PAUSED) not_paused++;
        if (r_used()) { ring_in += r_used(); g_rt = g_rh; }
        usleep(10000);
    }
    g_forward = 0;
    ka_stats(&s1);
    gssv_sess_t ga1 = ka_sess(ia, &nsess0, &nplay0);
    fprintf(stderr, "    menu 6 s: keepalives +%d, rtp +%u, paused_dropped +%u, kfrq +%u, pli +%u, ring %d B, audio +%d\n",
            ga1.n_ka - ga0.n_ka, s1.rtp_packets - s0.rtp_packets, s1.paused_dropped - s0.paused_dropped,
            s1.keyframe_requests - s0.keyframe_requests, s1.pli_sent - s0.pli_sent, ring_in, g_audio_pkts - apk0);
    CHECK(not_paused == 0, "PAUSED the whole time in the menu (%d)", not_paused);
    CHECK(ga1.n_ka - ga0.n_ka >= 4, "keepalive goes on in the menu (+%d in 6 s)", ga1.n_ka - ga0.n_ka);
    CHECK(ga1.n_delete == 0, "no DELETE in the menu (%d)", ga1.n_delete);
    CHECK(s1.rtp_packets > s0.rtp_packets + 100, "media still flowing (rtp +%u)", s1.rtp_packets - s0.rtp_packets);
    CHECK(s1.paused_dropped - s0.paused_dropped > 60, "AUs dropped in the receiver (+%u)",
          s1.paused_dropped - s0.paused_dropped);
    CHECK(ring_in == 0, "nothing reached the decoder ring (%d B)", ring_in);
    CHECK(g_audio_pkts == apk0, "no audio delivered in the menu (+%d)", g_audio_pkts - apk0);
    CHECK(s1.keyframe_requests == s0.keyframe_requests && s1.pli_sent == s0.pli_sent,
          "no keyframe request in the menu (kfrq +%u, pli +%u)", s1.keyframe_requests - s0.keyframe_requests,
          s1.pli_sent - s0.pli_sent);
    CHECK(xn_pc() != NULL, "PeerConnection kept");

    /* -- same game again: back at once, same session, PLI -- */
    CHECK(xn_resume("SEAOFTHIEVES") == XN_OK, "xn_resume SEAOFTHIEVES");
    uint64_t back = ka_wait_video(3000);
    fprintf(stderr, "    resume: first AU %llu ms after A, idr=%d\n", (unsigned long long)back, g_ka_idr_first);
    CHECK(back > 0 && back < 2000, "video back in %llu ms", (unsigned long long)back);
    CHECK(g_ka_idr_first == 1, "the first AU after the resume is an IDR (%d)", g_ka_idr_first);
    aus = ka_game(3000);
    xcw_stats_t s2;
    ka_stats(&s2);
    int nsess1, nplay1;
    gssv_sess_t ga2 = ka_sess(ia, &nsess1, &nplay1);
    CHECK(aus > 45, "game A again (%d AUs)", aus);
    CHECK(nsess1 == nsess0 && nplay1 == nplay0 && ga2.n_delete == 0, "same xCloud session (no /play, no DELETE)");
    CHECK(s2.pli_sent > s1.pli_sent && s2.keyframe_requests > s1.keyframe_requests, "PLI on resume (+%u)",
          s2.pli_sent - s1.pli_sent);
    CHECK(xn_session(NULL, 0, NULL, 0) == XN_SESS_PLAYING, "PLAYING again");

    /* -- Back+Start, then ANOTHER game: DELETE of A before /play of B -- */
    ka_back_start();
    uint64_t ts = xcw_now_ms();
    xn_stop();   /* main.c: A on another title = encerra a aberta, PLAY quando xn_can_play */
    CHECK(xn_session(NULL, 0, NULL, 0) == XN_SESS_ENDING, "ENDING after xn_stop");
    CHECK(wait_can_play(25000) == 0, "A closed in %.1f s", (xcw_now_ms() - ts) / 1000.0);
    CHECK(wait_pid(pa, 15000) == 0, "pion A finished after DELETE");
    pthread_mutex_lock(&g_gssv.mu);
    snprintf(g_gssv.next_dir, sizeof(g_gssv.next_dir), "%s", dir2);
    int ib = g_gssv.nsess;
    pthread_mutex_unlock(&g_gssv.mu);
    pid_t pb = spawn_pion(pion, dir2, h264, 150);
    CHECK(xn_play("GEARS5", "Gears 5", 0) == XN_OK, "xn_play GEARS5");
    CHECK(ka_wait_video(60000) > 0, "video of game B (%s)", g_mm.state_text);
    {
        int pd = -1, pp = -1;
        pthread_mutex_lock(&g_gssv.mu);
        for (int i = 0; i < g_gssv.nev; i++) {
            if (g_gssv.ev_kind[i] == 'D' && g_gssv.ev_sess[i] == ia && pd < 0) pd = i;
            if (g_gssv.ev_kind[i] == 'P' && g_gssv.ev_sess[i] == ib && pp < 0) pp = i;
        }
        pthread_mutex_unlock(&g_gssv.mu);
        CHECK(pd >= 0 && pp > pd, "DELETE of A (ev %d) before the /play of B (ev %d)", pd, pp);
    }
    gssv_sess_t ga3 = ka_sess(ia, NULL, NULL);
    CHECK(ga3.n_delete == 1, "A deleted once (%d)", ga3.n_delete);
    aus = ka_game(3000);
    CHECK(aus > 45, "game B streamed (%d AUs)", aus);

    /* -- Back+Start, then the xCloud ends B while it sits in the menu -- */
    ka_back_start();
    int nsess2, nplay2;
    ka_sess(ib, &nsess2, &nplay2);
    pthread_mutex_lock(&g_gssv.mu);
    g_gssv.ka_gone = ib + 1;
    pthread_mutex_unlock(&g_gssv.mu);
    fprintf(stderr, "    >>> server ends the paused session (keepalive 404)\n");
    ts = xcw_now_ms();
    int st = -1;
    while (xcw_now_ms() - ts < 25000) {
        poll_state();
        st = xn_session(NULL, 0, NULL, 0);   /* main.c calls it every menu loop: reaps */
        if (st == XN_SESS_NONE) break;
        usleep(10000);
    }
    poll_state();
    fprintf(stderr, "    paused session gone after %.1f s: %s\n", (xcw_now_ms() - ts) / 1000.0, g_mm.state_text);
    CHECK(st == XN_SESS_NONE, "state back to no session (%d)", st);
    CHECK(xn_can_play() && xn_pc() == NULL, "nothing left running");
    CHECK(g_mm.state == MM_ST_ERROR && strstr(g_mm.state_text, "encerrada pelo servidor"),
          "menu says the xCloud ended it (%s: %s)", g_mm.state_code, g_mm.state_text);
    int nsess3, nplay3;
    ka_sess(ib, &nsess3, &nplay3);
    CHECK(nsess3 == nsess2 && nplay3 == nplay2, "no new session opened by itself (%d/%d)", nsess3 - nsess2,
          nplay3 - nplay2);
    pthread_mutex_lock(&g_gssv.mu);
    g_gssv.ka_gone = 0;
    pthread_mutex_unlock(&g_gssv.mu);
    CHECK(wait_pid(pb, 15000) == 0, "pion B finished");

    /* -- B again: a new session, normally -- */
    pthread_mutex_lock(&g_gssv.mu);
    snprintf(g_gssv.next_dir, sizeof(g_gssv.next_dir), "%s", dir3);
    int ic = g_gssv.nsess;
    pthread_mutex_unlock(&g_gssv.mu);
    pid_t pc = spawn_pion(pion, dir3, h264, 120);
    CHECK(xn_resume("GEARS5") == XN_EBUSY, "nothing to resume");
    CHECK(xn_play("GEARS5", "Gears 5", 0) == XN_OK, "xn_play GEARS5 again");
    CHECK(ka_wait_video(60000) > 0, "video of the new B session (%s)", g_mm.state_text);
    aus = ka_game(2000);
    CHECK(aus > 30, "new B session streamed (%d AUs)", aus);
    g_forward = 0;
    g_neutral_req = 1;
    usleep(20000);
    xn_stop();
    usleep(30000);
    dec_close();
    CHECK(wait_can_play(25000) == 0, "final teardown");
    gssv_sess_t gc = ka_sess(ic, NULL, NULL);
    CHECK(ic == ib + 1 && gc.n_delete == 1, "one new session for the new PLAY, deleted at the end");
    CHECK(wait_pid(pc, 15000) == 0, "pion C finished");
}

int main(int argc, char **argv) {
    const char *base = NULL, *pion = NULL, *h264 = NULL, *cert = NULL, *key = NULL;
    int secs = 12;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) base = argv[++i];
        else if (!strcmp(argv[i], "--pion") && i + 1 < argc) pion = argv[++i];
        else if (!strcmp(argv[i], "--h264") && i + 1 < argc) h264 = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cert") && i + 1 < argc) cert = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
    }
    if (!base || !pion || !h264 || !cert || !key) {
        fprintf(stderr, "usage: %s --dir D --pion P --h264 F --cert C --key K [--seconds N]\n", argv[0]);
        return 2;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_mutex_init(&g_gssv.mu, NULL);
    mm_init(&g_mm);
    /* libpeer/usrsctp lines: xc_udp_posix.c xcw_log() -> stderr (Xbox: xcw_set_log_hook) */

    xn_platform_t plat = {plat_inject, plat_audio, rt_log_line, plat_before, plat_msg};
    /* consent 2500: pion only sends ICE binding requests while idle, so the
     * libpeer 10 s keepalive would close the call (xCloud sends them; the
     * Xbox build default is 0 = capture-rtp, knob NATIVE_CONSENT_MS). */
    xn_cfg_t cfg = {1500, 1000, 2500, 2};   /* v0.19.2: keepalive 1 s (many on one TLS conn) */
    xst_hw3_busy_hook = hw3_busy;
    CHECK(rt_tls_server_start(cert, key) == 0, "local TLS server (fake GSSV) up");
    CHECK(xn_init(&plat, &cfg) == XN_OK, "xn_init");
    pthread_t th;
    pthread_create(&th, NULL, hw1, NULL);

    play_game(base, 1, pion, h264, secs, "FORZAHORIZON5", "Forza Horizon 5", SC_401);
    cancel_in_queue(base, 2);
    play_game(base, 3, pion, h264, secs, "STARFIELD", "Starfield", SC_CLOSES);
    power_in_queue(base, 4);   /* v0.19.3 */
    keep_session(base, 5, pion, h264);   /* v0.19.3: s5..s7 */

    g_run = 0;
    pthread_join(th, NULL);

    const char *log = rt_log_text();
    int leak = strstr(log, "FAKE-REFRESH") || strstr(log, "FAKE-ACCESS") || strstr(log, "FAKE-XSTS") ||
               strstr(log, "FAKE-USERTOKEN") || strstr(log, "PASSPORT-FAKE") || strstr(log, "Bearer");
    for (const char *p = log; !leak && (p = strstr(p, "GS-")) != NULL; p += 3)
        if (p[3] >= '0' && p[3] <= '9') leak = 1;
    CHECK(!leak, "no token in any log line");
    CHECK(!strncmp(rt_token(), "FAKE-REFRESH-", 13) && atoi(rt_token() + 13) == 8,
          "refresh token rotated and saved once per session + once for the forced 401 (%s)", "TOKEN.TXT");
    {
        int full = log_count("(cpu ");
        fprintf(stderr, "    [tls] handshake lines in the app log: %d\n", full);
    }
    CHECK(g_gssv.n_play == 7, "7 /play total (%d)", g_gssv.n_play);   /* keep phase: A, B, B again (resume = none) */
    fprintf(stderr, "### standalone E2E: %d checks, %d failed\n", g_checks, g_fail);
    printf("STANDALONE E2E %s (%d checks, %d failed)\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
