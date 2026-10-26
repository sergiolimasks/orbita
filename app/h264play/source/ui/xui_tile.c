/*
 * XCloud360 — XUI: canvas linear -> framebuffer do console em tiles (Xenos).
 *
 * Layout = o do console.c do libxenon (e do antigo menu_ui.c, que saiu na v0.20.0): blocos de
 * 32x32, dentro deles grupos de 4 pixels contíguos em x; o XOR em y&8 mexe só
 * no bit 5 do offset, então cada grupo de 4 continua contíguo. Copiamos 4 px
 * por vez como 2 stores de 64 bits (o fb é memória não-cacheada: menos
 * stores = menos transações no barramento), com os bytes já na ordem que o
 * Xenos lê (BGRX na memória = o UI_RGB, XUI_FMT_UIRGB).
 * C puro: roda igual no host (bench/teste em hosttest/ui).
 */
#include "xui.h"

#include <string.h>

uint32_t xui_tile_offset(int x, int y, int fb_stride) {
    return (uint32_t)(((y >> 5) * 32 * fb_stride + ((x >> 5) << 10)
                       + (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3)
                       + (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2));
}

static inline uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

/* 0x00RRGGBB nativo -> bytes B,G,R,0 na memória (o que o Xenos lê). No
 * host little-endian o uint32 já fica assim; no PPC (big-endian) troca. */
static inline uint64_t pack2(uint32_t a, uint32_t b) {
    union { uint64_t q; uint32_t w[2]; } u;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    u.w[0] = bswap32(a);
    u.w[1] = bswap32(b);
#else
    u.w[0] = a;
    u.w[1] = b;
#endif
    return u.q;
}

void xui_tile_copy(uint32_t *fb, int fb_stride, int fb_w, int fb_h,
                   const xui_canvas_t *c, xui_rect_t r) {
    xui_rect_t all = { 0, 0, fb_w < c->w ? fb_w : c->w, fb_h < c->h ? fb_h : c->h };
    r = xui_rect_inter(r, all);
    if (xui_rect_empty(r)) return;
    int x0 = r.x & ~3, x1 = (r.x + r.w + 3) & ~3;
    if (x1 > all.w) x1 = all.w & ~3;   /* 1280 é múltiplo de 4 */
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *s = c->px + (long)y * c->stride;
        uint32_t ybase = (uint32_t)((y >> 5) * 32 * fb_stride + ((y & 1) << 2) + (((y & 31) >> 1) << 6));
        uint32_t yx = (uint32_t)((y & 8) << 2);
        for (int x = x0; x < x1; x += 4) {
            uint32_t off = (ybase + ((x >> 5) << 10) + (((x & 31) >> 2) << 3)) ^ yx;
            uint64_t *d = (uint64_t *)(fb + off);   /* off múltiplo de 4 px = 16 B */
            d[0] = pack2(s[x], s[x + 1]);
            d[1] = pack2(s[x + 2], s[x + 3]);
        }
    }
}
