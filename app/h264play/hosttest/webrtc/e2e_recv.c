/*
 * XCloud360 stage 4 — offline E2E receiver: the Xbox-side WebRTC code
 * (source/webrtc/xc_*.c + libpeer/usrsctp/libsrtp/mbedTLS from deps/webrtc-xenon)
 * driven exactly like the app will drive it, against a local test peer
 * (peer_pion/) instead of xCloud. Signaling = files in --dir:
 *
 *   offer.sdp        written by us      (munged offer)
 *   local.cands      written by us      (one "candidate:..." per line)
 *   answer.sdp       written by peer    (no a=candidate lines)
 *   remote.cands     written by peer    (one per line; tests trickle API)
 *
 * Outputs in --dir: recv.h264 (Annex-B AUs as delivered), recv.auidx
 * ("<bytes> <rtp_ts> <idr> <params_prepended>"), recv.opus
 * ([seq u16be][ts u32be][len u16be][payload]), recv.json (stats).
 *
 * usage: e2e_recv --dir D [--seconds N] [--kbps K] [--mt]
 *   --mt  drive it through xc_webrtc_mt.h: signaling + gamepad from this
 *         thread, xcwt_poll() on a second thread (= the Xbox layout).
 *   --xsm drive it through the stage 3 session mailbox (xsm_rtc_*, host copy
 *         in fake_xsm.c) + source/webrtc/xc_webrtc_xsm.c on a second thread.
 */
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xc_webrtc.h"
#include "xc_webrtc_mt.h"
#include "xc_webrtc_xsm.h"
#include "xcw_port.h"

/* fake_xsm.c: the session thread's side of the stage 3 mailbox */
void fx_request_offer(xsm_t *m);
char *fx_take_offer(xsm_t *m, char (*locals)[XS_CAND_LEN], int *nlocal);
void fx_publish(xsm_t *m, const char *answer, char (*remotes)[XS_CAND_LEN], int n);
void fx_events(xsm_t *m, int ev[5]);
static xsm_t g_m;
static xcws_t g_g;

unsigned long xcw_posix_dropped(void);
unsigned long xcw_posix_rx(void);
unsigned long xcw_posix_tx(void);

static const char *g_dir = ".";
static FILE *g_h264, *g_idx, *g_opus;
static xcw_t *g_x;

/* ping/pong over "chat" */
#define MAXPING 256
static uint64_t g_ping_sent_us[MAXPING];
static int g_pings, g_pongs;
static double g_rtt_sum_ms, g_rtt_max_ms;
static int g_ctrl_msgs, g_msg_msgs;

static char *path(const char *name) {
    static char buf[4][512];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", g_dir, name);
    return buf[k];
}

static void write_atomic(const char *name, const char *data) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path(name));
    FILE *f = fopen(tmp, "wb");
    if (!f) { perror(tmp); exit(3); }
    fputs(data, f);
    fclose(f);
    rename(tmp, path(name));
}

static char *read_file_wait(const char *name, int timeout_s) {
    for (int i = 0; i < timeout_s * 100; i++) {
        FILE *f = fopen(path(name), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *b = (char *)malloc((size_t)n + 1);
            size_t got = fread(b, 1, (size_t)n, f);
            b[got] = 0;
            fclose(f);
            return b;
        }
        usleep(10000);
    }
    return NULL;
}

static void on_au(void *u, const uint8_t *au, size_t n, const xcw_au_info_t *info) {
    (void)u;
    fwrite(au, 1, n, g_h264);
    fprintf(g_idx, "%zu %u %u %u\n", n, info->rtp_ts, info->idr, info->params_prepended);
}

static void on_audio(void *u, uint16_t seq, uint32_t ts, const uint8_t *p, size_t n) {
    (void)u;
    uint8_t h[8] = {(uint8_t)(seq >> 8), (uint8_t)seq, (uint8_t)(ts >> 24), (uint8_t)(ts >> 16),
                    (uint8_t)(ts >> 8), (uint8_t)ts, (uint8_t)(n >> 8), (uint8_t)n};
    fwrite(h, 1, 8, g_opus);
    fwrite(p, 1, n, g_opus);
}

static void on_msg(void *u, xcw_channel_t ch, const uint8_t *d, size_t n) {
    (void)u;
    if (ch == XCW_CH_CONTROL) g_ctrl_msgs++;
    if (ch == XCW_CH_MESSAGE) g_msg_msgs++;
    if (ch == XCW_CH_CHAT && n > 5 && memcmp(d, "pong ", 5) == 0) {
        int k = atoi((const char *)d + 5);
        if (k >= 0 && k < MAXPING && g_ping_sent_us[k]) {
            double rtt = (xcw_now_us() - g_ping_sent_us[k]) / 1000.0;
            g_rtt_sum_ms += rtt;
            if (rtt > g_rtt_max_ms) g_rtt_max_ms = rtt;
            g_pongs++;
        }
    }
}

/* the work the app does ON the WebRTC/network thread besides polling */
static int g_mt;
static volatile int g_net_run;
static uint64_t g_last_ping;
static void net_side(xcw_t *x) {
    uint64_t now = xcw_now_ms();
    if (xcw_state(x) == XCW_STREAMING && now - g_last_ping >= 250 && g_pings < MAXPING) {
        g_last_ping = now;
        char m[64];
        int n = snprintf(m, sizeof(m), "ping %d", g_pings);
        g_ping_sent_us[g_pings] = xcw_now_us();
        if (xcw_send(x, XCW_CH_CHAT, m, (size_t)n, 1) == 0)
            g_pings++;
        else
            g_ping_sent_us[g_pings] = 0;
    }
}
/* --mt / --xsm: plays the hw1 network thread (every ~0.5 ms) */
static int g_xsm;
static void *net_thread(void *a) {
    (void)a;
    while (g_net_run) {
        xcw_t *x;
        if (g_xsm) {
            xcws_step(&g_g);   /* = what net_step() will call on the Xbox */
            x = xcws_pc(&g_g);
        } else {
            xcwt_poll();
            x = xcwt_net_handle();
        }
        if (x) net_side(x);
        usleep(500);
    }
    return NULL;
}

/* heap: glibc arena + mmap'd blocks in use, sampled every second */
static size_t heap_now(void) {
    struct mallinfo2 mi = mallinfo2();
    return mi.uordblks + mi.hblkhd;
}
static size_t g_heap_base, g_heap_peak;

static int real_main(int argc, char **argv);
static int g_argc, g_rc;
static char **g_argv;
static void *thr(void *a) {
    (void)a;
    g_rc = real_main(g_argc, g_argv);
    return NULL;
}

/* Runs the receiver on a thread whose stack is painted, so the report can
 * give the real stack high-water mark of everything xcw_poll() calls. */
#define STACK_SZ (1024 * 1024)
static size_t g_stack_used;
int main(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
    uint8_t *stk = (uint8_t *)malloc(STACK_SZ);
    memset(stk, 0xA5, STACK_SZ);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstack(&at, stk, STACK_SZ);
    pthread_t t;
    if (pthread_create(&t, &at, thr, NULL) != 0) return 9;
    pthread_join(t, NULL);
    size_t untouched = 0;
    while (untouched < STACK_SZ && stk[untouched] == 0xA5) untouched++;
    g_stack_used = STACK_SZ - untouched;
    FILE *f = fopen(path("recv.mem"), "w");
    if (f) {
        fprintf(f, "{\"stack_peak_bytes\":%zu,\"heap_base_bytes\":%zu,\"heap_peak_bytes\":%zu}\n", g_stack_used,
                g_heap_base, g_heap_peak);
        fclose(f);
    }
    fprintf(stderr, "[e2e] memory: stack high-water %zu bytes, heap peak %zu bytes above a %zu byte baseline\n",
            g_stack_used, g_heap_peak - g_heap_base, g_heap_base);
    return g_rc;
}

static int real_main(int argc, char **argv) {
    int seconds = 20, kbps = 3000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) g_dir = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--kbps") && i + 1 < argc) kbps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mt")) g_mt = 1;
        else if (!strcmp(argv[i], "--xsm")) g_mt = g_xsm = 1;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    g_h264 = fopen(path("recv.h264"), "wb");
    g_idx = fopen(path("recv.auidx"), "w");
    g_opus = fopen(path("recv.opus"), "wb");
    if (!g_h264 || !g_idx || !g_opus) { perror("open outputs"); return 2; }

    if (xcw_global_init() != 0) { fprintf(stderr, "global init failed\n"); return 2; }
    xcw_cfg_t cfg;
    xcw_default_cfg(&cfg);
    cfg.video_kbps = kbps;
    /* pion only sends ICE binding requests while the pair is idle, so the
     * libpeer 10 s keepalive rule would close the call; xCloud sends them. */
    cfg.consent_interval_ms = getenv("XCW_CONSENT_MS") ? atoi(getenv("XCW_CONSENT_MS")) : 2500;
    cfg.on_video_au = on_au;
    cfg.on_audio = on_audio;
    cfg.on_channel_msg = on_msg;

    /* ---- offer (single-thread: direct API; --mt: xc_webrtc_mt.h from this
     * "session" thread while a second thread plays the hw1 network thread) */
    static char offer[XCWT_SDP_MAX], ufrag[64], cands[XCWT_CAND_MAX][192];
    int ncand = 0;
    pthread_t nt;
    uint64_t t0 = xcw_now_us();
    if (g_mt) {
        g_net_run = 1;
        pthread_create(&nt, NULL, net_thread, NULL);
        if (g_xsm) {
            /* stage 3 mailbox: the session asks for an offer, the hw1 glue
             * (xc_webrtc_xsm.c) answers with offer + local candidates */
            xcws_init(&g_g, &g_m, &cfg);
            fx_request_offer(&g_m);
            static char loc[XSM_MAX_LOCAL][XS_CAND_LEN];
            int nl = 0;
            char *o = NULL;
            for (int k = 0; k < 5000 && !(o = fx_take_offer(&g_m, loc, &nl)); k++) usleep(1000);
            if (!o) { fprintf(stderr, "xsm: no offer\n"); return 2; }
            snprintf(offer, sizeof(offer), "%s", o);
            free(o);
            for (ncand = 0; ncand < nl && ncand < XCWT_CAND_MAX; ncand++)
                snprintf(cands[ncand], sizeof(cands[ncand]), "%s", loc[ncand]);
            const char *u = strstr(offer, "a=ice-ufrag:");
            snprintf(ufrag, sizeof(ufrag), "%.*s", u ? (int)strcspn(u + 12, "\r\n") : 0, u ? u + 12 : "");
        } else {
            if (xcwt_start(&cfg) != 0) { fprintf(stderr, "xcwt_start failed\n"); return 2; }
            ncand = xcwt_wait_offer(offer, sizeof(offer), ufrag, sizeof(ufrag), cands, XCWT_CAND_MAX, 5000);
            if (ncand < 0) { fprintf(stderr, "xcwt_wait_offer failed\n"); return 2; }
        }
    } else {
        g_x = xcw_create(&cfg);
        if (!g_x) { fprintf(stderr, "xcw_create failed\n"); return 2; }
        const char *o = xcw_create_offer(g_x);
        if (!o) { fprintf(stderr, "create_offer failed\n"); return 2; }
        snprintf(offer, sizeof(offer), "%s", o);
        snprintf(ufrag, sizeof(ufrag), "%s", xcw_local_ufrag(g_x));
        for (; ncand < xcw_local_candidate_count(g_x) && ncand < XCWT_CAND_MAX; ncand++)
            snprintf(cands[ncand], sizeof(cands[ncand]), "%s", xcw_local_candidate(g_x, ncand));
    }
    double t_offer_ms = (xcw_now_us() - t0) / 1000.0;
    write_atomic("offer.sdp", offer);
    {
        char cl[2048] = "";
        for (int i = 0; i < ncand; i++) {
            strncat(cl, cands[i], sizeof(cl) - strlen(cl) - 2);
            strcat(cl, "\n");
        }
        write_atomic("local.cands", cl);
    }
    fprintf(stderr, "[e2e] %soffer written (%zu bytes, %d candidates, ufrag %s, %.1f ms)\n",
            g_mt ? "[mt] " : "", strlen(offer), ncand, ufrag, t_offer_ms);

    char *answer = read_file_wait("answer.sdp", 60);
    char *rc = read_file_wait("remote.cands", 60);
    if (!answer || !rc) { fprintf(stderr, "[e2e] no answer/candidates from peer\n"); return 4; }
    int nrc = 0;
    static char rem[XSM_MAX_REMOTE][XS_CAND_LEN];
    for (char *line = strtok(rc, "\n"); line; line = strtok(NULL, "\n")) {
        if (g_xsm) {
            if (nrc < XSM_MAX_REMOTE) snprintf(rem[nrc++], XS_CAND_LEN, "%s", line);
        } else if ((g_mt ? xcwt_add_remote_candidate(line) : xcw_add_remote_candidate(g_x, line)) == 0) {
            nrc++;
        }
    }
    fprintf(stderr, "[e2e] answer %zu bytes, %d remote candidates\n", strlen(answer), nrc);
    if (g_xsm)
        fx_publish(&g_m, answer, rem, nrc);   /* ICE_BRIDGE done: remotes + answer */
    else if (g_mt)
        xcwt_set_answer(answer);
    else
        xcw_set_answer(g_x, answer);

    uint64_t start = xcw_now_ms(), last_tick = start, last_pad = 0;
    xcp_gamepad_t pad;
    memset(&pad, 0, sizeof(pad));
    int pad_step = 0;
    while (xcw_now_ms() - start < (uint64_t)seconds * 1000) {
        if (!g_mt) {
            xcw_poll(g_x);
            net_side(g_x);
        }
        uint64_t now = xcw_now_ms();
        xcw_state_t st;
        if (g_xsm) {
            xcw_t *px = xcws_pc(&g_g);
            st = px ? xcw_state(px) : XCW_NEW;
        } else {
            st = g_mt ? xcwt_state() : xcw_state(g_x);
        }
        if (st == XCW_FAILED || st == XCW_CLOSED) break;
        if (st == XCW_STREAMING && now - last_pad >= 100) {   /* synthetic input: A toggles, LX sweeps */
            last_pad = now;
            pad_step++;
            pad.a = (uint8_t)(pad_step & 1);
            pad.left_x = (float)((pad_step % 21) - 10) / 10.0f;
            pad.right_trigger = (float)(pad_step % 11) / 10.0f;
            if (g_xsm && xcws_pc(&g_g))
                xcw_set_gamepad(xcws_pc(&g_g), &pad);   /* [any thread] */
            else if (g_mt)
                xcwt_set_gamepad(&pad);   /* cross-thread: seqlock in xc_webrtc.c */
            else
                xcw_set_gamepad(g_x, &pad);
        }
        if (now - last_tick >= 1000 && g_xsm) {
            last_tick = now;
            int ev[5];
            fx_events(&g_m, ev);
            fprintf(stderr, "[e2e] t+%2us xsm events ice_up=%d media_up=%d video=%d dead=%d dropped=%d\n",
                    (unsigned)((now - start) / 1000), ev[0], ev[1], ev[2], ev[3], ev[4]);
        }
        if (now - last_tick >= 1000) {
            last_tick = now;
            size_t h = heap_now();
            if (h > g_heap_peak) g_heap_peak = h;
            xcw_stats_t s;
            if (g_mt)
                xcwt_stats(&s);
            else
                xcw_get_stats(g_x, &s);
            fprintf(stderr,
                    "[e2e] t+%2us %s rtp=%u aus=%u idr=%u nacks=%u jitdrop=%u resync=%u audio=%u in=%u "
                    "dc_rx(ctl=%u msg=%u chat=%u) srtp_err=%u udp_drop=%lu\n",
                    (unsigned)((now - start) / 1000), xcw_state_name(s.state), s.rtp_packets, s.aus, s.idrs,
                    s.nacks_sent, s.jit_dropped, s.jit_resyncs, s.audio_packets, s.input_sent,
                    s.dc_rx[XCW_CH_CONTROL], s.dc_rx[XCW_CH_MESSAGE], s.dc_rx[XCW_CH_CHAT], s.srtp_errors,
                    xcw_posix_dropped());
        }
        usleep(g_mt ? 1000 : 500);   /* the Xbox net thread polls every ~1 ms */
    }

    xcw_stats_t s;
    if (g_xsm) {
        g_net_run = 0;
        pthread_join(nt, NULL);
        if (xcws_pc(&g_g)) xcw_get_stats(xcws_pc(&g_g), &s);
        else memset(&s, 0, sizeof(s));
        int ev[5];
        fx_events(&g_m, ev);
        fprintf(stderr, "[e2e] xsm events: ice_up=%d media_up=%d video=%d dead=%d dropped=%d\n", ev[0], ev[1],
                ev[2], ev[3], ev[4]);
    } else if (g_mt) {
        g_net_run = 0;
        pthread_join(nt, NULL);
        xcwt_stats(&s);
    } else {
        xcw_get_stats(g_x, &s);
    }
    fflush(g_h264);
    fflush(g_idx);
    fflush(g_opus);
    FILE *j = fopen(path("recv.json"), "w");
    fprintf(j,
            "{\"state\":\"%s\",\"fail_reason\":\"%s\",\"t_offer_ms\":%.1f,\"t_ice_ms\":%u,\"t_dtls_ms\":%u,"
            "\"t_sctp_ms\":%u,\"t_handshake_ms\":%u,\"t_first_au_ms\":%u,"
            "\"rtp_packets\":%u,\"aus\":%u,\"idrs\":%u,\"video_bytes\":%llu,\"gated_dropped\":%u,"
            "\"jit_dropped\":%u,\"jit_resyncs\":%u,\"jit_late\":%u,\"jit_dup\":%u,\"jit_pool_full\":%u,"
            "\"nacks_sent\":%u,\"pli_sent\":%u,\"keyframe_requests\":%u,\"remb_sent\":%u,\"rr_sent\":%u,"
            "\"audio_packets\":%u,\"audio_bytes\":%llu,\"srtp_errors\":%u,\"remote_pli\":%u,"
            "\"dc_rx\":[%u,%u,%u,%u],\"dc_tx\":[%u,%u,%u,%u],\"dc_tx_fail\":%u,"
            "\"input_sent\":%u,\"input_fail\":%u,\"input_seq\":%u,"
            "\"pings\":%d,\"pongs\":%d,\"rtt_avg_ms\":%.3f,\"rtt_max_ms\":%.3f,"
            "\"udp_rx\":%lu,\"udp_tx\":%lu,\"udp_dropped\":%lu}\n",
            xcw_state_name(s.state), s.fail_reason ? s.fail_reason : "", t_offer_ms, s.t_ice_ms, s.t_dtls_ms,
            s.t_sctp_ms, s.t_handshake_ms, s.t_first_au_ms, s.rtp_packets, s.aus, s.idrs,
            (unsigned long long)s.video_bytes, s.gated_dropped, s.jit_dropped, s.jit_resyncs, s.jit_late, s.jit_dup,
            s.jit_pool_full, s.nacks_sent, s.pli_sent, s.keyframe_requests, s.remb_sent, s.rr_sent,
            s.audio_packets, (unsigned long long)s.audio_bytes, s.srtp_errors, s.remote_pli, s.dc_rx[0], s.dc_rx[1],
            s.dc_rx[2], s.dc_rx[3], s.dc_tx[0], s.dc_tx[1], s.dc_tx[2], s.dc_tx[3], s.dc_tx_fail, s.input_sent,
            s.input_fail, s.input_seq, g_pings, g_pongs, g_pongs ? g_rtt_sum_ms / g_pongs : 0.0, g_rtt_max_ms,
            xcw_posix_rx(), xcw_posix_tx(), xcw_posix_dropped());
    fclose(j);
    write_atomic("recv.done", "1\n");
    fprintf(stderr, "[e2e] done: state=%s aus=%u nacks=%u pongs=%d/%d\n", xcw_state_name(s.state), s.aus,
            s.nacks_sent, g_pongs, g_pings);
    if (g_xsm) {
        xcws_shutdown(&g_g);
    } else if (g_mt) {
        xcwt_stop();
        xcwt_poll();   /* the network thread is gone: this thread takes over */
    } else {
        xcw_destroy(g_x);
    }
    xcw_global_deinit();
    return s.state == XCW_STREAMING ? 0 : 1;
}
