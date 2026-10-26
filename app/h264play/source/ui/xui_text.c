/*
 * XCloud360 — XUI texto: Inter rasterizada (xui_assets.h), UTF-8 -> Latin-1,
 * kerning, reticências e quebra de linha. Ver xui.h.
 */
#include "xui.h"
#include "xui_assets.h"

#include <string.h>

const xui_face_t *xui_face(int f) {
    if (f < 0 || f >= XUI_F_COUNT) f = XUI_F_BODY;
    return xui_faces[f];
}

int xui_line_h(int f) {
    const xui_face_t *fc = xui_face(f);
    return (fc->ascent + fc->descent) * 115 / 100;
}

/* Fora do Latin-1: o que aparece em nome de jogo vira algo legível. */
static unsigned fold_cp(unsigned cp, const char **extra) {
    *extra = 0;
    if (cp < 0x100) return cp;
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2026: *extra = ".."; return '.';
    case 0x2122: return 0;   /* ™: o site mostra sobrescrito; "FCTM 25" ficava pior que nada */
    case 0x2022: case 0x00B7: return 0xB7;
    case 0x0152: *extra = "E"; return 'O';
    case 0x0153: *extra = "e"; return 'o';
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return ' ';
    default: return 0;   /* some */
    }
}

/* Estado do decodificador pra emitir as letras "extra" (TM, ...). */
typedef struct { const char *s; const char *extra; } u8it_t;

static unsigned it_next(u8it_t *it) {
    for (;;) {
        if (it->extra && *it->extra) return (unsigned char)*it->extra++;
        const unsigned char *p = (const unsigned char *)it->s;
        unsigned c = p[0], cp;
        int n;
        if (!c) return 0;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
        else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3;
        } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = 0x10000; n = 4;   /* fora do BMP: some */
        } else {
            /* byte solto: trata como Latin-1 (nome já em Latin-1 também funciona) */
            cp = c; n = 1;
        }
        it->s += n;
        if (cp < 0x20 && cp) continue;
        unsigned r = fold_cp(cp, &it->extra);
        if (r) return r;
    }
}

unsigned xui_utf8_next(const char **s) {
    u8it_t it = { *s, 0 };
    unsigned r = it_next(&it);
    *s = it.s;
    return r;
}

int xui_utf8_to_latin1(char *dst, int cap, const char *utf8) {
    u8it_t it = { utf8 ? utf8 : "", 0 };
    int n = 0;
    unsigned c;
    while ((c = it_next(&it)) && n + 1 < cap) dst[n++] = (char)c;
    if (cap > 0) dst[n] = 0;
    return n;
}

static int kern64(const xui_face_t *f, unsigned a, unsigned b) {
    if (!f->nkern || !a) return 0;
    unsigned key = (a << 8) | b;
    int lo = 0, hi = f->nkern - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        unsigned k = f->kern[m].pair;
        if (k == key) return f->kern[m].k64;
        if (k < key) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

static const xui_glyph_t *glyph(const xui_face_t *f, unsigned c) {
    int i = f->map[c & 0xff];
    if (i < 0) i = f->map['?'];
    if (i < 0) i = f->map[' '];
    return i < 0 ? 0 : &f->glyphs[i];
}

/* largura em 1/64 px de um trecho Latin-1 */
static long width64_l1(const xui_face_t *f, const unsigned char *s, int n, int letter) {
    long w = 0;
    unsigned prev = 0;
    for (int i = 0; i < n; i++) {
        const xui_glyph_t *g = glyph(f, s[i]);
        if (!g) continue;
        w += kern64(f, prev, s[i]) + g->adv64 + letter * 64;
        prev = s[i];
    }
    if (n > 0) w -= letter * 64;
    return w;
}

int xui_text_width(int fi, const char *utf8, int letter) {
    unsigned char buf[512];
    int n = xui_utf8_to_latin1((char *)buf, sizeof(buf), utf8 ? utf8 : "");
    return (int)((width64_l1(xui_face(fi), buf, n, letter) + 63) >> 6);
}

static void draw_glyph(xui_canvas_t *c, const xui_face_t *f, const xui_glyph_t *g, int x, int y, uint32_t col) {
    if (!g->w) return;
    xui_mask_t m = { f->bits + g->off, g->w, g->h };
    xui_mask(c, &m, x + g->bx, y + g->by, col, 255);
}

/* desenha n chars Latin-1; retorna pen final em 1/64 */
static long draw_l1(xui_canvas_t *c, const xui_face_t *f, long pen64, int y, uint32_t col,
                    const unsigned char *s, int n, int letter) {
    unsigned prev = 0;
    for (int i = 0; i < n; i++) {
        const xui_glyph_t *g = glyph(f, s[i]);
        if (!g) continue;
        pen64 += kern64(f, prev, s[i]);
        draw_glyph(c, f, g, (int)((pen64 + 32) >> 6), y, col);
        pen64 += g->adv64 + letter * 64;
        prev = s[i];
    }
    return pen64;
}

int xui_text(xui_canvas_t *c, int fi, int x, int y, uint32_t col, const char *utf8, int letter, int max_w) {
    const xui_face_t *f = xui_face(fi);
    unsigned char buf[512];
    int n = xui_utf8_to_latin1((char *)buf, sizeof(buf), utf8 ? utf8 : "");
    /* fora do clip na vertical: nem mede */
    if (y + f->descent < c->clip.y || y - f->ascent >= c->clip.y + c->clip.h) {
        return x + (int)(width64_l1(f, buf, n, letter) >> 6);
    }
    if (max_w > 0 && width64_l1(f, buf, n, letter) > (long)max_w * 64) {
        static const unsigned char ell[3] = { '.', '.', '.' };
        long ew = width64_l1(f, ell, 3, 0);
        while (n > 0 && width64_l1(f, buf, n, letter) + ew > (long)max_w * 64) n--;
        while (n > 0 && buf[n - 1] == ' ') n--;
        long pen = draw_l1(c, f, (long)x * 64, y, col, buf, n, letter);
        pen = draw_l1(c, f, pen, y, col, ell, 3, 0);
        return (int)(pen >> 6);
    }
    return (int)(draw_l1(c, f, (long)x * 64, y, col, buf, n, letter) >> 6);
}

void xui_text_center(xui_canvas_t *c, int fi, int x, int w, int y, uint32_t col, const char *utf8, int letter) {
    int tw = xui_text_width(fi, utf8, letter);
    xui_text(c, fi, x + (w - tw) / 2, y, col, utf8, letter, w);
}

int xui_text_wrap(xui_canvas_t *c, int fi, int x, int y, int w, int line_h, int max_lines,
                  int align, uint32_t col, const char *utf8) {
    const xui_face_t *f = xui_face(fi);
    unsigned char buf[512];
    int n = xui_utf8_to_latin1((char *)buf, sizeof(buf), utf8 ? utf8 : "");
    if (line_h <= 0) line_h = xui_line_h(fi);
    int lines = 0, i = 0;
    while (i < n && lines < max_lines) {
        while (i < n && buf[i] == ' ') i++;
        if (i >= n) break;
        /* maior trecho [i, j) que cabe, quebrando em espaço */
        int j = i, last_sp = -1;
        while (j < n && buf[j] != '\n') {
            if (buf[j] == ' ') last_sp = j;
            if (width64_l1(f, buf + i, j - i + 1, 0) > (long)w * 64) break;
            j++;
        }
        int end = j, next = j;
        if (j < n && buf[j] != '\n' && last_sp > i) { end = last_sp; next = last_sp + 1; }
        else if (j < n && buf[j] == '\n') next = j + 1;
        else if (j == i) { end = i + 1; next = end; }   /* nem 1 letra cabe */
        int lastline = (lines == max_lines - 1) && next < n;
        char tmp[512];
        int tn = end - i;
        if (lastline) tn = n - i;   /* deixa o xui_text pôr "..." no resto */
        memcpy(tmp, buf + i, (size_t)tn);
        tmp[tn] = 0;
        /* tmp está em Latin-1: bytes soltos >=0x80 passam como Latin-1 no
         * decodificador, desde que não formem UTF-8 válido por acaso —
         * reconverte pra UTF-8 pra não ter ambiguidade. */
        char u8[1024];
        int un = 0;
        for (int k = 0; k < tn && un < (int)sizeof(u8) - 3; k++) {
            unsigned char ch = (unsigned char)tmp[k];
            if (ch < 0x80) u8[un++] = (char)ch;
            else { u8[un++] = (char)(0xC0 | (ch >> 6)); u8[un++] = (char)(0x80 | (ch & 0x3F)); }
        }
        u8[un] = 0;
        int yy = y + lines * line_h;
        if (align == 1) {
            int tw = xui_text_width(fi, u8, 0);
            if (tw > w) tw = w;
            xui_text(c, fi, x + (w - tw) / 2, yy, col, u8, 0, w);
        } else {
            xui_text(c, fi, x, yy, col, u8, 0, w);
        }
        lines++;
        i = next;
    }
    return lines;
}
