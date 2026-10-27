/*
 * orbita v0.21.25 — DIAG.TXT decoder/CPU switches, [dec] prof and [cpu]
 * lines. See xc_decdiag.h and DECODE-PERF.md.
 */
#include "xc_decdiag.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XCLOUD360_SKIP_DEBLOCK
#define XCLOUD360_SKIP_DEBLOCK 0
#endif

xdd_cfg_t xdd;

void xdd_defaults(xdd_cfg_t *c) {
    memset(c, 0, sizeof(*c));
    c->deblock = XCLOUD360_SKIP_DEBLOCK ? XDD_DEBLOCK_ALL : XDD_DEBLOCK_NONE;
    c->prof = 1;
    c->copy_vmx = 1;
    c->spin_low = 1;
    c->net_low = 1;
    c->lite = 1;          /* v0.21.27: beta sai com log leve no jogo (DIAG.TXT diag=full volta) */
}

static int val_is(const char *v, const char *w) {
    size_t n = strlen(w);
    return strncmp(v, w, n) == 0 && (v[n] == 0 || v[n] == '\r' || v[n] == '\n' || v[n] == ' ' || v[n] == '\t' ||
                                      v[n] == '#' || v[n] == ';');
}

/* one line "key=value"; returns 1 if the key is ours */
static int parse_kv(xdd_cfg_t *c, const char *k, size_t kl, const char *v) {
#define KEY(s) (kl == sizeof(s) - 1 && strncmp(k, s, kl) == 0)
    if (KEY("deblock")) {
        if (val_is(v, "none") || val_is(v, "on") || val_is(v, "full")) c->deblock = XDD_DEBLOCK_NONE;
        else if (val_is(v, "nonref")) c->deblock = XDD_DEBLOCK_NONREF;
        else if (val_is(v, "all") || val_is(v, "off")) c->deblock = XDD_DEBLOCK_ALL;
        else return 0;
        return 1;
    }
    if (KEY("decthreads")) {
        int n = atoi(v);
        if (n < 1 || n > 4) return 0;
        c->decthreads = n;
        return 1;
    }
    if (KEY("decprof")) { c->prof = v[0] != '0'; return 1; }
    if (KEY("profile")) {
        if (val_is(v, "baseline") || val_is(v, "base")) c->profile = XDD_PROFILE_BASELINE;
        else if (val_is(v, "main")) c->profile = XDD_PROFILE_MAIN;
        else if (val_is(v, "high")) c->profile = XDD_PROFILE_HIGH;
        else if (val_is(v, "default")) c->profile = XDD_PROFILE_DEFAULT;
        else return 0;
        return 1;
    }
    if (KEY("copy")) {
        if (val_is(v, "vmx")) c->copy_vmx = 1;
        else if (val_is(v, "memcpy")) c->copy_vmx = 0;
        else return 0;
        return 1;
    }
    if (KEY("spinprio")) { c->spin_low = !val_is(v, "med"); return 1; }
    if (KEY("netprio")) { c->net_low = !val_is(v, "med"); return 1; }
    if (KEY("diag")) {
        if (val_is(v, "lite")) c->lite = 1;
        else if (val_is(v, "full")) c->lite = 0;
        else return 0;
        return 1;
    }
    if (KEY("netlog")) { c->netlog_set = 1; return 0; }   /* main.c reads it; lite must not override */
#undef KEY
    return 0;
}

void xdd_parse(xdd_cfg_t *c, const char *txt) {
    const char *p = txt;
    while (p && *p) {
        const char *eol = p + strcspn(p, "\r\n");
        while (p < eol && isspace((unsigned char)*p)) p++;
        const char *eq = memchr(p, '=', (size_t)(eol - p));
        if (eq && *p != '#') {
            const char *ke = eq;
            while (ke > p && isspace((unsigned char)ke[-1])) ke--;
            const char *v = eq + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            char vb[24];
            size_t vl = (size_t)(eol - v);
            if (vl >= sizeof(vb)) vl = sizeof(vb) - 1;
            for (size_t i = 0; i < vl; i++) vb[i] = (char)tolower((unsigned char)v[i]);
            vb[vl] = 0;
            char kb[24];
            size_t kl = (size_t)(ke - p);
            if (kl < sizeof(kb)) {
                for (size_t i = 0; i < kl; i++) kb[i] = (char)tolower((unsigned char)p[i]);
                kb[kl] = 0;
                if (parse_kv(c, kb, kl, vb)) c->keys++;
            }
        }
        p = *eol ? eol + 1 : eol;
    }
}

const char *xdd_profile_name(int p) {
    switch (p) {
    case XDD_PROFILE_BASELINE: return "baseline";
    case XDD_PROFILE_MAIN: return "main";
    case XDD_PROFILE_HIGH: return "high";
    default: return "default";
    }
}

/* level 3.1 like the default template (42e01f); Chrome uses the same ids */
const char *xdd_profile_plid(int p) {
    switch (p) {
    case XDD_PROFILE_BASELINE: return "42e01f";
    case XDD_PROFILE_MAIN: return "4d001f";
    case XDD_PROFILE_HIGH: return "64001f";
    default: return NULL;
    }
}

const char *xdd_h264_profile_name(int idc) {
    switch (idc) {
    case 66: return "Baseline";
    case 77: return "Main";
    case 88: return "Extended";
    case 100: return "High";
    case 110: return "High 10";
    case 122: return "High 4:2:2";
    case 244: return "High 4:4:4";
    default: return "?";
    }
}

void xdd_describe(const xdd_cfg_t *c, char *o, size_t n) {
    static const char *db[] = { "none", "nonref", "all" };
    snprintf(o, n, "deblock(pula)=%s decthreads=%s%d decprof=%d profile=%s copy=%s spinprio=%s netprio=%s diag=%s",
             db[c->deblock % 3], c->decthreads ? "" : "padrão ", c->decthreads ? c->decthreads : 4, c->prof,
             xdd_profile_name(c->profile), c->copy_vmx ? "vmx" : "memcpy", c->spin_low ? "low" : "med",
             c->net_low ? "low" : "med", c->lite ? "lite" : "full");
}

/* ======================================================================== */
#ifdef XENON
#include <libavcodec/avcodec.h>
#include <libavcodec/xc_prof.h>
#include <ppc/timebase.h>
#include "../login/xc_port.h"
#include "../native/xc_native.h"
#include "../login/net_pipe.h"
#include <stdarg.h>

/* in game, from main: RAM log only (xc_log on hw0 also prints to the UART,
 * ~87 us per character: a 200-byte line would stall the present ~17 ms) */
static void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dlog(const char *fmt, ...) {
    char b[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    np_log_line(b);
}

/* other modules' switches (each defined where it is used) */
extern volatile uint64_t xpt_idle_tb[8];   /* deps/xenon-pthread: idle spin per hw thread */
extern volatile int xpt_spin_low;
extern volatile int stream_net_lowprio;     /* tcp_stream.c */
uint64_t stream_net_busy_tb(void);
extern int xw_diag_netlog;                  /* wifi: NETLOG raw writes */
extern int xw_diag_lite;                    /* wifi: stats every 30 s in game */
extern int xcw_h264_profile_plid_set(const char *plid);   /* webrtc offer */
extern volatile int gpu_present_copy_vmx;   /* gpu_present.c */
extern volatile uint64_t xc_log_tb[8];      /* xc_port_xenon.c: time inside xc_log */

volatile uint64_t xdd_tb_main_idle, xdd_tb_audio, xdd_tb_pad, xdd_tb_usbwifi;
volatile uint32_t xdd_n_audio, xdd_n_pad;

void xdd_boot_apply(void) {
    xc_prof_on = xdd.prof;
    xpt_spin_low = xdd.spin_low;
    stream_net_lowprio = xdd.net_low;
    gpu_present_copy_vmx = xdd.copy_vmx;
    xcw_h264_profile_plid_set(xdd_profile_plid(xdd.profile));
    xw_diag_lite = xdd.lite;
    if (xdd.lite && !xdd.netlog_set) xw_diag_netlog = 0;
    char d[256];
    xdd_describe(&xdd, d, sizeof(d));
    xc_log("[dec] DIAG.TXT: %s (%d chaves)", d, xdd.keys);
}

void xdd_apply_ctx(struct AVCodecContext *ctx) {
    ctx->flags2 |= CODEC_FLAG2_FAST;
    ctx->skip_loop_filter = xdd.deblock == XDD_DEBLOCK_ALL ? AVDISCARD_ALL
                          : xdd.deblock == XDD_DEBLOCK_NONREF ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

int xdd_max_threads(int def) { return xdd.decthreads ? xdd.decthreads : def; }



int xdd_drop_lib_line(const char *l, int in_game) {
    if (!xdd.lite || !in_game || !l) return 0;
    if (strstr(l, "ERR") || strstr(l, "err") || strstr(l, "fail") || strstr(l, "WARN") || strstr(l, "warn")) return 0;
    return 1;
}

#define TB_PER_MS ((double)PPC_TIMEBASE_FREQ / 1000.0)

static xc_prof_t g_pp;           /* previous snapshot */
static int g_seen_profile = -1;

static void prof_line(int dec_threads, uint64_t now_tb) {
    xc_prof_t c = xc_prof;       /* racy copy: counters only grow */
    uint64_t sum[XCP_NKIND] = { 0 };
    uint32_t frames = 0;
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < XCP_NKIND; k++) sum[k] += c.t[i].tb[k] - g_pp.t[i].tb[k];
        frames += c.t[i].frames - g_pp.t[i].frames;
    }
    uint32_t sl = c.slices - g_pp.slices, nr = c.slices_nonref - g_pp.slices_nonref;
    uint32_t ca = c.slices_cabac - g_pp.slices_cabac, idr = c.slices_idr - g_pp.slices_idr;
    double f = frames ? frames : 1;
    char h3[64] = "";
    xn_hw3_diag(h3, sizeof(h3));
    dlog("[dec] prof %u quadros, %d threads (%s): por quadro decode %.1f ms = entropia %.1f + recon %.1f (MC %.1f) + "
           "deblock %.1f + espera %.1f + resto %.1f; fatias %u (nao-ref %u, CABAC %u, IDR %u)",
           (unsigned)frames, dec_threads, h3, sum[XCP_DECODE] / TB_PER_MS / f, sum[XCP_ENTROPY] / TB_PER_MS / f,
           sum[XCP_RECON] / TB_PER_MS / f, sum[XCP_MC] / TB_PER_MS / f, sum[XCP_DEBLOCK] / TB_PER_MS / f,
           sum[XCP_AWAIT] / TB_PER_MS / f,
           ((double)sum[XCP_DECODE] - sum[XCP_ENTROPY] - sum[XCP_RECON] - sum[XCP_DEBLOCK] - sum[XCP_AWAIT]) / TB_PER_MS / f,
           (unsigned)sl, (unsigned)nr, (unsigned)ca, (unsigned)idr);
    if (c.profile_idc != g_seen_profile && c.profile_idc > 0) {
        g_seen_profile = c.profile_idc;
        dlog("[dec] stream H.264: perfil %d (%s), nivel %d.%d, %dx%d MBs, %s (oferta: %s)", c.profile_idc,
               xdd_h264_profile_name(c.profile_idc), c.level_idc / 10, c.level_idc % 10, c.mb_w, c.mb_h,
               ca ? "CABAC" : "CAVLC", xdd_profile_name(xdd.profile));
    }
    g_pp = c;
    (void)now_tb;
}

/* [cpu]: busy % per hw thread over the last window */
static uint64_t g_cp_tb, g_cp_idle[8], g_cp_net, g_cp_main_idle, g_cp_audio, g_cp_pad, g_cp_usb, g_cp_log[8];
static xc_prof_t g_cp;
static unsigned g_cp_gwait, g_cp_na, g_cp_np;

static void cp_reset(uint64_t now_tb, unsigned gwait_us);

static void cpu_line(uint64_t now_tb, unsigned gwait_us) {
    double w = (double)(now_tb - g_cp_tb);
    if (w <= 0) { cp_reset(now_tb, gwait_us); return; }
    xc_prof_t c = xc_prof;
    uint64_t idle[8], logt[8];
    for (int i = 0; i < 8; i++) { idle[i] = xpt_idle_tb[i] - g_cp_idle[i]; logt[i] = xc_log_tb[i] - g_cp_log[i]; }
    uint64_t net = stream_net_busy_tb() - g_cp_net;
    uint64_t mi = xdd_tb_main_idle - g_cp_main_idle;
    double gw = (double)(gwait_us - g_cp_gwait) * (PPC_TIMEBASE_FREQ / 1000000.0);
    double sec = w / PPC_TIMEBASE_FREQ;
    double hw0 = 100.0 * (1.0 - ((double)idle[0] + (double)mi + gw) / w);
    if (hw0 < 0) hw0 = 0;
    char o[200];
    size_t k = (size_t)snprintf(o, sizeof(o), "[cpu] %.0fs: hw0 main %.0f%% hw1 rede %.0f%% (audio %.1f ms/s em %u, "
                                "controle %.2f ms/s em %u, usb+wifi %.1f ms/s, log %.1f+%.1f ms/s) | decoder",
                                sec, hw0, 100.0 * net / w, (xdd_tb_audio - g_cp_audio) / TB_PER_MS / sec,
                                (unsigned)(xdd_n_audio - g_cp_na), (xdd_tb_pad - g_cp_pad) / TB_PER_MS / sec,
                                (unsigned)(xdd_n_pad - g_cp_np), (xdd_tb_usbwifi - g_cp_usb) / TB_PER_MS / sec,
                                logt[0] / TB_PER_MS / sec, logt[1] / TB_PER_MS / sec);
    static const int hw[] = { 2, 3, 4, 5 };
    for (int j = 0; j < 4 && k < sizeof(o); j++) {
        int i = hw[j];
        double dec = (double)(c.t[i].tb[XCP_DECODE] - g_cp.t[i].tb[XCP_DECODE]);
        double aw = (double)(c.t[i].tb[XCP_AWAIT] - g_cp.t[i].tb[XCP_AWAIT]);
        k += (size_t)snprintf(o + k, sizeof(o) - k, " hw%d %.0f%%/%.0f%%", i, 100.0 * (dec - aw) / w,
                              100.0 * (aw + (double)idle[i]) / w);
    }
    dlog("%s", o);
    cp_reset(now_tb, gwait_us);
}

static void cp_reset(uint64_t now_tb, unsigned gwait_us) {
    g_cp = xc_prof;
    for (int i = 0; i < 8; i++) { g_cp_idle[i] = xpt_idle_tb[i]; g_cp_log[i] = xc_log_tb[i]; }
    g_cp_net = stream_net_busy_tb();
    g_cp_main_idle = xdd_tb_main_idle;
    g_cp_audio = xdd_tb_audio;
    g_cp_pad = xdd_tb_pad;
    g_cp_usb = xdd_tb_usbwifi;
    g_cp_na = xdd_n_audio;
    g_cp_np = xdd_n_pad;
    g_cp_gwait = gwait_us;
    g_cp_tb = now_tb;
}

void xdd_tick(uint64_t now_us, int dec_threads, unsigned gwait_us) {
    static uint64_t next_cpu, last_us;
    uint64_t now_tb = mftb();
    int fresh = !g_cp_tb || now_us - last_us > 3000000;   /* 1st tick of this game */
    last_us = now_us;
    if (fresh) {   /* just take the snapshots */
        g_pp = xc_prof;
        g_cp_tb = now_tb;
        cp_reset(now_tb, gwait_us);
        next_cpu = now_us + 5000000;
        return;
    }
    if (xdd.prof) prof_line(dec_threads, now_tb);
    if (now_us >= next_cpu) {
        next_cpu = now_us + 5000000;
        cpu_line(now_tb, gwait_us);
    }
}
/* v0.21.26: threads de frame presas às hw 4,2,5 (+3 com o aluguel) */
extern void xpt_set_slots(const int *ids, int n);
extern void xpt_track_reset(void);
extern volatile int xpt_last_ids[8], xpt_n_last, xpt_parked;
extern int xenon_is_thread_task_running(int thread);
int xdd_open_pinned(struct AVCodecContext *ctx, struct AVCodec *codec, int hw3) {
    static const int s4[] = { 4, 2, 5, 3 }, s3[] = { 4, 2, 5 };
    char busy[32] = "";
    size_t bk = 0;
    for (int id = 1; id <= 5; id++)
        if (xenon_is_thread_task_running(id) && bk + 5 < sizeof(busy)) bk += (size_t)snprintf(busy + bk, sizeof(busy) - bk, " hw%d", id);
    int want = ctx->thread_count, r = -1;
    for (int t = want; t >= 1; t--) {
        ctx->thread_count = t;
        xpt_track_reset();
        xpt_set_slots(hw3 ? s4 : s3, hw3 ? 4 : 3);
        r = avcodec_open2(ctx, codec, NULL);
        xpt_set_slots(NULL, 0);
        if (r >= 0) break;
    }
    char ids[32] = "";
    size_t k = 0;
    for (int i = 0; i < xpt_n_last && i < 8 && k + 4 < sizeof(ids); i++)
        k += (size_t)snprintf(ids + k, sizeof(ids) - k, "%s%d", i ? "," : "", xpt_last_ids[i]);
    dlog("[dec] abriu %d threads de frame (pedido %d) nas hw %s; permitidas %s; ocupadas antes:%s; estacionadas %d",
         r >= 0 ? ctx->thread_count : 0, want, ids[0] ? ids : "-", hw3 ? "4,2,5,3" : "4,2,5", busy[0] ? busy : " nenhuma",
         xpt_parked);
    return r;
}
#endif
