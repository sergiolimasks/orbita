/*
 * XCloud360 — XUI backend Xbox: canvas (RAM cacheada) -> framebuffer do
 * console. Só o retângulo sujo é copiado (xui_tile_copy: tiles 32x32 +
 * troca de bytes), e o fb é o mesmo que o console usa (lido do
 * registrador de scanout, alias não-cacheado |0x80000000).
 *
 * Só no ELF (v0.20.0: sempre); o host usa hosttest/ui/xui_png.c no lugar.
 * Main thread apenas. Não desenha nada sozinho: quem chama decide quando
 * (o loop do menu; nunca durante o jogo — o scanout é da GPU então).
 */
#include "xui_fb_xenon.h"

#include <stdlib.h>
#include <ppc/cache.h>

static uint32_t *g_fb;
static int       g_w, g_h, g_stride, g_hpad;
static uint32_t *g_px;
static xui_canvas_t g_canvas;

int xui_fb_attach(void) {
    struct ati_info { uint32_t u1[4]; uint32_t base; uint32_t u2[8]; uint32_t width, height; } __attribute__((packed));
    volatile struct ati_info *ai = (struct ati_info *)0xec806100ULL;
    uint32_t base = ai->base, w = ai->width, h = ai->height;
    if (w == 0 || w > 4096 || h == 0 || h > 4096) { g_fb = NULL; return -1; }
    g_fb = (uint32_t *)(unsigned long)(base | 0x80000000);
    g_stride = (int)(((w + 31) >> 5) << 5);
    g_hpad = (int)(((h + 31) >> 5) << 5);
    if (!g_px || g_w != (int)w || g_h != (int)h) {
        free(g_px);
        g_px = (uint32_t *)malloc((size_t)w * h * 4);
        if (!g_px) { g_fb = NULL; return -2; }
        g_w = (int)w;
        g_h = (int)h;
        xui_canvas_init(&g_canvas, g_px, g_w, g_h, g_w);
    }
    return 0;
}

xui_canvas_t *xui_fb_canvas(void) { return g_px ? &g_canvas : NULL; }

void xui_fb_present(xui_rect_t r) {
    if (!g_fb || !g_px || xui_rect_empty(r)) return;
    xui_tile_copy(g_fb, g_stride, g_w, g_h, &g_canvas, r);
    /* alias não-cacheado: praticamente no-op, mas garante */
    uint32_t y0 = (uint32_t)(r.y >> 5) * 32 * (uint32_t)g_stride;
    uint32_t y1 = (uint32_t)((r.y + r.h + 31) >> 5) * 32 * (uint32_t)g_stride;
    memdcbst(g_fb + y0, (y1 - y0) * 4);
}

