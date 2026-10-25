/*
 * Host-side (x86) test of app/h264play/source/gpu_present.c scheduling logic.
 *
 * libxenon is replaced by mocks (hosttest/mock/) and the Xenos command
 * processor by an in-order fake GPU: each Xe_Execute() that carries a
 * SCRATCH0 write becomes a job that "completes" `latency` simulated us
 * later; the scratch writeback word then shows its fence value. Time only
 * advances when the code reads the timebase (1 us per read) or when the
 * driver below simulates decode time.
 *
 * This checks the CPU/GPU hand-off invariants, NOT the GPU itself:
 *   I1 the CPU never locks a texture set that an unfinished job samples
 *   I2 a resolve never targets the buffer on screen or one an unfinished job writes
 *   I3 the scanout only flips to buffers whose last job finished, in frame order
 *   I4 Xe_VBReclaim only runs when every submitted job finished
 *   I5 one texture set per draw
 * Build/run: hosttest/run.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "xenos/xe.h"
#include "ppc/timebase.h"
#define PPC_TIMEBASE_FREQ_MOCK ((uint64_t)(PPC_TIMEBASE_FREQ / 1000000ULL))
#include "../source/gpu_present.h"

/* ------------------------------------------------------------------ clock */
static uint64_t sim_us;
uint64_t mock_mftb(void);
static void gpu_update(void);
uint64_t mock_mftb(void) { sim_us++; gpu_update(); return sim_us * (PPC_TIMEBASE_FREQ_MOCK); }

/* ------------------------------------------------------------------ fake regs */
uint32_t mock_regs[0x10000 / 4];

/* ------------------------------------------------------------------ fake GPU */
#define MAXJOBS 100000
typedef struct { uint32_t fence; uint64_t done_at; int set; int scan; uint64_t frame; } job_t;
static job_t jobs[MAXJOBS];
static int njobs, ndone;
static uint64_t latency_us = 1000;
static int hang;                        /* 1: jobs never complete */
static uint8_t rb_mem[0x20000];
static uint8_t ring_mem[1 << 22];
static int have_pending_fence; static uint32_t pending_fence;
static int last_was_scratch_hdr;
static int bound_set[3] = { -1, -1, -1 };
static int draw_set = -1;               /* set sampled by the draw being built */
static int draw_scan = -1;              /* scan resolved by the job being built */
static uint64_t frame_counter;
static int violations;
static long long n_vbreclaim, n_flips_mock, n_resolves;

static int quiet;  /* hang test: violations after a fence timeout are expected */
#define FAIL(...) do { violations++; if (!quiet && violations < 20) { printf("  VIOLATION @%llu us: ", (unsigned long long)sim_us); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_scratch(uint32_t v) { memcpy(rb_mem + 0x10100, &v, 4); }

static void gpu_update(void) {
    if (hang) return;
    while (ndone < njobs && jobs[ndone].done_at <= sim_us) {
        set_scratch(jobs[ndone].fence);
        ndone++;
    }
}
static int job_pending_with_set(int s) {
    for (int i = ndone; i < njobs; i++) if (jobs[i].set == s) return 1;
    return 0;
}
static int job_pending_with_scan(int k) {
    for (int i = ndone; i < njobs; i++) if (jobs[i].scan == k) return 1;
    return 0;
}

void mock_rput32(struct XenosDevice *xe, u32 d) {
    (void)xe;
    if (last_was_scratch_hdr) { pending_fence = d; have_pending_fence = 1; last_was_scratch_hdr = 0; return; }
    if (d == (0x15e0 >> 2)) last_was_scratch_hdr = 1;
}
void Xe_pSyncFromDevice(struct XenosDevice *xe, volatile void *data, int len) { (void)xe; (void)data; (void)len; gpu_update(); }

/* ------------------------------------------------------------------ surfaces */
static struct XenosSurface surfs[32];
static int nsurf, ntiled, nlin;
static int surf_kind[32], surf_idx[32];   /* kind 1 = scan (idx), 2 = texture (set) */
static int shown_scan = -1;               /* -1 = console fb */
static uint64_t shown_frame;
static uint64_t scan_frame[3];
static struct XenosDevice *the_xe;
static int fatal_in_edram;
static int console_closed;
static int n_xe_init;

void Xe_Init(struct XenosDevice *xe) {
    n_xe_init++;
    memset(xe, 0, sizeof(*xe));
    the_xe = xe;
    xe->rb = rb_mem; xe->rb_secondary = ring_mem;
    set_scratch(0x1234567);
    xe->default_fb.width = 1280; xe->default_fb.height = 720; xe->default_fb.tiled = 1;
    xe->tex_fb = xe->default_fb;
}
struct XenosSurface *Xe_GetFramebufferSurface(struct XenosDevice *xe) { return &xe->tex_fb; }
void Xe_SetFrameBufferSurface(struct XenosDevice *xe, struct XenosSurface *fb) {
    if (fb == &xe->default_fb) { shown_scan = -1; return; }
    int k = surf_idx[fb->id];
    if (surf_kind[fb->id] != 1) { FAIL("flip to non-scan surface"); return; }
    if (job_pending_with_scan(k)) FAIL("I3 flip to scan %d whose job is unfinished", k);
    if (scan_frame[k] < shown_frame) FAIL("I3 flip backwards: frame %llu after %llu", (unsigned long long)scan_frame[k], (unsigned long long)shown_frame);
    shown_scan = k; shown_frame = scan_frame[k];
    n_flips_mock++;
}
struct XenosSurface *Xe_CreateTexture(struct XenosDevice *xe, unsigned w, unsigned h, unsigned levels, int format, int tiled) {
    (void)xe; (void)levels;
    struct XenosSurface *s = &surfs[nsurf];
    memset(s, 0, sizeof(*s));
    int bypp = (format & 0x3f) == XE_FMT_8888 ? 4 : 1;
    s->width = w; s->height = h; s->wpitch = (w * bypp + 127) & ~127; s->hpitch = (h + 31) & ~31;
    s->tiled = tiled; s->format = format; s->bypp = bypp; s->use_filtering = 1;
    s->base = malloc((size_t)s->wpitch * s->hpitch);
    s->id = nsurf;
    if (tiled) { surf_kind[nsurf] = 1; surf_idx[nsurf] = ntiled++; }
    else       { surf_kind[nsurf] = 2; surf_idx[nsurf] = nlin++ / 3; }
    nsurf++;
    return s;
}
void *Xe_Surface_LockRect(struct XenosDevice *xe, struct XenosSurface *s, int x, int y, int w, int h, int flags) {
    (void)xe; (void)x; (void)y; (void)w; (void)h; (void)flags;
    if (surf_kind[s->id] == 2 && job_pending_with_set(surf_idx[s->id]))
        FAIL("I1 CPU locks texture set %d while GPU samples it", surf_idx[s->id]);
    return s->base;
}
void Xe_Surface_Unlock(struct XenosDevice *xe, struct XenosSurface *s) { (void)xe; (void)s; }
void Xe_SetRenderTarget(struct XenosDevice *xe, struct XenosSurface *rt) { (void)xe; (void)rt; }
static struct XenosShader sh_ps, sh_vs; static int nsh;
struct XenosShader *Xe_LoadShaderFromMemory(struct XenosDevice *xe, void *sh) {
    (void)xe; const uint32_t *w = sh;
    if ((w[0] & 0xffffff00) != 0x102a1100) FAIL("bad shader magic %08x", w[0]);
    return nsh++ ? &sh_vs : &sh_ps;
}
void Xe_InstantiateShader(struct XenosDevice *xe, struct XenosShader *sh, unsigned idx) { (void)xe; (void)sh; (void)idx; }
void Xe_ShaderApplyVFetchPatches(struct XenosDevice *xe, struct XenosShader *sh, unsigned idx, const struct XenosVBFFormat *f) {
    (void)xe; (void)sh; (void)idx;
    if (f->num != 2) FAIL("vbf num %d", f->num);
}
static struct XenosVertexBuffer the_vb; static float vb_data[18];
struct XenosVertexBuffer *Xe_CreateVertexBuffer(struct XenosDevice *xe, int len) { (void)xe; if (len != 72) FAIL("vb len %d", len); return &the_vb; }
void *Xe_VB_Lock(struct XenosDevice *xe, struct XenosVertexBuffer *vb, int off, int size, int flags) {
    (void)xe; (void)vb; (void)off; (void)size; (void)flags;
    for (int i = ndone; i < njobs; i++) if (jobs[i].set >= 0) { FAIL("VB rewritten while a draw is in flight"); break; }
    return vb_data;
}
void Xe_VB_Unlock(struct XenosDevice *xe, struct XenosVertexBuffer *vb) { (void)xe; (void)vb; }
void Xe_SetClearColor(struct XenosDevice *xe, u32 c) { (void)xe; (void)c; }
void Xe_InvalidateState(struct XenosDevice *xe) { (void)xe; bound_set[0] = bound_set[1] = bound_set[2] = -1; }
void Xe_SetZEnable(struct XenosDevice *xe, int z) { (void)xe; (void)z; }
void Xe_SetCullMode(struct XenosDevice *xe, unsigned m) { (void)xe; (void)m; }
void Xe_SetStreamSource(struct XenosDevice *xe, int i, struct XenosVertexBuffer *vb, int off, int stride) { (void)xe; (void)i; (void)vb; (void)off; if (stride != 24) FAIL("stride %d", stride); }
void Xe_SetShader(struct XenosDevice *xe, int type, struct XenosShader *sh, int inst) {
    (void)xe; (void)inst;
    if (type == SHADER_TYPE_PIXEL && sh != &sh_ps) FAIL("wrong pixel shader");
    if (type == SHADER_TYPE_VERTEX && sh != &sh_vs) FAIL("wrong vertex shader");
}
void Xe_SetTexture(struct XenosDevice *xe, int i, struct XenosSurface *t) {
    (void)xe;
    if (surf_kind[t->id] != 2) FAIL("sampler %d bound to non-texture", i);
    bound_set[i] = surf_idx[t->id];
    /* sampler i must be plane i: Y,U,V were created in that order per set */
    if ((t->id - ntiled) % 3 != i) FAIL("sampler %d bound to plane %d", i, (t->id - ntiled) % 3);
}
void Xe_DrawPrimitive(struct XenosDevice *xe, int type, int start, int n) {
    (void)xe; (void)start;
    if (type != XE_PRIMTYPE_RECTLIST || n != 1) FAIL("draw type %d n %d", type, n);
    if (bound_set[0] < 0 || bound_set[0] != bound_set[1] || bound_set[1] != bound_set[2]) FAIL("I5 mixed texture sets %d/%d/%d", bound_set[0], bound_set[1], bound_set[2]);
    draw_set = bound_set[0];
}
void Xe_ResolveInto(struct XenosDevice *xe, struct XenosSurface *s, int src, int clear) {
    (void)xe; (void)src; (void)clear;
    int k = surf_idx[s->id];
    if (surf_kind[s->id] != 1) FAIL("resolve into non-scan");
    if (k == shown_scan) FAIL("I2 resolve into scan %d which is ON SCREEN", k);
    if (job_pending_with_scan(k)) FAIL("I2 resolve into scan %d still being written", k);
    draw_scan = k;
    scan_frame[k] = ++frame_counter;
    n_resolves++;
}
void Xe_Execute(struct XenosDevice *xe) {
    (void)xe;
    if (!have_pending_fence) return;
    if (njobs >= MAXJOBS) { printf("too many jobs\n"); exit(2); }
    uint64_t start = njobs && jobs[njobs - 1].done_at > sim_us ? jobs[njobs - 1].done_at : sim_us;
    jobs[njobs] = (job_t){ pending_fence, hang ? UINT64_MAX : start + latency_us, draw_set, draw_scan, frame_counter };
    njobs++;
    have_pending_fence = 0; draw_set = -1; draw_scan = -1;
}
void Xe_VBReclaim(struct XenosDevice *xe) {
    (void)xe; gpu_update();
    if (ndone != njobs) FAIL("I4 VBReclaim with %d unfinished jobs", njobs - ndone);
    n_vbreclaim++;
}
int Xe_IsVBlank(struct XenosDevice *xe) { (void)xe; return 1; }
void console_open(void) { console_closed = 0; }
void console_close(void) { console_closed = 1; }
void Xe_Fatal(struct XenosDevice *xe, const char *fmt, ...);
void Xe_Fatal(struct XenosDevice *xe, const char *fmt, ...) {  /* = __real_Xe_Fatal under --wrap */ (void)xe; printf("real Xe_Fatal: %s\n", fmt); exit(3); }
/* Like edram.o: calls Xe_Fatal from another object -> goes through --wrap. */
void edram_fail_hook(struct XenosDevice *xe);
void edram_init(struct XenosDevice *xe) { if (fatal_in_edram) edram_fail_hook(xe); }

/* ------------------------------------------------------------------ driver */
static uint8_t Y[1280 * 720], U[640 * 360], V[640 * 360];

static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

static void reset_world(void) {
    njobs = ndone = 0; nsurf = ntiled = nlin = 0; nsh = 0; shown_scan = -1; shown_frame = 0;
    frame_counter = 0; hang = 0; violations = 0; n_vbreclaim = n_flips_mock = n_resolves = 0;
    have_pending_fence = 0; last_was_scratch_hdr = 0; console_closed = 0;
    memset(scan_frame, 0, sizeof(scan_frame));
    mock_regs[0x6134 / 4] = 1280; mock_regs[0x6138 / 4] = 720;
}

typedef struct { const char *name; int frames; uint64_t lat_min, lat_max, iv_min, iv_max; int poll_min, poll_max; int res_change; } scen_t;

static int run(const scen_t *sc, int fresh_init) {
    if (fresh_init) {
        /* gpu_present.c keeps static state; each scenario runs in its own process (see main). */
    }
    printf("scenario %-34s ", sc->name);
    fflush(stdout);
    reset_world();
    latency_us = 1000;
    if (gpu_present_init(1280, 720) != 0) { printf("init failed\n"); return 1; }
    uint8_t *d[3] = { Y, U, V };
    int ls[3] = { 1344, 704, 704 };     /* padded like libavcodec */
    for (int f = 0; f < sc->frames; f++) {
        latency_us = sc->lat_min + rnd((uint32_t)(sc->lat_max - sc->lat_min + 1));
        int w = 1280, h = 720;
        if (sc->res_change && (f / 97) % 2) { w = 960; h = 540; }
        gpu_present_frame(d, ls, w, h, (uint64_t)f);
        uint64_t iv = sc->iv_min + rnd((uint32_t)(sc->iv_max - sc->iv_min + 1));
        int polls = sc->poll_min + (int)rnd((uint32_t)(sc->poll_max - sc->poll_min + 1));
        for (int p = 0; p < polls; p++) { sim_us += iv / (polls + 1); gpu_present_poll(); }
        sim_us += iv / (polls + 1);
    }
    gpu_present_finish();
    gpu_present_stats_t st; gpu_present_get_stats(&st);
    if (shown_scan != -1) FAIL("finish did not restore console fb");
    if (console_closed) FAIL("finish did not reopen console");
    printf("frames=%llu flips=%llu sup=%u wait_avg=%.3fms flip_lag_avg=%.3fms timeouts=%u reclaims=%lld -> %s\n",
           (unsigned long long)st.frames, (unsigned long long)st.flips, st.superseded,
           st.frames ? st.wait_us / 1000.0 / st.frames : 0.0,
           st.flips ? st.flip_lag_us / 1000.0 / st.flips : 0.0, st.timeouts, n_vbreclaim,
           violations ? "FAIL" : "ok");
    if (st.flips + st.superseded != st.frames) { printf("  frames != flips+superseded\n"); violations++; }
    return violations ? 1 : 0;
}

int main(int argc, char **argv) {
    static const scen_t sc[] = {
        { "60fps, GPU 1ms, poll every 1ms",     3000, 800, 1200, 16667, 16667, 15, 15, 0 },
        { "30fps (DIVISOR=2), GPU 1ms",         3000, 800, 1200, 33333, 33333, 30, 30, 0 },
        { "60fps, no polling between frames",   3000, 800, 1200, 16667, 16667, 0, 0, 0 },
        { "back-to-back frames (burst)",        3000, 500, 3000, 0, 300, 0, 1, 0 },
        { "GPU slower than frame rate (30ms)",  1000, 25000, 35000, 16667, 16667, 2, 10, 0 },
        { "random everything + res change",     20000, 50, 40000, 0, 40000, 0, 20, 1 },
    };
    int which = argc > 1 ? atoi(argv[1]) : -1;
    if (which >= 0 && which < (int)(sizeof(sc) / sizeof(sc[0]))) return run(&sc[which], 1);
    if (which == 100) {   /* GPU hang: every wait must time out, not spin forever */
        printf("scenario %-34s ", "GPU hangs after init");
        reset_world();
        if (gpu_present_init(1280, 720) != 0) { printf("init failed\n"); return 1; }
        hang = 1; quiet = 1;   /* after a timeout gpu_present carries on: overlap is expected */
        uint8_t *d[3] = { Y, U, V }; int ls[3] = { 1280, 640, 640 };
        for (int f = 0; f < 6; f++) { gpu_present_frame(d, ls, 1280, 720, f); sim_us += 16667; gpu_present_poll(); }
        gpu_present_stats_t st; gpu_present_get_stats(&st);
        printf("frames=%llu flips=%llu timeouts=%u -> %s\n", (unsigned long long)st.frames,
               (unsigned long long)st.flips, st.timeouts, (st.timeouts > 0 && st.flips == 0) ? "ok" : "FAIL");
        return (st.timeouts > 0 && st.flips == 0) ? 0 : 1;
    }
    if (which == 101) {   /* edram_init Xe_Fatal -> init returns -1 (CPU fallback) */
        printf("scenario %-34s ", "edram_init Xe_Fatal -> fallback");
        reset_world(); fatal_in_edram = 1;
        int r = gpu_present_init(1280, 720);
        printf("init r=%d -> %s\n", r, r < 0 ? "ok" : "FAIL");
        return r < 0 ? 0 : 1;
    }
    if (which == 102) {   /* GPU never executes even the init fence -> fallback */
        printf("scenario %-34s ", "dead CP at init -> fallback");
        reset_world(); hang = 1;
        int r = gpu_present_init(1280, 720);
        printf("init r=%d -> %s\n", r, r < 0 ? "ok" : "FAIL");
        return r < 0 ? 0 : 1;
    }
    if (which == 103) {   /* 1080p scanout -> refuse (EDRAM budget) */
        printf("scenario %-34s ", "1080p scanout -> fallback");
        reset_world(); mock_regs[0x6134 / 4] = 1920; mock_regs[0x6138 / 4] = 1080;
        int r = gpu_present_init(1280, 720);
        printf("init r=%d -> %s\n", r, r < 0 ? "ok" : "FAIL");
        return r < 0 ? 0 : 1;
    }
    if (which == 104) {   /* v0.16 menu: finish -> init de novo, 3 sessões */
        printf("scenario %-34s ", "re-arm after finish (menu, 3 games)");
        reset_world(); n_xe_init = 0;
        if (gpu_present_init(1280, 720) != 0) { printf("init failed\n"); return 1; }
        int surf0 = nsurf;
        if (gpu_present_init(1280, 720) != 0) FAIL("init while active must be a no-op");
        uint8_t *d[3] = { Y, U, V }; int ls[3] = { 1344, 704, 704 };
        uint64_t frames = 0;
        for (int sess = 0; sess < 3; sess++) {
            if (sess && gpu_present_init(1280, 720) != 0) { FAIL("re-arm returned error"); break; }
            if (console_closed) FAIL("console closed before the first flip of session %d", sess);
            for (int f = 0; f < 700; f++) {
                latency_us = 200 + rnd(20000);
                gpu_present_frame(d, ls, 1280, 720, frames++);
                int polls = (int)rnd(8);
                for (int k = 0; k < polls; k++) { sim_us += 2000; gpu_present_poll(); }
                sim_us += 1000 + rnd(15000);
            }
            if (!console_closed) FAIL("session %d never flipped", sess);
            gpu_present_finish();
            if (shown_scan != -1) FAIL("finish did not restore console fb (session %d)", sess);
            if (console_closed) FAIL("finish did not reopen console (session %d)", sess);
            if (ndone != njobs) FAIL("finish returned with %d GPU jobs pending", njobs - ndone);
            sim_us += 5000000;   /* tempo no menu */
        }
        if (gpu_present_init(1280, 999) == 0) FAIL("re-arm accepted a different texture size");
        gpu_present_stats_t st; gpu_present_get_stats(&st);
        if (st.flips + st.superseded != st.frames) FAIL("frames %llu != flips+superseded", (unsigned long long)st.frames);
        if (n_xe_init != 1) FAIL("Xe_Init called %d times", n_xe_init);
        if (nsurf != surf0) FAIL("surfaces allocated again (%d -> %d)", surf0, nsurf);
        printf("frames=%llu flips=%llu sup=%u xe_init=%d surfaces=%d -> %s\n",
               (unsigned long long)st.frames, (unsigned long long)st.flips, st.superseded,
               n_xe_init, nsurf, violations ? "FAIL" : "ok");
        return violations ? 1 : 0;
    }
    printf("usage: sim <0..5|100..104>\n");
    return 2;
}
