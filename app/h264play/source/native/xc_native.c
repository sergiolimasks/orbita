/* XCloud360 stage 5 — see xc_native.h. Our code (no third-party source). */
#include "xc_native.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../session/xs_machine.h"
#include "../session/xs_port.h"
#include "../session/xs_task.h"
#include "../login/https.h"
#include "../webrtc/xc_proto.h"
#include "../webrtc/xc_webrtc.h"
#include "../webrtc/xc_webrtc_xsm.h"
#include "xcw_port.h"

#ifndef XN_STATUS_PERIOD_MS
#define XN_STATUS_PERIOD_MS 2000   /* setup-phase status line (no STAT yet) */
#endif
#define XN_LOG_BURST 60            /* xcw lines per second before rate-limiting */

static xn_platform_t g_p;
static xn_cfg_t      g_cfg;
static xcw_cfg_t     g_xcfg;
static int           g_inited;
static void (*g_load_probe)(uint64_t *, uint64_t *);

/* main -> hw1 */
static volatile int      g_want;       /* 1 = attach to the session machine */
/* v0.19.2: who owns hw3 (xs_lock). */
enum { HW3_FREE = 0, HW3_DECODER = 1, HW3_SESSION = 2 };
static int               g_hw3;
static volatile int      g_dec_thr;    /* main: current decoder threads (STAT) */
/* hw1: session driving */
static uint64_t g_sess_us;             /* hw1 time in xsm_net_step + spawn decisions */
static uint64_t g_t_net_since;         /* ms: owner became NET (0 = not NET) */
static uint64_t g_last_wait_log;
static unsigned g_respawns;
static int      g_own_seen = -1;
static volatile unsigned g_play_seq;
static volatile int      g_kf_req, g_kf_force;
/* hw1 -> main */
static volatile int      g_att;        /* hw1 is attached (owns a PC or may create one) */
/* v0.19.3: main -> hw1, jogo pausado no menu (xn_pause / xn_resume) */
static volatile int      g_paused;
static char              g_title[48], g_name[64];   /* main: o do último xn_play */
/* hw1: pausa já aplicada em qual PC (PC novo nasce sem pausa) */
static xcw_t   *g_pause_pc;
static int      g_pause_on;
static uint32_t g_pause_aus, g_pause_audio;

/* hw1 only */
static xcws_t   g_g;
static unsigned g_att_seq;
static unsigned g_pc_gen_seen;
static xcw_t   *g_pc_seen;
static uint32_t g_pads_used;
static uint64_t g_t_att_ms, g_next_status_ms, g_next_snap_ms, g_last_drop_log_ms;
static uint32_t g_srtp_logged;
static int      g_first_au_logged;
static uint64_t g_log_win_ms;
static int      g_log_n, g_log_dropped;
static int      g_last_xcw_state = -1;

/* shared stats (xs_lock) */
typedef struct {
    xcw_stats_t s;
    int      have;
    uint32_t inj_aus, inj_drops, a_pkts, pcs;
    uint64_t inj_bytes, rtc_us;
    uint32_t steps;
    uint64_t sess_us;              /* v0.19.2 */
    int      owner;
    unsigned tls_full, tls_req, respawns;
    unsigned tls_max_poll_us;
} xn_shared_t;
static xn_shared_t g_sh;
/* hw1 accumulators, copied into g_sh by snapshot() */
static uint32_t g_inj_aus, g_inj_drops, g_a_pkts, g_pcs;
static uint64_t g_inj_bytes, g_rtc_us;
static uint32_t g_steps;

static inline void fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_p.log) g_p.log(line);
}

/* ------------------------------------------------------- xcw callbacks */

/* xcw lines (state changes, SDP, ICE, dc, SPS/PPS...): to the PC LOG, with a
 * burst limit so a chatty channel cannot flood the 64-line UDP log queue. */
static void cb_log(void *u, const char *line) {
    (void)u;
    uint64_t now = xcw_now_ms();
    if (now - g_log_win_ms >= 1000) {
        if (g_log_dropped) logf_("[xn] (%d linhas do webrtc suprimidas)", g_log_dropped);
        g_log_win_ms = now;
        g_log_n = g_log_dropped = 0;
    }
    if (++g_log_n > XN_LOG_BURST) { g_log_dropped++; return; }
    if (g_p.log) g_p.log(line);
}

static void cb_au(void *u, const uint8_t *au, size_t n, const xcw_au_info_t *info) {
    (void)u;
    /* v0.19.3: pausado (a hw1 ainda não aplicou no PC): fora, sem resync */
    if (g_paused) { g_pause_aus++; return; }
    if (g_p.inject_au && g_p.inject_au(au, n, info->idr, info->params_prepended) == 0) {
        g_inj_aus++;
        g_inj_bytes += n;
        if (!g_first_au_logged) {
            g_first_au_logged = 1;
            logf_("[xn] 1o AU no decoder: %u B idr=%u params=%u (%ums depois do attach)", (unsigned)n, info->idr,
                  info->params_prepended, (unsigned)(xcw_now_ms() - g_t_att_ms));
        }
        return;
    }
    /* decoder ring full (decoder behind / still opening): drop, wait for an
     * IDR and ask for one (capture-rtp TcpOutServer congestion rule). */
    g_inj_drops++;
    xcw_t *x = xcws_pc(&g_g);
    if (x) xcw_resync_video(x);
    uint64_t now = xcw_now_ms();
    if (now - g_last_drop_log_ms >= 1000) {
        g_last_drop_log_ms = now;
        logf_("[xn] anel do decoder cheio: AU %u B descartado (total %u), esperando IDR", (unsigned)n,
              (unsigned)g_inj_drops);
    }
}

static void cb_audio(void *u, uint16_t seq, uint32_t ts, const uint8_t *p, size_t n) {
    (void)u;
    g_a_pkts++;
    if (g_paused) { g_pause_audio++; return; }   /* v0.19.3: menu é mudo */
    if (g_p.audio) g_p.audio(seq, ts, p, n);
}

/* v0.21.11: video lost in game. The WebRTC died (closed/failed) or the
 * session is waiting for hw3 while the decoder holds it: inside the game
 * nothing can bring it back (re-signaling needs the session thread on hw3),
 * so the game loop leaves after a few seconds (xn_video_lost_ms). */
static volatile uint64_t g_rtc_dead_since = 0, g_hw3_wait_since = 0;

static void cb_state(void *u, xcw_state_t st) {
    (void)u;
    g_last_xcw_state = (int)st;   /* the line itself comes through cb_log */
    if (st == XCW_FAILED || st == XCW_CLOSED) {
        if (!g_rtc_dead_since) g_rtc_dead_since = xcw_now_ms();
    } else {
        g_rtc_dead_since = 0;
    }
}

int xn_video_lost_ms(void) {
    uint64_t now = xcw_now_ms(), a = g_rtc_dead_since, b = g_hw3_wait_since;
    uint64_t da = a && now > a ? now - a : 0, db = b && now > b ? now - b : 0;
    uint64_t d = da > db ? da : db;
    return d > 0x7fffffff ? 0x7fffffff : (int)d;
}

static void cb_msg(void *u, xcw_channel_t ch, const uint8_t *d, size_t n) {
    (void)u;
    if (g_p.channel_msg) g_p.channel_msg((int)ch, d, n);
}

/* ------------------------------------------------------------ main side */

int xn_init(const xn_platform_t *p, const xn_cfg_t *cfg) {
    if (g_inited) return XN_OK;
    g_p = *p;
    g_cfg = *cfg;
    if (g_cfg.video_kbps <= 0) g_cfg.video_kbps = 1500;
    if (g_cfg.keepalive_ms <= 0) g_cfg.keepalive_ms = 5000;
    if (g_cfg.max_attempts <= 0) g_cfg.max_attempts = 2;
    if (xcw_global_init() != 0) {
        logf_("[xn] xcw_global_init falhou (usrsctp/libsrtp)");
        return XN_EINIT;
    }
    xcw_default_cfg(&g_xcfg);
    g_xcfg.video_kbps = g_cfg.video_kbps;   /* launcher --kbps 1500 */
    g_xcfg.video_w = 1280;
    g_xcfg.video_h = 720;
    g_xcfg.video_fps = 30;
    g_xcfg.resolution_alias = "720";
    g_xcfg.stun_url = NULL;                 /* host candidate only, like the PC path */
    g_xcfg.consent_interval_ms = g_cfg.consent_ms;
    g_xcfg.on_video_au = cb_au;
    g_xcfg.on_audio = cb_audio;
    g_xcfg.on_state = cb_state;
    g_xcfg.on_log = cb_log;
    g_xcfg.on_channel_msg = cb_msg;
    g_inited = 1;
    logf_("[xn] nativo pronto: %d kbps, keepalive %d ms, consent %d ms, %d tentativas", g_cfg.video_kbps,
          g_cfg.keepalive_ms, g_cfg.consent_ms, g_cfg.max_attempts);
    return XN_OK;
}

/* v0.19.2: a session whose live phases sit on hw1 has no thread running
 * but is still alive (owner != DONE). */
/* v0.20.4: a hw 3 também é usada pelo worker das capas, pela atualização e
 * pela validação da chave beta; com eles lá, xst_running() dava "sessão
 * ocupada" sem jogo nenhum ("Feche o jogo aberto antes de atualizar").
 * O main diz quem mais está na hw 3; o padrão (hosttests) é ninguém. */
__attribute__((weak)) int xc_hw3_other_owner(void) { return 0; }
static int sess_thread_running(void) { return xst_running() && !xc_hw3_other_owner(); }

int xn_can_play(void) { return g_inited && !g_want && !g_att && !sess_thread_running() && xst_owner() == XSM_OWN_DONE; }
int xn_busy(void) { return g_inited && (g_want || g_att || sess_thread_running() || xst_owner() != XSM_OWN_DONE); }
int xn_session_thread_busy(void) { return xst_running(); }

static int owner_locked(void) {   /* xs_lock held */
    xsm_t *m = xst_machine();
    return m ? m->owner : XSM_OWN_DONE;
}

int xn_dec_claim_hw3(void) {
    int ok;
    int busy = xst_running();
    xs_lock();
    ok = g_hw3 == HW3_FREE && !busy && owner_locked() != XSM_OWN_WANT_THREAD;
    if (ok) g_hw3 = HW3_DECODER;
    xs_unlock();
    return ok;
}

void xn_dec_release_hw3(void) {
    xs_lock();
    if (g_hw3 == HW3_DECODER) g_hw3 = HW3_FREE;
    xs_unlock();
}

void xn_set_dec_threads(int n) { g_dec_thr = n; }

/* v0.21.25: estado da hw 3 pro [dec] prof (por que o decoder tem 3 ou 4) */
void xn_hw3_diag(char *o, size_t n) {
    if (!g_inited) { snprintf(o, n, "hw3 ?"); return; }
    int busy = xst_running();
    xs_lock();
    int own = owner_locked(), hw3 = g_hw3;
    xs_unlock();
    snprintf(o, n, "hw3=%s sessao=%s%s", hw3 == HW3_DECODER ? "decoder" : hw3 == HW3_SESSION ? "sessao" : "livre",
             own == XSM_OWN_NET ? "hw1" : own == XSM_OWN_WANT_THREAD ? "quer-hw3" : own == XSM_OWN_DONE ? "fim" : "thread",
             busy ? " thread-viva" : "");
}

int xn_dec_threads_wanted(int cur, int max) {
    if (!g_inited || max < 2) return cur;
    int busy = xst_running();
    xs_lock();
    int own = owner_locked(), hw3 = g_hw3;
    xsm_t *m = xst_machine();
    int streaming = m && m->phase == XSM_P_STREAMING;
    xs_unlock();
    if (cur >= max && hw3 == HW3_DECODER && own == XSM_OWN_WANT_THREAD) return max - 1;
    /* back to max only after the session is settled on hw1 again (no
     * flapping: each reopen costs an IDR) */
    if (cur < max && own == XSM_OWN_NET && hw3 == HW3_FREE && !busy && streaming && g_t_net_since &&
        xcw_now_ms() - g_t_net_since >= 3000)
        return max;
    return cur;
}

int xn_play(const char *title_id, const char *name, int f2p) {
    if (!g_inited) return XN_EINIT;
    if (!xn_can_play()) return XN_EBUSY;
    if (g_p.before_session) g_p.before_session();
    xsm_cfg_t c;
    xsm_cfg_defaults(&c, title_id, name);
    c.f2p = f2p;
    c.active_policy = XSM_ACTIVE_CANCEL;   /* = capture-rtp cleanup_stale_sessions */
    c.keepalive_ms = g_cfg.keepalive_ms;
    c.max_attempts = g_cfg.max_attempts;
    c.net_live = 1;                        /* v0.19.2: fases ao vivo na hw1 */
    xs_lock();
    int hw3 = g_hw3;
    if (hw3 == HW3_FREE) g_hw3 = HW3_SESSION;
    xs_unlock();
    if (hw3 != HW3_FREE) {
        logf_("[xn] PLAY %s: hw3 ainda e do decoder", title_id);
        return XN_EBUSY;
    }
    if (xst_start_cfg(&c, NULL) != 0) {
        xs_lock();
        g_hw3 = HW3_FREE;
        xs_unlock();
        logf_("[xn] PLAY %s: a thread da sessao nao subiu", title_id);
        return XN_ESTART;
    }
    g_kf_req = g_kf_force = 0;
    g_play_seq++;
    g_paused = 0;   /* v0.19.3 */
    snprintf(g_title, sizeof(g_title), "%s", title_id);
    snprintf(g_name, sizeof(g_name), "%s", name ? name : "");
    fence();
    g_want = 1;   /* hw1 attaches on its next step */
    logf_("[xn] PLAY %s (%s)%s: sessao no Xbox", title_id, name, f2p ? " f2p" : "");
    return XN_OK;
}

void xn_stop(void) {
    if (!g_inited) return;
    if (g_want || xst_running() || xst_owner() != XSM_OWN_DONE) logf_("[xn] STOP: DELETE da sessao + webrtc desligado");
    g_want = 0;
    fence();
    xst_stop();
}

unsigned xn_state_line(char *out, size_t cap) { return xst_state_line(out, cap); }

/* ---- v0.19.3: pausa no menu ---- */

/* A thread da sessão saiu e a máquina terminou (DONE/ERROR): só falta
 * soltar o WebRTC. */
static int machine_finished(void) { return !xst_running() && xst_owner() == XSM_OWN_DONE; }

int xn_pause(void) {
    if (!g_inited || !g_want || machine_finished()) return XN_EBUSY;
    xsm_t *m = xst_machine();
    if (m) xsm_set_keep(m, 1);
    fence();
    g_paused = 1;
    fence();
    logf_("[xn] PAUSA: %s segue aberto no menu (keepalive), video/audio descartados", g_title);
    return XN_OK;
}

int xn_resume(const char *title_id) {
    if (!g_inited || !g_want || !g_paused || machine_finished()) return XN_EBUSY;
    if (!title_id || strcmp(title_id, g_title)) return XN_EBUSY;
    xsm_t *m = xst_machine();
    if (m) xsm_set_keep(m, 0);
    fence();
    g_paused = 0;   /* a hw1 liga o IDR gate e pede o keyframe (apply_pause) */
    fence();
    logf_("[xn] VOLTA: %s na mesma sessao (sem /play), pedindo keyframe; %u AUs / %u audio descartados no menu",
          g_title, (unsigned)g_pause_aus, (unsigned)g_pause_audio);
    return XN_OK;
}

int xn_session(char *title_id, size_t tn, char *name, size_t nn) {
    int st = XN_SESS_NONE;
    if (g_inited) {
        if (g_want && g_paused && machine_finished()) {
            /* o xCloud derrubou com o jogo no menu: solta o WebRTC já */
            logf_("[xn] sessao pausada de %s acabou: webrtc desligado", g_title);
            xn_stop();
        }
        if (g_want) {
            if (g_paused) {
                st = XN_SESS_PAUSED;
            } else if (machine_finished()) {
                st = XN_SESS_ENDING;   /* acabou no jogo: o main faz o xn_stop */
            } else {
                xsm_t *m = xst_machine();
                xs_lock();
                int streaming = m && m->phase == XSM_P_STREAMING;
                xs_unlock();
                st = streaming ? XN_SESS_PLAYING : XN_SESS_STARTING;
            }
        } else if (xn_busy()) {
            st = XN_SESS_ENDING;
        }
    }
    if (title_id && tn) snprintf(title_id, tn, "%s", st ? g_title : "");
    if (name && nn) snprintf(name, nn, "%s", st ? g_name : "");
    return st;
}

void xn_request_keyframe(int force) {
    if (force) g_kf_force = 1;
    else g_kf_req = 1;
}

void xn_set_load_probe(void (*fn)(uint64_t *, uint64_t *)) { g_load_probe = fn; }

static const char *st_short(int st) {
    switch (st) {
        case XCW_NEW: return "new";
        case XCW_CHECKING: return "ice";
        case XCW_ICE_CONNECTED: return "dtls";
        case XCW_DTLS_CONNECTED: return "sctp";
        case XCW_CHANNELS_OPEN: return "hsk";
        case XCW_STREAMING: return "ok";
        case XCW_FAILED: return "failed";
        case XCW_CLOSED: return "closed";
    }
    return "-";
}

int xn_stat_tail(char *dst, size_t n) {
    static xn_shared_t prev;
    static uint64_t prev_ms;
    if (!n) return 0;
    xn_shared_t c;
    xs_lock();
    c = g_sh;
    xs_unlock();
    uint64_t now = xcw_now_ms();
    double dt = prev_ms && now > prev_ms ? (now - prev_ms) / 1000.0 : 0;
    if (c.pcs != prev.pcs || c.s.video_bytes < prev.s.video_bytes) memset(&prev.s, 0, sizeof(prev.s));   /* new PC */
    double kbps = dt > 0 ? (double)(c.s.video_bytes - prev.s.video_bytes) * 8.0 / 1000.0 / dt : 0;
    uint32_t dexp = c.s.rtp_expected - prev.s.rtp_expected, dlost = c.s.rtp_lost - prev.s.rtp_lost;
    double loss = dexp ? 100.0 * dlost / dexp : 0;
    double hw1 = -1, rtc = -1;
    if (g_load_probe) {
        uint64_t busy = 0, wall = 0;
        g_load_probe(&busy, &wall);
        if (wall) {
            hw1 = 100.0 * (double)busy / (double)wall;
            rtc = 100.0 * (double)(c.rtc_us - prev.rtc_us) / (double)wall;
        }
    }
    /* v0.19.2: sess_cpu = ms of hw1 spent driving the session per 2 s
     * (keepalive/ICE record crypto + HTTP + state checks), dec_thr = decoder
     * frame threads now, sess = who drives it (net = hw1, thr = session
     * thread on hw3, wait = waiting for hw3), tls_hs = full TLS handshakes
     * on the keep-alive connection while hw1 had it (should stay put). */
    double sess = dt > 0 && c.sess_us >= prev.sess_us ? (double)(c.sess_us - prev.sess_us) / 1000.0 * (2.0 / dt) : 0;
    static const char *own_s[] = {"thr", "net", "wait", "done"};
    int m = snprintf(dst, n,
                     " rtc=%s rtt_ms=%.1f loss_pct=%.2f lost=%u kbps=%.0f nack=%u kfrq=%u pli=%u srtp_err=%u"
                     " inj_drop=%u jit_drop=%u pads=%u in=%u hw1=%.1f rtc_cpu=%.1f sess_cpu=%.2f dec_thr=%d"
                     " sess=%s tls_req=%u tls_hs=%u",
                     c.have ? st_short(c.s.state) : "-", c.s.rtt_samples ? c.s.rtt_avg_us / 1000.0 : -1.0, loss,
                     c.s.rtp_lost, kbps, c.s.nacks_sent, c.s.keyframe_requests, c.s.pli_sent, c.s.srtp_errors,
                     c.inj_drops, c.s.jit_dropped, c.s.pads_announced + 1, c.s.input_sent, hw1, rtc, sess,
                     (int)g_dec_thr, (unsigned)c.owner < 4 ? own_s[c.owner] : "-", c.tls_req, c.tls_full);
    prev = c;
    prev_ms = now;
    if (m < 0) return 0;
    return m >= (int)n ? (int)n - 1 : m;
}

void xn_summary(char *dst, size_t n) {
    xn_shared_t c;
    xs_lock();
    c = g_sh;
    xs_unlock();
    snprintf(dst, n, "rede: %u AUs, %u perdas RTP, %u NACK, rtt %.0f ms, %u descartes", c.inj_aus, c.s.rtp_lost,
             c.s.nacks_sent, c.s.rtt_samples ? c.s.rtt_avg_us / 1000.0 : 0.0, c.inj_drops);
}

/* ------------------------------------------------------------ hw1 side */

static void snapshot(xcw_t *x) {
    xcw_stats_t s;
    if (x) xcw_get_stats(x, &s);
    xs_lock();
    if (x) {
        g_sh.s = s;
        g_sh.have = 1;
    }
    g_sh.inj_aus = g_inj_aus;
    g_sh.inj_drops = g_inj_drops;
    g_sh.inj_bytes = g_inj_bytes;
    g_sh.a_pkts = g_a_pkts;
    g_sh.rtc_us = g_rtc_us;
    g_sh.steps = g_steps;
    g_sh.pcs = g_pcs;
    g_sh.sess_us = g_sess_us;
    g_sh.respawns = g_respawns;
    xs_unlock();
    if (x && s.srtp_errors > g_srtp_logged) {
        logf_("[xn] SRTP: %u pacotes rejeitados (total %u)", s.srtp_errors - g_srtp_logged, s.srtp_errors);
        g_srtp_logged = s.srtp_errors;
    }
}

/* v0.19.3: hw1, leva o g_paused pro PC atual (e pro PC novo de uma
 * re-sinalização). Entrando na pausa: controles neutros no xCloud. */
static void apply_pause(xcw_t *x) {
    if (!x) return;
    int p = g_paused;
    if (x == g_pause_pc && p == g_pause_on) return;
    int was = x == g_pause_pc ? g_pause_on : 0;
    xcw_set_video_paused(x, p);
    if (p && !was) xn_pad_neutral();
    g_pause_pc = x;
    g_pause_on = p;
}

static void detach(void) {
    xcw_t *x = xcws_pc(&g_g);
    if (x) snapshot(x);
    xcws_shutdown(&g_g);
    memset(&g_g, 0, sizeof(g_g));
    g_pc_seen = NULL;
    fence();
    g_att = 0;
    logf_("[xn] webrtc desligado (PeerConnection destruida), %u AUs entregues, %u descartados", g_inj_aus,
          g_inj_drops);
}

/* v0.19.2: drive the session's live phases (owner NET), re-spawn the
 * session thread when the machine asks for it and hw3 is free. Runs every
 * hw1 step whether or not WebRTC is attached (the final DELETE is asked
 * for after detach). */
static uint64_t g_drive_next_us, g_stats_next_ms;

static void session_drive(void) {
    xsm_t *m = xst_machine();
    if (!m) return;
    uint64_t t0 = xcw_now_us();
    /* fast path (every hw1 step, ~no cost): nothing changed hands, no
     * request in flight and the next look is not due. Owner changes are
     * re-checked under the lock below. The live machine is looked at every
     * 5 ms (events/timeouts; the old thread used 50 ms slices) and a
     * request in flight is polled every 1 ms. */
    int own_fast = __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE);
    int inflight = own_fast == XSM_OWN_NET && m->s.aq;
    if (own_fast == g_own_seen && t0 < g_drive_next_us) return;
    g_drive_next_us = t0 + (inflight ? 1000 : own_fast == XSM_OWN_NET ? 5000 : 20000);
    int own = xsm_owner(m);
    if (own != g_own_seen) {
        g_t_net_since = own == XSM_OWN_NET ? xcw_now_ms() : 0;
        g_own_seen = own;
    }
    if (own == XSM_OWN_NET) {
        xsm_net_step(m);
        if (m->s.aq) g_drive_next_us = xcw_now_us() + 1000;   /* poll the reply soon */
        uint64_t nm = xcw_now_ms();
        if (nm >= g_stats_next_ms) {
            g_stats_next_ms = nm + 250;
            https_conn_stats_t cs;
            https_conn_get_stats(xs_conn(&m->creds), &cs);
            xs_lock();
            g_sh.tls_full = cs.full;
            g_sh.tls_req = cs.requests;
            g_sh.tls_max_poll_us = cs.max_nb_poll_us;
            xs_unlock();
        }
        own = xsm_owner(m);
    }
    int busy = xst_running();
    if (own == XSM_OWN_WANT_THREAD) {
        int go = 0, hw3;
        xs_lock();
        hw3 = g_hw3;
        if (hw3 == HW3_FREE && !busy) { g_hw3 = HW3_SESSION; go = 1; }
        xs_unlock();
        if (go) {
            if (xst_resume() == 0) {
                g_respawns++;
                logf_("[xn] thread da sessao de volta na hw%d (%s), #%u", XST_HW_THREAD, m->esc_why, g_respawns);
            } else {
                xs_lock();
                g_hw3 = HW3_FREE;
                xs_unlock();
            }
        } else if (xcw_now_ms() - g_last_wait_log >= 2000) {
            if (hw3 == HW3_DECODER && !g_hw3_wait_since) g_hw3_wait_since = xcw_now_ms();
            g_last_wait_log = xcw_now_ms();
            logf_("[xn] sessao esperando a hw%d (%s) pra: %s", XST_HW_THREAD,
                  hw3 == HW3_DECODER ? "decoder" : "ocupada", m->esc_why);
        }
    } else if (!busy) {
        g_hw3_wait_since = 0;
        /* session thread gone (handed to hw1 or finished): hw3 is free */
        xs_lock();
        if (g_hw3 == HW3_SESSION) g_hw3 = HW3_FREE;
        xs_unlock();
    }
    if (own != g_own_seen) {   /* changed during this call: look again next step */
        g_drive_next_us = 0;
    }
    xs_lock();
    g_sh.owner = m->owner;
    xs_unlock();
    g_sess_us += xcw_now_us() - t0;
}

void xn_net_step(void) {
    if (!g_inited) return;
    session_drive();
    int want = g_want;
    fence();
    if (!want) {
        if (g_att) detach();
        return;
    }
    if (!g_att) {
        xsm_t *m = xst_machine();
        if (!m) return;
        xcws_init(&g_g, m, &g_xcfg);
        g_att_seq = g_play_seq;
        g_pc_gen_seen = 0;
        g_pc_seen = NULL;
        g_pads_used = 0;
        g_first_au_logged = 0;
        g_srtp_logged = 0;
        g_inj_aus = g_inj_drops = g_a_pkts = g_pcs = g_steps = 0;
        g_inj_bytes = g_rtc_us = 0;
        g_last_xcw_state = -1;
        g_rtc_dead_since = 0;
        g_hw3_wait_since = 0;
        g_pause_pc = NULL;
        g_pause_on = 0;
        g_pause_aus = g_pause_audio = 0;
        xs_lock();
        memset(&g_sh, 0, sizeof(g_sh));
        xs_unlock();
        g_t_att_ms = xcw_now_ms();
        g_next_status_ms = g_t_att_ms + XN_STATUS_PERIOD_MS;
        g_next_snap_ms = g_t_att_ms;
        fence();
        g_att = 1;
        logf_("[xn] webrtc ligado na sessao (play #%u), esperando o pedido de offer", g_att_seq);
    }

    uint64_t t0 = xcw_now_us();
    xcw_t *x = xcws_pc(&g_g);
    apply_pause(x);   /* v0.19.3: antes dos pedidos e do xcws_step */
    if (x) {
        if (g_kf_force) { g_kf_force = 0; g_kf_req = 0; xcw_request_keyframe(x, 1); }
        else if (g_kf_req) { g_kf_req = 0; xcw_request_keyframe(x, 0); }
    }
    xcws_step(&g_g);
    x = xcws_pc(&g_g);
    apply_pause(x);   /* PC novo (re-sinalização) no meio da pausa */
    g_rtc_us += xcw_now_us() - t0;
    g_steps++;

    if (x != g_pc_seen || (x && g_g.gen != g_pc_gen_seen)) {
        if (x) {
            g_pcs++;
            logf_("[xn] PeerConnection #%u (gen %u): offer feito", g_pcs, g_g.gen);
        }
        g_pc_seen = x;
        g_pc_gen_seen = g_g.gen;
        g_first_au_logged = 0;
        g_srtp_logged = 0;
    }

    uint64_t now = xcw_now_ms();
    if (now >= g_next_snap_ms) {
        g_next_snap_ms = now + 250;
        snapshot(x);
    }
    /* Before the game screen there is no STAT: say where the setup is. */
    if (now >= g_next_status_ms) {
        g_next_status_ms = now + XN_STATUS_PERIOD_MS;
        xsm_t *m = xst_machine();
        if (!g_inj_aus || (x && xcw_state(x) != XCW_STREAMING)) {
            xcw_stats_t s;
            memset(&s, 0, sizeof(s));
            if (x) xcw_get_stats(x, &s);
            logf_("[xn] t=%us sess=%s rtc=%s rx=%u rtp=%u aus=%u audio=%u srtp_err=%u dc_rx=%u/%u/%u",
                  (unsigned)((now - g_t_att_ms) / 1000), m ? xsm_phase_name(xsm_phase(m)) : "-",
                  x ? st_short(xcw_state(x)) : "-", s.rx_datagrams, s.rtp_packets, g_inj_aus, g_a_pkts,
                  s.srtp_errors, s.dc_rx[XCW_CH_CONTROL], s.dc_rx[XCW_CH_MESSAGE], s.dc_rx[XCW_CH_CHAT]);
        }
    }
}

xcw_t *xn_pc(void) { return g_att ? xcws_pc(&g_g) : NULL; }

void xn_pad_frame(const uint8_t *buf, size_t n) {
    if (!g_att || g_paused) return;   /* v0.19.3: pausado = neutro */
    xcw_t *x = xcws_pc(&g_g);
    if (!x) return;
    xcp_gamepad_t pad;
    if (xcp_gamepad_from_wire24(buf, n, &pad) != 0) return;
    if (pad.index >= XCW_MAX_PADS) pad.index = 0;
    g_pads_used |= 1u << pad.index;
    xcw_set_gamepad(x, &pad);
}

void xn_pad_neutral(void) {
    xcw_t *x = g_att ? xcws_pc(&g_g) : NULL;
    if (!x) return;
    for (int i = 0; i < XCW_MAX_PADS; i++) {
        if (!(g_pads_used & (1u << i))) continue;
        xcp_gamepad_t pad;
        memset(&pad, 0, sizeof(pad));
        pad.index = (uint8_t)i;
        xcw_set_gamepad(x, &pad);
    }
}
