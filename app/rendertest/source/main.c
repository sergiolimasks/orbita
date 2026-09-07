/*
 * XCloud360 — P4 rendertest (framebuffer @720p, no shader path)
 *
 * Spec §4 asked for "gerar imagem YUV de teste + renderizar 1280x720 @60Hz +
 * testar shader YUV→RGB + gerar pattern". Reality check per §21 ("primeira
 * versão pode ter memcpy") + §31 ("meça antes de otimizar"): the shader
 * path needs Xbox 360 SDK's shadercompiler.exe (Windows+XNA 3.0+VS 2008)
 * to produce .psu/.vsu bytecode — no open-source path exists yet. Deferring
 * shader work until GOLDEN STREAM (P5) hands us real YUV samples to convert.
 *
 * This program proves:
 *   - Xenos can be forced to VIDEO_MODE_HDMI_720P (not AUTO), and initializes.
 *   - console framebuffer paints correctly at 1280x720.
 *   - CPU-side pattern render (SMPTE color bars) is intelligible on the TV.
 *   - loop-per-second timing on core (used later to bound decoder budget).
 *
 * Not proven yet (deferred to P4.5 / P6 integration):
 *   - vsync-locked 60Hz present (needs Xe_Sync / Xe stack + shader).
 *   - YUV texture upload + YUV→RGB shader on the GPU.
 *
 * Spec refs: §4, §20, §21, §29 P4, §31.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <xetypes.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <xenos/xenos_videomodes.h>
#include <xenon_soc/xenon_power.h>
#include <ppc/timebase.h>
#include <input/input.h>
#include <usb/usbmain.h>

#ifndef XCLOUD360_BUILD_VERSION
#define XCLOUD360_BUILD_VERSION "0.0.1-rendertest"
#endif

/* SMPTE 100% color bars (7 vertical bars).
 * Order: white, yellow, cyan, green, magenta, red, blue (100% amplitude). */
static const uint8_t BARS[7][3] = {
    {255, 255, 255},   /* white   */
    {255, 255,   0},   /* yellow  */
    {  0, 255, 255},   /* cyan    */
    {  0, 255,   0},   /* green   */
    {255,   0, 255},   /* magenta */
    {255,   0,   0},   /* red     */
    {  0,   0, 255},   /* blue    */
};

static void tb_udelay(uint64_t usec) {
    uint64_t start = mftb();
    uint64_t ticks = usec * (PPC_TIMEBASE_FREQ / 1000000ULL);
    while ((mftb() - start) < ticks) { }
}

static void draw_smpte_bars(unsigned int w, unsigned int h,
                            unsigned int header_h) {
    /* header_h rows at top left for text. Bars fill the remainder. */
    for (unsigned int x = 0; x < w; x++) {
        int band = (x * 7) / w;              /* 0..6 */
        if (band > 6) band = 6;
        uint8_t r = BARS[band][0];
        uint8_t g = BARS[band][1];
        uint8_t b = BARS[band][2];
        for (unsigned int y = header_h; y < h; y++) {
            console_pset((int)x, (int)y, r, g, b);
        }
    }
}

int main(void) {
    /* Force HDMI 720p (rather than AUTO) — this is our target output for the
     * whole project. If HDMI negotiation refuses 720p, we want the failure
     * to be visible, not silently downgraded. */
    xenos_init(VIDEO_MODE_HDMI_720P);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    printf("========================================\n");
    printf(" XCloud360 rendertest (P4)\n");
    printf(" build: %s\n", XCLOUD360_BUILD_VERSION);
    printf(" built: %s\n", __DATE__);
    printf("========================================\n");

    /* Verify Xenos init succeeded. */
    printf("[XENOS] init returned; is_initialized=%d\n", xenos_is_initialized());
    printf("[XENOS] overscan flag: %d\n", xenos_is_overscan());

    /* Report console framebuffer dimensions (this is the actual pixel
     * resolution the framebuffer is being painted at). */
    unsigned int w = 0, h = 0;
    console_get_dimensions(&w, &h);
    printf("[FB] console dimensions: %u x %u\n", w, h);

    if (w != 1280 || h != 720) {
        printf("[FB] WARN: expected 1280x720, got %ux%u\n", w, h);
        printf("[FB]       HDMI may have negotiated a different mode\n");
    }

    /* Paint SMPTE color bars. Reserve top 32 rows for text overlay. */
    printf("[PATTERN] painting SMPTE color bars (this takes a moment)...\n");
    uint64_t t0 = mftb();
    draw_smpte_bars(w, h, 32);
    uint64_t t1 = mftb();
    unsigned long paint_us = tb_diff_usec(t1, t0);
    printf("[PATTERN] paint done in %lu us (~%lu ms)\n",
           paint_us, paint_us / 1000);

    /* Loop-per-second measurement. Idle spin gives a baseline for how
     * many CPU cycles are available per frame at 60Hz (16.67 ms). */
    printf("[LOOP] measuring 300-iteration spin rate...\n");
    uint64_t l0 = mftb();
    for (int i = 0; i < 300; i++) {
        /* No-op body — measures pure loop + mftb overhead. */
        asm volatile ("" ::: "memory");
    }
    uint64_t l1 = mftb();
    unsigned long spin_us = tb_diff_usec(l1, l0);
    printf("[LOOP] 300 empty loops = %lu us (~%lu ns/loop)\n",
           spin_us, (spin_us * 1000UL) / 300UL);

    /* Now the useful measurement: how long does one full frame's worth of
     * pixel-by-pixel writes take? This bounds any CPU-side upload strategy.
     * We only paint the frame ONCE and time it — 1280x720 = 921,600 pixels.
     * If this is >> 16.67 ms then CPU-side upload is impossible at 60 FPS
     * and the shader path becomes mandatory (as spec §21 predicts). */
    printf("[BUDGET] timing a full-frame CPU repaint (worst case)...\n");
    uint64_t p0 = mftb();
    draw_smpte_bars(w, h, 32);
    uint64_t p1 = mftb();
    unsigned long full_us = tb_diff_usec(p1, p0);
    printf("[BUDGET] full 1280x720 CPU repaint = %lu us (%lu ms)\n",
           full_us, full_us / 1000);
    printf("[BUDGET] 60Hz budget = 16667 us. %s\n",
           full_us < 16667 ? "FITS -> CPU upload viable"
                           : "OVER -> shader path required");

    /* USB init so BACK can exit. */
    printf("[USB] init...\n");
    usb_init();
    for (int i = 0; i < 40; i++) { usb_do_poll(); tb_udelay(50 * 1000); }
    printf("[USB] settle done.\n");

    printf("========================================\n");
    printf(" PATTERN VISIBLE ON TV = renderer PASS\n");
    printf(" Press BACK to exit.\n");
    printf("========================================\n");

    /* Idle loop. */
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
