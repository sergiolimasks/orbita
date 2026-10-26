/*
 * XCloud360 — XUI primitivas de desenho (C puro). Ver xui.h.
 * Tudo respeita c->clip; inteiros/fixo onde é quente (cópias, escala),
 * float só nos cantos arredondados (poucos pixels).
 */
#include "xui.h"

#include <math.h>
#include <string.h>

void xui_canvas_init(xui_canvas_t *c, uint32_t *px, int w, int h, int stride) {
    c->px = px;
    c->w = w;
    c->h = h;
    c->stride = stride;
    xui_clip_all(c);
}

xui_rect_t xui_rect_inter(xui_rect_t a, xui_rect_t b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    xui_rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w <= 0 || r.h <= 0) r.w = r.h = 0;
    return r;
}

xui_rect_t xui_rect_union(xui_rect_t a, xui_rect_t b) {
    if (xui_rect_empty(a)) return b;
    if (xui_rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    xui_rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

void xui_clip_all(xui_canvas_t *c) {
    xui_rect_t r = { 0, 0, c->w, c->h };
    c->clip = r;
}

void xui_set_clip(xui_canvas_t *c, xui_rect_t r) {
    xui_rect_t all = { 0, 0, c->w, c->h };
    c->clip = xui_rect_inter(r, all);
}

/* Recorta (x,y,w,h) ao clip; 0 = nada sobra. */
static int clipr(const xui_canvas_t *c, int *x, int *y, int *w, int *h) {
    xui_rect_t r = { *x, *y, *w, *h };
    r = xui_rect_inter(r, c->clip);
    if (r.w <= 0) return 0;
    *x = r.x; *y = r.y; *w = r.w; *h = r.h;
    return 1;
}

static inline uint32_t *row(const xui_canvas_t *c, int y) { return c->px + (long)y * c->stride; }

void xui_fill(xui_canvas_t *c, int x, int y, int w, int h, uint32_t col) {
    if (!clipr(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = row(c, y + j) + x;
        for (int i = 0; i < w; i++) p[i] = col;
    }
}

void xui_fill_a(xui_canvas_t *c, int x, int y, int w, int h, uint32_t col, unsigned a) {
    if (a >= 255) { xui_fill(c, x, y, w, h, col); return; }
    if (a == 0 || !clipr(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = row(c, y + j) + x;
        for (int i = 0; i < w; i++) p[i] = xui_mix(p[i], col, a);
    }
}

static inline void plot_a(xui_canvas_t *c, int x, int y, uint32_t col, unsigned a) {
    if (a == 0) return;
    if (x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h) return;
    uint32_t *p = row(c, y) + x;
    *p = a >= 255 ? col : xui_mix(*p, col, a);
}

/* Cobertura (0..255) do pixel (px,py) de um retângulo arredondado
 * [x0,x1)x[y0,y1) de raio r: só vale perto dos cantos. */
static unsigned rr_cov(float px, float py, float x0, float y0, float x1, float y1, float r) {
    float cx = px < x0 + r ? x0 + r : (px > x1 - r ? x1 - r : px);
    float cy = py < y0 + r ? y0 + r : (py > y1 - r ? y1 - r : py);
    float dx = px - cx, dy = py - cy;
    if (dx == 0.f && dy == 0.f) {
        /* dentro (ou na borda reta): cobertura pela distância à borda */
        float e = px - x0;
        if (x1 - px < e) e = x1 - px;
        if (py - y0 < e) e = py - y0;
        if (y1 - py < e) e = y1 - py;
        e += 0.0f;
        return e >= 0.5f ? 255u : (unsigned)((e + 0.5f) * 255.f);
    }
    float d = sqrtf(dx * dx + dy * dy);
    float cov = r - d + 0.5f;
    if (cov <= 0.f) return 0;
    if (cov >= 1.f) return 255;
    return (unsigned)(cov * 255.f);
}

void xui_round_rect(xui_canvas_t *c, int x, int y, int w, int h, int r, uint32_t col, unsigned alpha) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { xui_fill_a(c, x, y, w, h, col, alpha); return; }
    /* miolo sem cantos */
    xui_fill_a(c, x, y + r, w, h - 2 * r, col, alpha);
    xui_fill_a(c, x + r, y, w - 2 * r, r, col, alpha);
    xui_fill_a(c, x + r, y + h - r, w - 2 * r, r, col, alpha);
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            unsigned cv = rr_cov(x + i + 0.5f, y + j + 0.5f, (float)x, (float)y, (float)(x + w), (float)(y + h), (float)r);
            cv = cv * alpha / 255;
            plot_a(c, x + i, y + j, col, cv);
            plot_a(c, x + w - 1 - i, y + j, col, cv);
            plot_a(c, x + i, y + h - 1 - j, col, cv);
            plot_a(c, x + w - 1 - i, y + h - 1 - j, col, cv);
        }
    }
}

void xui_round_frame(xui_canvas_t *c, int x, int y, int w, int h, int r, int t, uint32_t col) {
    int ri = r - t > 0 ? r - t : 0;
    /* lados retos */
    xui_fill(c, x + r, y, w - 2 * r, t, col);
    xui_fill(c, x + r, y + h - t, w - 2 * r, t, col);
    xui_fill(c, x, y + r, t, h - 2 * r, col);
    xui_fill(c, x + w - t, y + r, t, h - 2 * r, col);
    /* cantos: externo menos interno */
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            float px = x + i + 0.5f, py = y + j + 0.5f;
            unsigned co = rr_cov(px, py, (float)x, (float)y, (float)(x + w), (float)(y + h), (float)r);
            unsigned ci = rr_cov(px, py, (float)(x + t), (float)(y + t), (float)(x + w - t), (float)(y + h - t), (float)ri);
            unsigned cv = co > ci ? co - ci : 0;
            plot_a(c, x + i, y + j, col, cv);
            plot_a(c, x + w - 1 - i, y + j, col, cv);
            plot_a(c, x + i, y + h - 1 - j, col, cv);
            plot_a(c, x + w - 1 - i, y + h - 1 - j, col, cv);
        }
    }
}

void xui_circle(xui_canvas_t *c, int cx, int cy, int rad, uint32_t col, unsigned alpha) {
    xui_round_rect(c, cx - rad, cy - rad, rad * 2, rad * 2, rad, col, alpha);
}

void xui_vgrad(xui_canvas_t *c, int x, int y, int w, int h, uint32_t col, unsigned a0, unsigned a1) {
    if (h <= 0) return;
    int y0 = y;
    int x2 = x, y2 = y, w2 = w, h2 = h;
    if (!clipr(c, &x2, &y2, &w2, &h2)) return;
    for (int j = y2; j < y2 + h2; j++) {
        unsigned a = a0 + (unsigned)(((int)a1 - (int)a0) * (j - y0) / (h > 1 ? h - 1 : 1));
        uint32_t *p = row(c, j) + x2;
        if (a >= 255) { for (int i = 0; i < w2; i++) p[i] = col; continue; }
        if (a == 0) continue;
        for (int i = 0; i < w2; i++) p[i] = xui_mix(p[i], col, a);
    }
}

void xui_hgrad(xui_canvas_t *c, int x, int y, int w, int h, uint32_t col, unsigned a0, unsigned a1) {
    if (w <= 0) return;
    int x0 = x;
    int x2 = x, y2 = y, w2 = w, h2 = h;
    if (!clipr(c, &x2, &y2, &w2, &h2)) return;
    unsigned char al[2048];
    if (w2 > 2048) w2 = 2048;
    for (int i = 0; i < w2; i++)
        al[i] = (unsigned char)(a0 + ((int)a1 - (int)a0) * (x2 + i - x0) / (w > 1 ? w - 1 : 1));
    for (int j = y2; j < y2 + h2; j++) {
        uint32_t *p = row(c, j) + x2;
        for (int i = 0; i < w2; i++) if (al[i]) p[i] = xui_mix(p[i], col, al[i]);
    }
}

void xui_blit(xui_canvas_t *c, const xui_image_t *img, int x, int y, int r) {
    int bx = x, by = y, bw = img->w, bh = img->h;
    if (!clipr(c, &bx, &by, &bw, &bh)) return;
    if (r * 2 > img->w) r = img->w / 2;
    for (int j = by; j < by + bh; j++) {
        int sy = j - y;
        const uint32_t *s = img->px + (long)sy * img->stride + (bx - x);
        uint32_t *d = row(c, j) + bx;
        int corner_row = r > 0 && (sy < r || sy >= img->h - r);
        if (!corner_row) { memcpy(d, s, (size_t)bw * 4); continue; }
        for (int i = 0; i < bw; i++) {
            int sx = bx - x + i;
            if (sx >= r && sx < img->w - r) { d[i] = s[i]; continue; }
            unsigned cv = rr_cov(sx + 0.5f, sy + 0.5f, 0.f, 0.f, (float)img->w, (float)img->h, (float)r);
            if (cv) d[i] = cv >= 255 ? s[i] : xui_mix(d[i], s[i], cv);
        }
    }
}

void xui_sprite(xui_canvas_t *c, const xui_sprite_t *s, int x, int y) {
    int bx = x, by = y, bw = s->w, bh = s->h;
    if (!clipr(c, &bx, &by, &bw, &bh)) return;
    for (int j = by; j < by + bh; j++) {
        long o = (long)(j - y) * s->w + (bx - x);
        const unsigned char *a = s->a + o;
        const uint32_t *p = s->px + o;
        uint32_t *d = row(c, j) + bx;
        for (int i = 0; i < bw; i++) {
            if (!a[i]) continue;
            d[i] = a[i] >= 255 ? p[i] : xui_mix(d[i], p[i], a[i]);
        }
    }
}

void xui_mask(xui_canvas_t *c, const xui_mask_t *m, int x, int y, uint32_t col, unsigned alpha) {
    int bx = x, by = y, bw = m->w, bh = m->h;
    if (!clipr(c, &bx, &by, &bw, &bh)) return;
    for (int j = by; j < by + bh; j++) {
        const unsigned char *a = m->a + (long)(j - y) * m->w + (bx - x);
        uint32_t *d = row(c, j) + bx;
        for (int i = 0; i < bw; i++) {
            unsigned v = a[i];
            if (!v) continue;
            if (alpha < 255) v = v * alpha / 255;
            d[i] = v >= 255 ? col : xui_mix(d[i], col, v);
        }
    }
}

/* 0x00BBGGRR -> 0x00RRGGBB */
static void swap_rb(uint32_t *p, int w, int rows) {
    for (long i = 0, n = (long)w * rows; i < n; i++) {
        uint32_t v = p[i];
        p[i] = ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
    }
}

void xui_scale_cover(uint32_t *dst, int dw, int dh, const xui_image_t *src) {
    xui_scale_cover_part(dst, dw, dh, src, dh);
}

void xui_scale_cover_part(uint32_t *dst, int dw, int dh, const xui_image_t *src, int rows) {
    if (rows > dh) rows = dh;
    /* escala que cobre dw x dh, recorte centralizado */
    long sw = src->w, sh = src->h;
    /* passo em 16.16: fonte por pixel de destino */
    long step;
    if (sw * dh > sh * dw) step = (sh << 16) / dh;   /* fonte mais larga: casa altura */
    else step = (sw << 16) / dw;
    long ox = ((sw << 16) - step * dw) / 2, oy = ((sh << 16) - step * dh) / 2;
    if (dw > XUI_MAX_W || dw <= 0 || sw <= 0 || sh <= 0) return;   /* tabelas abaixo */
    /* XUI_FMT_UIRGB (catálogo: B<<24|G<<16|R<<8): p>>8 = 0x00BBGGRR, a
     * mesma conta por canal serve; no fim troca R<->B nas linhas feitas. */
    const unsigned sh8 = src->fmt == XUI_FMT_UIRGB ? 8u : 0u;

    /* 1:1 (o catálogo já pediu no tamanho certo): só copia */
    if (step == 65536 && !(ox & 0xffff) && !(oy & 0xffff)) {
        for (int y = 0; y < rows; y++) {
            const uint32_t *s = src->px + (long)(y + (oy >> 16)) * src->stride + (ox >> 16);
            uint32_t *d = dst + (long)y * dw;
            if (!sh8) { memcpy(d, s, (size_t)dw * 4); continue; }
            for (int x = 0; x < dw; x++) d[x] = s[x] >> 8;
        }
        if (sh8) swap_rb(dst, dw, rows);
        return;
    }

    /* Single-thread (só o main desenha): tabelas estáticas, nada na pilha. */
    static int           tx[XUI_MAX_W];
    static unsigned char ta[XUI_MAX_W];

    if (step >= (2 << 16)) {
        /* Redução >= 2x: média de um bloco n x n (n = 2..4) por pixel — sem
         * serrilhado e bem mais barato que várias amostras bilineares.
         * Soma empacotada: R e B no mesmo uint32 (n*n <= 16 não estoura). */
        int n = (int)(step >> 16);
        if (n > 4) n = 4;
        int nn = n * n;
        for (int x = 0; x < dw; x++) {
            long sx = (ox + step * x + step / 2) >> 16;
            sx -= n / 2;
            if (sx < 0) sx = 0;
            if (sx > sw - n) sx = sw - n;
            tx[x] = (int)sx;
        }
        for (int y = 0; y < rows; y++) {
            long sy = ((oy + step * y + step / 2) >> 16) - n / 2;
            if (sy < 0) sy = 0;
            if (sy > sh - n) sy = sh - n;
            uint32_t *d = dst + (long)y * dw;
            for (int x = 0; x < dw; x++) {
                uint32_t rb = 0, g = 0;
                const uint32_t *p = src->px + sy * src->stride + tx[x];
                for (int j = 0; j < n; j++, p += src->stride)
                    for (int i = 0; i < n; i++) {
                        uint32_t q = p[i] >> sh8;
                        rb += q & 0xff00ff; g += q & 0x00ff00;
                    }
                if (nn == 4)       { rb >>= 2; g >>= 2; }
                else if (nn == 16) { rb >>= 4; g >>= 4; }
                else {
                    uint32_t r = (rb >> 16) / (uint32_t)nn, b = (rb & 0xffff) / (uint32_t)nn;
                    rb = (r << 16) | b;
                    g = (g / (uint32_t)nn);
                }
                d[x] = (rb & 0xff00ff) | (g & 0x00ff00);
            }
        }
        if (sh8) swap_rb(dst, dw, rows);
        return;
    }

    /* Bilinear (ampliação ou redução < 2x), com as colunas pré-calculadas. */
    for (int x = 0; x < dw; x++) {
        long fx = ox + step * x + step / 2 - 32768;
        if (fx < 0) fx = 0;
        long ix = fx >> 16;
        unsigned a = (unsigned)(fx >> 8) & 0xff;
        if (ix >= sw - 1) { ix = sw - 2 >= 0 ? sw - 2 : 0; a = sw > 1 ? 255 : 0; }
        tx[x] = (int)ix;
        ta[x] = (unsigned char)a;
    }
    for (int y = 0; y < rows; y++) {
        long fy = oy + step * y + step / 2 - 32768;
        if (fy < 0) fy = 0;
        long iy = fy >> 16;
        unsigned ay = (unsigned)(fy >> 8) & 0xff;
        long iy1 = iy + 1 < sh ? iy + 1 : sh - 1;
        if (iy >= sh) iy = iy1 = sh - 1;
        const uint32_t *r0 = src->px + iy * src->stride, *r1 = src->px + iy1 * src->stride;
        uint32_t *d = dst + (long)y * dw;
        int x1off = sw > 1 ? 1 : 0;
        for (int x = 0; x < dw; x++) {
            int ix = tx[x];
            unsigned ax = ta[x];
            uint32_t top = xui_mix(r0[ix] >> sh8, r0[ix + x1off] >> sh8, ax);
            uint32_t bot = xui_mix(r1[ix] >> sh8, r1[ix + x1off] >> sh8, ax);
            d[x] = ay ? xui_mix(top, bot, ay) : top;
        }
    }
    if (sh8) swap_rb(dst, dw, rows);
}

void xui_copy_from(xui_canvas_t *c, const uint32_t *src, int src_stride, xui_rect_t r) {
    int x = r.x, y = r.y, w = r.w, h = r.h;
    if (!clipr(c, &x, &y, &w, &h)) return;
    for (int j = y; j < y + h; j++)
        memcpy(row(c, j) + x, src + (long)j * src_stride + x, (size_t)w * 4);
}
