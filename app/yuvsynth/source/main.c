/*
 * XCloud360 — P4.5 yuvsynth (synthetic YUV420 pattern + CPU YUV->RGB present)
 *
 * Purpose: prove the full "YUV420 source -> present" path end-to-end BEFORE
 * we have the GOLDEN STREAM. Uses a deterministic synthetic YUV420 frame so
 * the output pixels are predictable and any visual defect points at a
 * pipeline bug (color-conversion / stride / bounds), not at "the input is
 * weird".
 *
 * The YUV -> RGB conversion here runs on the CPU (BT.601 full-range, scalar
 * for clarity). This is the SIMPLEST viable path — not the production one.
 * Sergio's spec says the FINAL renderer must be:
 *
 *   H264 decoder -> YUV420 -> Y/U/V textures -> Xenos pixel shader
 *   -> YUV->RGB -> HDMI
 *
 * so the CPU is preserved for H.264/WebRTC/Opus/input.
 *
 * Shader path status (as of writing):
 *   - Xbox 360 pixel-shader bytecode requires shadercompiler.exe (Windows +
 *     XNA 3.0 + VS 2008), no open-source producer exists.
 *   - XenosRecomp (hedge-dev) is a DIS-assembler (bytecode -> HLSL), useful
 *     for reading, not producing.
 *   - XBMC-360 does its YUV->RGB via libswscale AltiVec (CPU), NOT a Xenos
 *     shader — confirming there's no drop-in .psu we can borrow.
 *
 * So this file is the interim "prove the frame is intelligible" step.
 * A follow-up (P4.6+) will replace the CPU convert with a Xenos shader
 * once we either (a) hand-author bytecode against the Xenos ISA (Xenia
 * has partial docs), or (b) discover a pre-compiled .psu somewhere.
 *
 * Pattern:
 *   Y plane   : diagonal gradient (0..255) — highlights any stride/luma bug.
 *   U/V plane : SMPTE-like color bars in chroma space — 7 vertical bands
 *               that map back to expected RGB colors after the convert.
 *
 * Spec refs: §4, §9 (video pipeline), §11 (FFmpeg minimal), §20-§21
 *            (renderer / memcpy), §29 P4 target, §31 (measure before opt).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <xetypes.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <xenos/xenos_videomodes.h>
#include <xenon_soc/xenon_power.h>
#include <ppc/timebase.h>
#include <input/input.h>
#include <usb/usbmain.h>

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.0.1-yuvsynth"
#endif

#define FB_W 1280
#define FB_H 720
#define TEXT_STRIP_H 32

/* YUV420: Y is full-res, U/V are quarter-res (half W, half H). */
#define UV_W (FB_W / 2)
#define UV_H (FB_H / 2)

/* SMPTE-like bar RGB targets — same order as our RGB test.
 * The synthetic U/V pattern will be authored so BT.601 CPU convert
 * reproduces THESE colors. */
static const uint8_t BAR_RGB[7][3] = {
    {255, 255, 255},   /* white   */
    {255, 255,   0},   /* yellow  */
    {  0, 255, 255},   /* cyan    */
    {  0, 255,   0},   /* green   */
    {255,   0, 255},   /* magenta */
    {255,   0,   0},   /* red     */
    {  0,   0, 255},   /* blue    */
};

/* BT.601 full-range RGB -> YUV (per-band Y/U/V constants).
 * Y = 0.299R + 0.587G + 0.114B
 * U = -0.169R - 0.331G + 0.500B + 128
 * V = 0.500R - 0.419G - 0.081B + 128
 */
static uint8_t clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static void rgb_to_yuv601(uint8_t r, uint8_t g, uint8_t b,
                          uint8_t *y, uint8_t *u, uint8_t *v) {
    int Yi = ( 299 * r + 587 * g + 114 * b) / 1000;
    int Ui = (-169 * r - 331 * g + 500 * b) / 1000 + 128;
    int Vi = ( 500 * r - 419 * g - 81  * b) / 1000 + 128;
    *y = clamp255(Yi);
    *u = clamp255(Ui);
    *v = clamp255(Vi);
}

/* Convert YUV (BT.601 full-range) back to RGB. */
static void yuv_to_rgb601(uint8_t y, uint8_t u, uint8_t v,
                          uint8_t *r, uint8_t *g, uint8_t *b) {
    int Y = (int)y;
    int U = (int)u - 128;
    int V = (int)v - 128;
    int Ri = Y + (1402 * V) / 1000;
    int Gi = Y - (344 * U + 714 * V) / 1000;
    int Bi = Y + (1772 * U) / 1000;
    *r = clamp255(Ri);
    *g = clamp255(Gi);
    *b = clamp255(Bi);
}

/* Fill Y plane with per-band Y values (SMPTE bars). We do NOT do a gradient
 * here — for visual verification, each band must be a solid, uniform color.
 * A gradient is easier to author but hides U/V bugs behind Y contrast. */
static void fill_planes_smpte(uint8_t *Y, uint8_t *U, uint8_t *V) {
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            int band = (x * 7) / FB_W;
            if (band > 6) band = 6;
            uint8_t yy, uu, vv;
            rgb_to_yuv601(BAR_RGB[band][0], BAR_RGB[band][1], BAR_RGB[band][2],
                          &yy, &uu, &vv);
            Y[y * FB_W + x] = yy;
            /* subsample U/V for YUV420 */
            if ((x & 1) == 0 && (y & 1) == 0) {
                U[(y / 2) * UV_W + (x / 2)] = uu;
                V[(y / 2) * UV_W + (x / 2)] = vv;
            }
        }
    }
}

/* CPU-side YUV420 -> RGB present via console_pset.
 * This is NOT the production path — see file header. */
static void present_yuv420_cpu(const uint8_t *Y, const uint8_t *U,
                               const uint8_t *V, int top) {
    for (int y = top; y < FB_H; y++) {
        int uv_row = y / 2;
        for (int x = 0; x < FB_W; x++) {
            int uv_col = x / 2;
            uint8_t yy = Y[y * FB_W + x];
            uint8_t uu = U[uv_row * UV_W + uv_col];
            uint8_t vv = V[uv_row * UV_W + uv_col];
            uint8_t r, g, b;
            yuv_to_rgb601(yy, uu, vv, &r, &g, &b);
            console_pset(x, y, r, g, b);
        }
    }
}

static void tb_udelay(uint64_t usec) {
    uint64_t start = mftb();
    uint64_t ticks = usec * (PPC_TIMEBASE_FREQ / 1000000ULL);
    while ((mftb() - start) < ticks) { }
}

int main(void) {
    xenos_init(VIDEO_MODE_HDMI_720P);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    printf("==========================================\n");
    printf(" XCloud360 yuvsynth (P4.5 interim)\n");
    printf(" build: %s\n", XCLOUD360_BUILD_VERSION);
    printf(" built: %s\n", __DATE__);
    printf("==========================================\n");
    printf("[XENOS] is_initialized=%d overscan=%d\n",
           xenos_is_initialized(), xenos_is_overscan());

    /* Allocate Y/U/V planes. Y = 1280*720 = 900 KiB. UV each = 640*360 = 225 KiB.
     * Total ~1.35 MiB. Fits in the ~5 MiB .bss budget the linker script gives. */
    static uint8_t Y[FB_W * FB_H];
    static uint8_t U[UV_W * UV_H];
    static uint8_t V[UV_W * UV_H];
    printf("[MEM] Y=%d bytes  U=%d bytes  V=%d bytes  total=%d\n",
           (int)sizeof(Y), (int)sizeof(U), (int)sizeof(V),
           (int)(sizeof(Y) + sizeof(U) + sizeof(V)));

    /* Author YUV420 pattern once. */
    printf("[SYNTH] filling YUV420 planes (SMPTE bars)...\n");
    uint64_t s0 = mftb();
    fill_planes_smpte(Y, U, V);
    uint64_t s1 = mftb();
    printf("[SYNTH] done in %lu us\n", tb_diff_usec(s1, s0));

    /* Sanity: report the Y values of each band (should be non-uniform). */
    for (int b = 0; b < 7; b++) {
        int x = (b * FB_W / 7) + (FB_W / 14);   /* center of band */
        uint8_t yy = Y[100 * FB_W + x];
        int uv_col = x / 2;
        uint8_t uu = U[50 * UV_W + uv_col];
        uint8_t vv = V[50 * UV_W + uv_col];
        printf("[SYNTH] band %d: Y=%3u  U=%3u  V=%3u\n", b, yy, uu, vv);
    }

    /* Present. */
    long paint_pixels = (long)FB_W * (long)(FB_H - TEXT_STRIP_H);
    printf("[PRESENT] CPU YUV->RGB + console_pset for %ld pixels...\n",
           paint_pixels);
    uint64_t p0 = mftb();
    present_yuv420_cpu(Y, U, V, TEXT_STRIP_H);
    uint64_t p1 = mftb();
    unsigned long present_us = tb_diff_usec(p1, p0);
    unsigned long present_ms = present_us / 1000UL;
    unsigned long fps_upper = present_us
                            ? (unsigned long)(1000000ULL / present_us)
                            : 0;
    printf("[PRESENT] done in %lu us (~%lu ms). CPU-path fps ceiling: ~%lu\n",
           present_us, present_ms, fps_upper);
    printf("[PRESENT] 60Hz budget = 16667 us. verdict: %s\n",
           present_us < 16667UL
               ? "FITS (CPU convert usable as fallback)"
               : "OVER (shader path mandatory for 60 fps)");

    /* USB init so BACK exits. */
    printf("[USB] init...\n");
    usb_init();
    for (int i = 0; i < 40; i++) { usb_do_poll(); tb_udelay(50 * 1000); }
    printf("[USB] settle done.\n");

    printf("==========================================\n");
    printf(" TV should show 7 SMPTE bars converted\n");
    printf(" from a YUV420 source. Colors should\n");
    printf(" match the RGB-native rendertest2.\n");
    printf(" Press BACK to exit.\n");
    printf("==========================================\n");

    for (;;) {
        usb_do_poll();
        for (int p = 0; p < 4; p++) {
            struct controller_data_s pad;
            if (get_controller_data(&pad, p) && pad.back) {
                return 0;
            }
        }
        tb_udelay(20 * 1000);
    }
}
