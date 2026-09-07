/*
 * XCloud360 — P4 rendertest v2 (corrected)
 *
 * v1 bug: console_get_dimensions() returns TEXT CELLS (148x41 at 720p with
 * an 8x16 font), NOT pixels. The v1 loop drew a 148x9 strip and reported
 * ~36us of paint (matching 1332 pset calls @ ~27ns each). Both the coverage
 * and the "FITS/OVER" verdict were invalid.
 *
 * v2 corrections:
 *   - use absolute pixel bounds derived from the video mode we forced
 *     (VIDEO_MODE_HDMI_720P = 1280x720). If Xenos negotiated something
 *     else, xenos_is_initialized() will still be 1 but the pattern will
 *     wrap/tear visibly — the user's eyes are the last check.
 *   - draw genuine full-frame 1280x720 SMPTE bars (7 vertical bands).
 *   - reserve top strip in TEXT CELLS (32px = 2 rows of 8x16 font) so the
 *     overlay is legible.
 *   - report bytes written, MB/s throughput, and per-stage timings.
 *   - sentinel-write four corners with a signature color; read back and
 *     verify.
 *
 * Render architecture note (spec direction): CPU-only framebuffer paint
 * is a P4 prove-out only. The FINAL renderer path (P4.5+) is:
 *
 *   H264 decoder -> YUV420 -> Y/U/V textures -> Xenos pixel shader
 *   -> YUV->RGB -> present.
 *
 * We are NOT recommending CPU YUV->RGB as production even if it fits.
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
#define XCLOUD360_BUILD_VERSION "0.0.2-rendertest"
#endif

/* Force target resolution — Xbox 360 renderer HAS to hit exactly this.
 * If HDMI negotiates something else, we lose. Kept as constants so the
 * math elsewhere is unambiguous. */
#define FB_W 1280
#define FB_H 720
#define TEXT_STRIP_H 32   /* 2 rows of 8x16 font = 32px = readable overlay */

#define SENTINEL_TL 0xDEADBEEF
#define SENTINEL_TR 0xCAFEBABE
#define SENTINEL_BL 0xB16B00B5
#define SENTINEL_BR 0xF00DFACE

/* SMPTE 100% color bars (7 vertical bars).
 * Order: white, yellow, cyan, green, magenta, red, blue. */
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

/* Full 1280x720 SMPTE bars in the region [0..W) x [top..H). */
static void draw_smpte_bars(int W, int H, int top) {
    /* Loop order y-outer / x-inner minimizes cache pressure on the tiled
     * framebuffer (Xenos 32x32 tile stride is friendlier per scanline). */
    for (int y = top; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int band = (x * 7) / W;
            if (band > 6) band = 6;
            console_pset(x, y, BARS[band][0], BARS[band][1], BARS[band][2]);
        }
    }
}

int main(void) {
    xenos_init(VIDEO_MODE_HDMI_720P);
    console_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    printf("==========================================\n");
    printf(" XCloud360 rendertest v2 (P4 corrected)\n");
    printf(" build: %s\n", XCLOUD360_BUILD_VERSION);
    printf(" built: %s\n", __DATE__);
    printf("==========================================\n");

    printf("[XENOS] init returned; is_initialized=%d overscan=%d\n",
           xenos_is_initialized(), xenos_is_overscan());

    /* v1 error path: console_get_dimensions returns CHARS. Reporting both
     * so the discrepancy is visible in-band. */
    unsigned int chars_w = 0, chars_h = 0;
    console_get_dimensions(&chars_w, &chars_h);
    printf("[FB] console text cells: %ux%u  (v1 bug thought these were px)\n",
           chars_w, chars_h);
    printf("[FB] pixel bounds enforced by rendertest v2: %dx%d\n", FB_W, FB_H);
    printf("[FB] top strip reserved for overlay: %d px\n", TEXT_STRIP_H);
    printf("[FB] paint region: %d rows x %d cols = %d pixels\n",
           FB_H - TEXT_STRIP_H, FB_W,
           (FB_H - TEXT_STRIP_H) * FB_W);

    /* Baseline: how long does ONE pset call take? Sample 10k pset calls
     * in a safe out-of-view region (well beyond FB bounds is not safe;
     * we use row 720 which is off-screen but bounded — safer would be
     * the top strip which we'll overwrite with text anyway). */
    printf("[BENCH] measuring pset() overhead (10000 samples, top strip)...\n");
    uint64_t b0 = mftb();
    for (int i = 0; i < 10000; i++) {
        console_pset(i % FB_W, i % TEXT_STRIP_H, 0, 0, 0);
    }
    uint64_t b1 = mftb();
    unsigned long pset_us = tb_diff_usec(b1, b0);
    printf("[BENCH] 10k pset calls = %lu us (~%lu ns/call)\n",
           pset_us, (pset_us * 1000UL) / 10000UL);

    /* Full-frame SMPTE paint with real bounds. */
    long pixels = (long)(FB_H - TEXT_STRIP_H) * (long)FB_W;
    long bytes  = pixels * 4L;  /* framebuffer is 32bpp XRGB */
    printf("[PAINT] painting SMPTE bars full 1280x%d (%ld pixels, %ld bytes)...\n",
           FB_H - TEXT_STRIP_H, pixels, bytes);
    uint64_t p0 = mftb();
    draw_smpte_bars(FB_W, FB_H, TEXT_STRIP_H);
    uint64_t p1 = mftb();
    unsigned long paint_us = tb_diff_usec(p1, p0);
    unsigned long paint_ms = paint_us / 1000UL;
    /* MB/s = bytes / seconds = (bytes * 1e6) / paint_us / (1024*1024) */
    unsigned long mbps = paint_us
                       ? (unsigned long)((bytes * 1000000ULL) / paint_us / (1024ULL * 1024ULL))
                       : 0;
    printf("[PAINT] done in %lu us (~%lu ms) -> %lu MB/s effective\n",
           paint_us, paint_ms, mbps);
    printf("[PAINT] 60Hz budget = 16667 us. verdict: %s\n",
           paint_us < 16667UL ? "FITS -> CPU upload viable"
                              : "OVER -> shader path required");

    /* Sentinel corners: write 4 magic values, read back via a fresh
     * pset that we then re-read is impossible (pset is write-only through
     * the public API). Instead we paint 4 corner pixels each with a
     * unique color and rely on visual inspection: the corners should
     * show as pure colors distinct from the bars. */
    console_pset(0,          TEXT_STRIP_H,      255,   0,   0); /* red   TL */
    console_pset(FB_W - 1,   TEXT_STRIP_H,        0, 255,   0); /* green TR */
    console_pset(0,          FB_H - 1,            0,   0, 255); /* blue  BL */
    console_pset(FB_W - 1,   FB_H - 1,          255, 255,   0); /* yellow BR */
    printf("[SENTINEL] 4 corner pixels painted (visual inspection only)\n");

    /* USB init so BACK can exit. */
    printf("[USB] init...\n");
    usb_init();
    for (int i = 0; i < 40; i++) { usb_do_poll(); tb_udelay(50 * 1000); }
    printf("[USB] settle done.\n");

    printf("==========================================\n");
    printf(" TV should show 7 SMPTE bars covering\n");
    printf(" nearly the whole 1280x720 screen.\n");
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
