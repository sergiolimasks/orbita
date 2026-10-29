/*
 * XCloud360 — h264play GPU display backend (Track B, `make GPU=1`).
 *
 * Frame N (main thread only):
 *   1. poll: flip to the newest finished frame, recycle libxenon VBs
 *   2. wait (normally 0 us) until texture set N%2 is no longer sampled by
 *      frame N-2 and scanout buffer N%3 is not on screen
 *   3. memcpy Y/U/V rows into set N%2 (linear XE_FMT_8 textures, row stride
 *      = surface->wpitch), Xe_Surface_Unlock = dcbst + GPU cache invalidate
 *   4. fullscreen RECTLIST with xmplayer's BT.601 pixel shader, resolve EDRAM
 *      into scanout buffer N%3 (tiled BGRA8888)
 *   5. fence: WAIT_UNTIL(idle) + SCRATCH(0x15e0) = frameidx, Xe_Execute (kick,
 *      no wait). The CP writes the scratch value back to xe->rb+0x10100 when
 *      it gets there; gpu_present_poll() reads it without blocking and flips.
 *
 * 2 texture sets: CPU fills N+1 while the GPU samples N.
 * 3 scanout buffers: the resolve of N never targets the buffer on screen
 * (N-1 or N-2) even when the flip of N-1 has not happened yet.
 *
 * Nothing here has run on hardware. See GPU-BACKEND.md.
 */
#ifdef XCLOUD360_GPU

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#include <xetypes.h>
#include <console/console.h>
#include <xenos/xe.h>
#include <xenos/xe_internal.h>   /* rput32(), Xe_pSyncFromDevice() */
#include <xenos/edram.h>
#include <altivec.h>   /* v0.21.25: gp_copy_row_vmx (lvx/stvx) */
#include <ppc/timebase.h>

#include "gpu_present.h"
#include "xmp_shaders.h"         /* xmp_ps[], xmp_vs[] — from xmplayer, GPL */

#ifndef XCLOUD360_VSYNC
#define XCLOUD360_VSYNC 0
#endif

/* Mirrors libxenon drivers/xenos/xe.c (not exported by the headers). */
#define GP_SCRATCH_WRITEBACK 0x10100   /* xe->rb offset the CP writes SCRATCH0 to */
#define GP_REG_SCRATCH0      0x15e0    /* written by Xe_pDebugSync()/Xe_Sync() too */
#define GP_REG_WAIT_UNTIL    0x1720    /* Xe_pBlockUntilIdle(): 0x20000 */
#define GP_CP_PACKET0(reg)   ((reg) >> 2)

/* Seams for the host-side test (hosttest/): real hardware values by default. */
#ifndef GP_REGS_BASE
#define GP_REGS_BASE 0xec800000UL        /* Xenos MMIO, as xe->regs in Xe_Init */
#endif
#ifndef GP_SPIN_PAUSE
#define GP_SPIN_PAUSE() __asm__ __volatile__("db16cyc" ::: "memory")
#endif

#define GP_SETS  2
#define GP_SCANS 3
#define GP_FENCE_TIMEOUT_US 1000000ULL

typedef struct { float x, y, z, w, u, v; } gp_vtx_t;   /* 24 B, matches vbf */

static struct XenosDevice  g_dev;
static struct XenosDevice *g_xe;
static struct XenosShader *g_ps, *g_vs;
static struct XenosVertexBuffer *g_vb;
static struct XenosSurface *g_tex[GP_SETS][3];   /* [set][Y,U,V] */
static struct XenosSurface *g_scan[GP_SCANS];    /* tiled BGRA scanout */

static int      g_active;
static int      g_inited;                   /* v0.16: Xe_Init + buffers já feitos */
static int      g_set;                      /* next texture set to fill */
static int      g_scan_next;                /* next scanout buffer to resolve into */
static int      g_shown = -1;               /* scanout index on screen; -1 = console fb */
static int      g_set_busy[GP_SETS];
static uint32_t g_set_fence[GP_SETS];
static int      g_scan_pending[GP_SCANS];   /* submitted, not yet flipped/superseded */
static uint32_t g_scan_fence[GP_SCANS];
static uint64_t g_scan_submit_us[GP_SCANS];
static uint64_t g_scan_tag[GP_SCANS];
static int      g_have_last;
static uint32_t g_last_fence;               /* newest submitted fence */
static int      g_vb_dirty_frames;          /* frames since last Xe_VBReclaim */
static int      g_uv_w, g_uv_h;             /* frame size the quad UVs are set for */
static int      g_tex_w, g_tex_h;
static int      g_console_closed;
static void   (*g_flip_cb)(uint64_t, uint64_t);
static gpu_present_stats_t g_st;

static inline uint64_t gp_now_us(void) {
    return mftb() / (PPC_TIMEBASE_FREQ / 1000000ULL);
}

/* ---- Xe_Fatal interception during init (link with -Wl,--wrap=Xe_Fatal) ----
 * Only calls that cross object files are wrapped: edram_init()'s "EDRAM init
 * failed, again and again" (edram.o) is caught and turned into a CPU
 * fallback. Fatal errors raised inside xe.o itself (e.g. out of memory in
 * Xe_pAlloc) still abort, as before. Outside init everything is forwarded
 * to the real Xe_Fatal unchanged. */
static jmp_buf       g_init_jmp;
static volatile int  g_in_init;
static char          g_fatal_msg[160];

void __real_Xe_Fatal(struct XenosDevice *xe, const char *fmt, ...) __attribute__((noreturn));
void __wrap_Xe_Fatal(struct XenosDevice *xe, const char *fmt, ...) __attribute__((noreturn));
void __wrap_Xe_Fatal(struct XenosDevice *xe, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_fatal_msg, sizeof(g_fatal_msg), fmt, ap);
    va_end(ap);
    if (g_in_init) longjmp(g_init_jmp, 1);
    __real_Xe_Fatal(xe, "%s", g_fatal_msg);
}

/* ---- fences ---- */

static inline uint32_t gp_scratch(void) {
    volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)g_xe->rb + GP_SCRATCH_WRITEBACK);
    Xe_pSyncFromDevice(g_xe, p, 4);          /* dcbf: drop our stale cached copy */
    return *p;
}

/* Fence values are xe->frameidx, the same counter Xe_Sync() uses, so mixing
 * in an Xe_Sync() later stays consistent. Wrap-aware comparison. */
static inline int gp_done(uint32_t scratch, uint32_t fence) {
    return (int32_t)(scratch - fence) >= 0;
}

static uint32_t gp_emit_fence_and_kick(void) {
    struct XenosDevice *xe = g_xe;            /* rput32() expands to xe->... */
    uint32_t f = (uint32_t)xe->frameidx++;
    rput32(GP_CP_PACKET0(GP_REG_WAIT_UNTIL)); rput32(0x20000);   /* CP waits for idle */
    rput32(GP_CP_PACKET0(GP_REG_SCRATCH0));   rput32(f);         /* ...then reports f */
    Xe_Execute(xe);                                              /* kick, no CPU wait */
    return f;
}

/* Returns 0 when done, -1 on timeout (counted; we then carry on). */
static int gp_wait_fence(uint32_t fence) {
    uint64_t t0 = gp_now_us();
    while (!gp_done(gp_scratch(), fence)) {
        if (gp_now_us() - t0 > GP_FENCE_TIMEOUT_US) {
            g_st.timeouts++;
            return -1;
        }
        GP_SPIN_PAUSE();
    }
    return 0;
}

/* ---- quad ---- */

static void gp_write_quad(float umax, float vmax) {
    gp_vtx_t *v = Xe_VB_Lock(g_xe, g_vb, 0, 3 * sizeof(gp_vtx_t), XE_LOCK_WRITE);
    /* RECTLIST: top-left, bottom-left, top-right (same as vo_xenon.c update_vb) */
    v[0] = (gp_vtx_t){ -1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    v[1] = (gp_vtx_t){ -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, vmax };
    v[2] = (gp_vtx_t){  1.0f,  1.0f, 0.0f, 1.0f, umax, 0.0f };
    Xe_VB_Unlock(g_xe, g_vb);
}

/* ---- flip ---- */

static void gp_flip_to(int k) {
#if XCLOUD360_VSYNC
    uint64_t t0 = gp_now_us();
    while (!Xe_IsVBlank(g_xe) && gp_now_us() - t0 < 20000) { }
#endif
    if (!g_console_closed) {
        /* From now on the console fb is no longer scanned out: stop drawing
         * text into it (console_putch dcbst's the whole fb per character). */
        console_close();
        g_console_closed = 1;
    }
    Xe_SetFrameBufferSurface(g_xe, g_scan[k]);   /* MMIO 0x6110/0x6120/0x6134/0x6138 */
    g_shown = k;
}

void gpu_present_poll(void) {
    if (!g_active || !g_have_last) return;
    uint32_t s = gp_scratch();
    int best = -1;
    for (int k = 0; k < GP_SCANS; k++) {
        if (!g_scan_pending[k] || !gp_done(s, g_scan_fence[k])) continue;
        if (best < 0 || (int32_t)(g_scan_fence[k] - g_scan_fence[best]) > 0) best = k;
    }
    if (best >= 0) {
        for (int k = 0; k < GP_SCANS; k++) {
            if (k == best || !g_scan_pending[k]) continue;
            if ((int32_t)(g_scan_fence[k] - g_scan_fence[best]) < 0) {
                g_scan_pending[k] = 0;            /* older finished frame: never shown */
                g_st.superseded++;
            }
        }
        g_scan_pending[best] = 0;
        gp_flip_to(best);
        uint64_t t = gp_now_us();
        g_st.flips++;
        g_st.flip_lag_us += t - g_scan_submit_us[best];
        {   /* v0.21.43: Desempenho (GPU %, estimada) */
            uint64_t b = t - g_scan_submit_us[best];
            g_st.busy_us += b > 16667 ? 16667 : b;
        }
        if (g_flip_cb) g_flip_cb(g_scan_tag[best], t);
    }
    for (int i = 0; i < GP_SETS; i++)
        if (g_set_busy[i] && gp_done(s, g_set_fence[i])) g_set_busy[i] = 0;
    /* Xe_ResolveInto() puts one VB per frame on vb_pool_after_frame; libxenon
     * only recycles them in Xe_Sync(). Everything submitted is done here, so
     * recycling all of them is safe. */
    if (g_vb_dirty_frames && gp_done(s, g_last_fence)) {
        Xe_VBReclaim(g_xe);
        g_vb_dirty_frames = 0;
    }
}

/* ---- per frame ---- */

/* v0.21.25: cópia de linha com VMX (lvx/stvx: instruções do Xenon) e dcbz
 * nas linhas de cache de 128 B que a linha de destino cobre inteiras (o
 * memcpy lia cada linha do destino antes de escrever). dst/src/w múltiplos
 * de 16. DIAG.TXT copy=memcpy volta pro caminho antigo. */
volatile int gpu_present_copy_vmx = 0;
static void gp_copy_row_vmx(uint8_t *dst, const uint8_t *src, int w) {
    int x = 0;
    for (; x + 128 <= w; x += 128) {
        uint8_t *d = dst + x;
        const uint8_t *s = src + x;
        __asm__ __volatile__("dcbt 0,%0" : : "r"(s + 256));
        if (((uintptr_t)d & 127) == 0) __asm__ __volatile__("dcbz 0,%0" : : "r"(d) : "memory");
        vector unsigned char v0 = vec_ld(0, s), v1 = vec_ld(16, s), v2 = vec_ld(32, s), v3 = vec_ld(48, s);
        vector unsigned char v4 = vec_ld(64, s), v5 = vec_ld(80, s), v6 = vec_ld(96, s), v7 = vec_ld(112, s);
        vec_st(v0, 0, d); vec_st(v1, 16, d); vec_st(v2, 32, d); vec_st(v3, 48, d);
        vec_st(v4, 64, d); vec_st(v5, 80, d); vec_st(v6, 96, d); vec_st(v7, 112, d);
    }
    for (; x < w; x += 16) vec_st(vec_ld(x, src), x, dst);
}

/* v0.21.43: overlay (Configurações > Desempenho > Mostrar no jogo) */
#define GP_OVL_X 16
#define GP_OVL_Y 16
static const uint8_t *g_ovl_a;
static int g_ovl_w, g_ovl_h;
void gpu_present_set_overlay(const uint8_t *alpha, int w, int h) {
    g_ovl_a = alpha;
    g_ovl_w = w;
    g_ovl_h = h;
}
/* plane 0 = Y (caixa escura + texto), 1/2 = U/V (meia resolução, sem cor na caixa) */
static void gp_ovl_apply(uint8_t *dst, int pitch, int plane, int pw, int ph) {
    const uint8_t *a = g_ovl_a;
    if (!a) return;
    int sh = plane ? 1 : 0;
    int x0 = GP_OVL_X >> sh, y0 = GP_OVL_Y >> sh, w = g_ovl_w >> sh, h = g_ovl_h >> sh;
    if (x0 + w > pw) w = pw - x0;
    if (y0 + h > ph) h = ph - y0;
    for (int y = 0; y < h; y++) {
        uint8_t *d = dst + (y0 + y) * pitch + x0;
        if (plane) {
            for (int x = 0; x < w; x++) d[x] = (uint8_t)(128 + (((int)d[x] - 128) * 64 >> 8));
        } else {
            const uint8_t *m = a + y * g_ovl_w;
            for (int x = 0; x < w; x++) {
                int v = 16 + (((int)d[x] - 16) * 90 >> 8);
                v += (235 - v) * m[x] >> 8;
                d[x] = (uint8_t)v;
            }
        }
    }
}

static void gp_copy_plane(struct XenosSurface *s, const uint8_t *src, int stride, int w, int h, int plane) {
    uint8_t *dst = Xe_Surface_LockRect(g_xe, s, 0, 0, 0, 0, XE_LOCK_WRITE);
    const int pitch = s->wpitch;              /* bytes per row (XE_FMT_8: 1 B/texel) */
    if (gpu_present_copy_vmx && (((uintptr_t)dst | (uintptr_t)src | (unsigned)pitch | (unsigned)stride | (unsigned)w) & 15) == 0) {
        for (int y = 0; y < h; y++) gp_copy_row_vmx(dst + y * pitch, src + y * stride, w);
    } else if (stride == pitch && w == pitch) {
        memcpy(dst, src, (size_t)pitch * h);
    } else {
        for (int y = 0; y < h; y++)
            memcpy(dst + y * pitch, src + y * stride, w);
    }
    if (g_ovl_a) gp_ovl_apply(dst, pitch, plane, w, h);   /* v0.21.43 */
    Xe_Surface_Unlock(g_xe, s);               /* dcbst range + GPU cache invalidate packet */
}

void gpu_present_frame(uint8_t *const data[3], const int linesize[3], int w, int h, uint64_t tag) {
    if (!g_active) return;
    if (w > g_tex_w) w = g_tex_w;
    if (h > g_tex_h) h = g_tex_h;
    w &= ~1; h &= ~1;
    if (w <= 0 || h <= 0) return;

    gpu_present_poll();

    const int s = g_set;
    const int k = g_scan_next;
    uint64_t tw0 = gp_now_us();
    /* Texture set s was last sampled by frame N-2. */
    if (g_set_busy[s]) {
        gp_wait_fence(g_set_fence[s]);
        gpu_present_poll();
        g_set_busy[s] = 0;
    }
    /* Never resolve into the buffer on screen (or one whose flip is still
     * pending). Once N-2 is done, the screen shows N-2 or N-1, i.e. buffer
     * (N-2)%3 or (N-1)%3, so with 3 buffers this is a safety net only: after
     * everything finished, the poll flips to N-1 = buffer (k+2)%3. */
    if (k == g_shown || g_scan_pending[k]) {
        gp_wait_fence(g_last_fence);
        gpu_present_poll();
    }
    /* Quad UVs cover the decoded area of the (fixed-size) textures. Changing
     * the VB needs the GPU idle: rare (stream resolution change). */
    if (w != g_uv_w || h != g_uv_h) {
        if (g_have_last) gp_wait_fence(g_last_fence);
        gp_write_quad((float)w / g_tex_w, (float)h / g_tex_h);
        g_uv_w = w; g_uv_h = h;
    }
    uint64_t tu0 = gp_now_us();
    g_st.wait_us += tu0 - tw0;

    struct XenosSurface **t = g_tex[s];
    gp_copy_plane(t[0], data[0], linesize[0], w,     h,     0);
    gp_copy_plane(t[1], data[1], linesize[1], w / 2, h / 2, 1);
    gp_copy_plane(t[2], data[2], linesize[2], w / 2, h / 2, 2);
    uint64_t tu1 = gp_now_us();
    g_st.upload_us += tu1 - tu0;

    Xe_InvalidateState(g_xe);                 /* defaults: blend ONE/ZERO, no alpha test */
    Xe_SetZEnable(g_xe, 0);
    Xe_SetCullMode(g_xe, XE_CULL_NONE);
    Xe_SetStreamSource(g_xe, 0, g_vb, 0, sizeof(gp_vtx_t));
    Xe_SetShader(g_xe, SHADER_TYPE_PIXEL,  g_ps, 0);
    Xe_SetShader(g_xe, SHADER_TYPE_VERTEX, g_vs, 0);
    Xe_SetTexture(g_xe, 0, t[0]);             /* s0 = YTexture */
    Xe_SetTexture(g_xe, 1, t[1]);             /* s1 = UTexture */
    Xe_SetTexture(g_xe, 2, t[2]);             /* s2 = VTexture */
    Xe_DrawPrimitive(g_xe, XE_PRIMTYPE_RECTLIST, 0, 1);
    Xe_ResolveInto(g_xe, g_scan[k], XE_SOURCE_COLOR, XE_CLEAR_COLOR | XE_CLEAR_DS);
    uint32_t f = gp_emit_fence_and_kick();

    g_set_busy[s] = 1;      g_set_fence[s] = f;
    g_scan_pending[k] = 1;  g_scan_fence[k] = f;
    g_scan_submit_us[k] = gp_now_us();
    g_scan_tag[k] = tag;
    g_last_fence = f;       g_have_last = 1;
    g_vb_dirty_frames++;
    g_st.frames++;

    g_set = (s + 1) % GP_SETS;
    g_scan_next = (k + 1) % GP_SCANS;

    /* Bound the VB pool growth if the main loop stops polling for a while
     * (each unreclaimed frame keeps one ~48 KB pool VB alive). */
    if (g_vb_dirty_frames > 16) {
        gp_wait_fence(f);
        gpu_present_poll();
    }
}

/* ---- init / finish ---- */

static int gp_fail(const char *why) {
    g_in_init = 0;
    printf("[gpu] init FAILED: %s\n", why);
    return -1;
}

/* v0.16 (menu): segunda chamada depois de gpu_present_finish(). Xe_Init de
 * novo resetaria a GPU e o ring, e realocar ~14 MB de superfícies a cada
 * jogo fragmentaria o heap (memalign do libxenon nunca devolve). Tudo já
 * existe: só volta o estado de agendamento pro começo. finish() esperou o
 * último fence e o poll dele marcou tudo como feito; se a GPU travou ali
 * (timeout), esperamos de novo aqui antes de reusar texturas/scanouts. */
static int gp_rearm(int tex_w, int tex_h) {
    if (g_active) return 0;
    if (tex_w != g_tex_w || tex_h != g_tex_h)
        return gp_fail("re-init with a different texture size");
    if (g_have_last && gp_wait_fence(g_last_fence) != 0)
        return gp_fail("GPU still busy from the previous session");
    for (int i = 0; i < GP_SETS; i++) g_set_busy[i] = 0;
    for (int k = 0; k < GP_SCANS; k++) g_scan_pending[k] = 0;
    if (g_vb_dirty_frames) {
        Xe_VBReclaim(g_xe);
        g_vb_dirty_frames = 0;
    }
    g_set = 0;
    g_scan_next = 0;
    g_shown = -1;                             /* finish() pôs o console fb na tela */
    g_active = 1;
    printf("[gpu] re-armed (no Xe_Init): tex %dx%d, %d scanout bufs\n",
           tex_w, tex_h, GP_SCANS);
    return 0;
}

int gpu_present_init(int tex_w, int tex_h) {
    if (g_inited) return gp_rearm(tex_w, tex_h);
    volatile uint32_t *regs = (volatile uint32_t *)GP_REGS_BASE;
    uint32_t sw = regs[0x6134 / 4], sh = regs[0x6138 / 4];
    if (sw == 0 || sh == 0 || sw * sh > 1280u * 720u)
        return gp_fail("scanout not 720p or smaller (EDRAM budget)");
    if (tex_w <= 0 || tex_h <= 0 || (tex_w & 1) || (tex_h & 1))
        return gp_fail("bad texture size");

    if (setjmp(g_init_jmp)) {
        char why[200];
        snprintf(why, sizeof(why), "Xe_Fatal: %s", g_fatal_msg);
        return gp_fail(why);
    }
    g_in_init = 1;

    g_xe = &g_dev;
    Xe_Init(g_xe);                           /* GPU reset, microcode, rings */
    struct XenosSurface *fb = Xe_GetFramebufferSurface(g_xe);
    printf("[gpu] scanout %dx%d pitch=%d @%08x\n", fb->width, fb->height, fb->wpitch,
           (unsigned)fb->ptr);

    for (int i = 0; i < GP_SCANS; i++)
        g_scan[i] = Xe_CreateTexture(g_xe, fb->width, fb->height, 0, XE_FMT_8888 | XE_FMT_BGRA, 1);
    for (int i = 0; i < GP_SETS; i++) {
        g_tex[i][0] = Xe_CreateTexture(g_xe, tex_w,     tex_h,     1, XE_FMT_8, 0);
        g_tex[i][1] = Xe_CreateTexture(g_xe, tex_w / 2, tex_h / 2, 1, XE_FMT_8, 0);
        g_tex[i][2] = Xe_CreateTexture(g_xe, tex_w / 2, tex_h / 2, 1, XE_FMT_8, 0);
        for (int p = 0; p < 3; p++) {
            struct XenosSurface *s = g_tex[i][p];
            s->u_addressing = XE_TEXADDR_CLAMP;
            s->v_addressing = XE_TEXADDR_CLAMP;
            /* use_filtering stays 1 (bilinear), as in xmplayer: chroma needs
             * it for upsampling; luma is sampled 1:1 at 1280x720. */
            /* Start black (Y=16, U=V=128) and flushed. */
            memset(s->base, p == 0 ? 16 : 128, (size_t)s->wpitch * s->hpitch);
            Xe_Surface_LockRect(g_xe, s, 0, 0, 0, 0, XE_LOCK_WRITE);
            Xe_Surface_Unlock(g_xe, s);
        }
    }
    /* Own surface as render target (only viewport + EDRAM layout are taken
     * from it), not &xe->tex_fb which Xe_SetFrameBufferSurface rewrites. */
    Xe_SetRenderTarget(g_xe, g_scan[0]);

    static const struct XenosVBFFormat vbf = { 2, {
        { XE_USAGE_POSITION, 0, XE_TYPE_FLOAT4 },
        { XE_USAGE_TEXCOORD, 0, XE_TYPE_FLOAT2 } } };
    g_ps = Xe_LoadShaderFromMemory(g_xe, (void *)xmp_ps);
    Xe_InstantiateShader(g_xe, g_ps, 0);
    g_vs = Xe_LoadShaderFromMemory(g_xe, (void *)xmp_vs);
    Xe_InstantiateShader(g_xe, g_vs, 0);
    Xe_ShaderApplyVFetchPatches(g_xe, g_vs, 0, &vbf);

    edram_init(g_xe);                        /* may Xe_Fatal -> longjmp above */

    g_vb = Xe_CreateVertexBuffer(g_xe, 3 * sizeof(gp_vtx_t));
    g_tex_w = tex_w; g_tex_h = tex_h;
    gp_write_quad(1.0f, 1.0f);
    g_uv_w = tex_w; g_uv_h = tex_h;
    Xe_SetClearColor(g_xe, 0);
    Xe_InvalidateState(g_xe);

    /* Prove the CP executes our ring and the scratch writeback works before
     * committing to the GPU path. Exact match: SCRATCH0 still holds Xe_Init's
     * 0x1234567 (or edram_init's last Xe_Sync value) before this runs. */
    uint32_t f = gp_emit_fence_and_kick();
    uint64_t t0 = gp_now_us();
    while (gp_scratch() != f) {
        if (gp_now_us() - t0 > 500000) return gp_fail("GPU fence did not complete in 500 ms");
    }
    Xe_VBReclaim(g_xe);
    g_in_init = 0;
    g_active = 1;
    g_inited = 1;
    printf("[gpu] ready: tex %dx%d x%d sets, %d scanout bufs, vsync=%d (fence %u ok in %llu us)\n",
           tex_w, tex_h, GP_SETS, GP_SCANS, XCLOUD360_VSYNC, (unsigned)f,
           (unsigned long long)(gp_now_us() - t0));
    return 0;
}

void gpu_present_finish(void) {
    if (!g_active) return;
    if (g_have_last) gp_wait_fence(g_last_fence);
    gpu_present_poll();
    Xe_SetFrameBufferSurface(g_xe, &g_xe->default_fb);
    g_shown = -1;
    if (g_console_closed) {
        console_open();
        g_console_closed = 0;
    }
    g_active = 0;
}

void gpu_present_set_flip_cb(void (*cb)(uint64_t, uint64_t)) { g_flip_cb = cb; }

void gpu_present_get_stats(gpu_present_stats_t *out) { *out = g_st; }

#else
typedef int xcloud360_gpu_present_disabled;   /* GPU=0: empty translation unit */
#endif /* XCLOUD360_GPU */
