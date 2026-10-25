/*
 * XCloud360 — desenho do menu direto no framebuffer do console (v0.16).
 * Ver menu_ui.h pro porquê.
 */
#ifdef XCLOUD360_MENU

#include "menu_ui.h"

#include <stdio.h>
#include <string.h>

#include <ppc/atomic.h>
#include <ppc/cache.h>

/* Fonte do console do libxenon (drivers/console/font_8x16.h, exportada). */
extern const unsigned char fontdata_8x16[];

#define UI_MAX_COLS 160

static uint32_t *g_fb = NULL;
static int       g_w, g_h, g_stride, g_hpad;
static int       g_dirty;

typedef struct {
    int      valid, x, y, scale, cols;
    uint32_t fg, bg;
    char     text[UI_MAX_COLS + 1];
} ui_cache_t;
static ui_cache_t g_cache[UI_SLOTS];

int ui_attach(void) {
    struct ati_info { uint32_t u1[4]; uint32_t base; uint32_t u2[8]; uint32_t width, height; } __attribute__((packed));
    volatile struct ati_info *ai = (struct ati_info *)0xec806100ULL;
    uint32_t base = ai->base, w = ai->width, h = ai->height;
    if (w == 0 || w > 4096 || h == 0 || h > 4096) { g_fb = NULL; return -1; }
    g_fb = (uint32_t *)(unsigned long)(base | 0x80000000);
    g_w = (int)w;
    g_h = (int)h;
    g_stride = (int)(((w + 31) >> 5) << 5);
    g_hpad = (int)(((h + 31) >> 5) << 5);
    ui_invalidate();
    return 0;
}

int ui_width(void)  { return g_w; }
int ui_height(void) { return g_h; }

/* Mesmo layout de tiles do console.c (blocos 32x32, X-swap em y&8). */
static inline void pset(int x, int y, uint32_t c) {
    if ((unsigned)x >= (unsigned)g_w || (unsigned)y >= (unsigned)g_h) return;
    uint32_t off = ((y >> 5) * 32 * g_stride + ((x >> 5) << 10)
                  + (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3)
                  + (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2);
    g_fb[off] = c;
}

void ui_clear(uint32_t bg) {
    if (!g_fb) return;
    /* Linear serve: limpa o fb inteiro (com o padding dos tiles). */
    size_t n = (size_t)g_stride * g_hpad;
    for (size_t i = 0; i < n; i++) g_fb[i] = bg;
    g_dirty = 1;
    ui_invalidate();
}

void ui_invalidate(void) {
    for (int i = 0; i < UI_SLOTS; i++) g_cache[i].valid = 0;
}

void ui_fill(int x, int y, int w, int h, uint32_t color) {
    if (!g_fb) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) pset(xx, yy, color);
    g_dirty = 1;
}

static void draw_char(int x, int y, int s, uint32_t fg, uint32_t bg, unsigned char c) {
    const unsigned char *gl = fontdata_8x16 + (unsigned)c * 16;
    for (int gy = 0; gy < 16; gy++) {
        unsigned bits = gl[gy];
        for (int sy = 0; sy < s; sy++) {
            int py = y + gy * s + sy;
            for (int gx = 0; gx < 8; gx++) {
                uint32_t col = ((bits >> (7 - gx)) & 1) ? fg : bg;
                for (int sx = 0; sx < s; sx++) pset(x + gx * s + sx, py, col);
            }
        }
    }
}

void ui_line(int slot, int x, int y, int scale, uint32_t fg, uint32_t bg,
             int cols, const char *text) {
    if (!g_fb) return;
    if (scale < 1) scale = 1;
    if (cols > UI_MAX_COLS) cols = UI_MAX_COLS;
    char buf[UI_MAX_COLS + 1];
    int i = 0;
    for (; i < cols && text && text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        buf[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    for (; i < cols; i++) buf[i] = ' ';
    buf[cols] = '\0';
    if (slot >= 0 && slot < UI_SLOTS) {
        ui_cache_t *k = &g_cache[slot];
        if (k->valid && k->x == x && k->y == y && k->scale == scale && k->cols == cols &&
            k->fg == fg && k->bg == bg && strcmp(k->text, buf) == 0)
            return;   /* igual ao que já está na tela */
        k->valid = 1; k->x = x; k->y = y; k->scale = scale; k->cols = cols;
        k->fg = fg; k->bg = bg;
        memcpy(k->text, buf, (size_t)cols + 1);
    }
    for (i = 0; i < cols; i++) draw_char(x + i * 8 * scale, y, scale, fg, bg, (unsigned char)buf[i]);
    g_dirty = 1;
}

void ui_flush(void) {
    if (!g_fb || !g_dirty) return;
    /* O console faz isso por caractere; aqui uma vez por quadro do menu. No
     * alias não-cacheado é praticamente no-op, mas não custa garantir. */
    memdcbst(g_fb, g_stride * g_hpad * 4);
    g_dirty = 0;
}

/* ---- log do stdout ---- */

static unsigned int      g_log_lock = 0;
static char              g_log[UI_LOG_LINES][UI_LOG_COLS];
static int               g_log_head = 0;      /* linha sendo escrita */
static int               g_log_len = 0;
static volatile unsigned g_log_seq = 0;

static void ui_log_hook(const char *buf, int len) {
    lock(&g_log_lock);
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') {
            if (g_log_len == 0) continue;    /* linhas vazias não gastam espaço */
            g_log_head = (g_log_head + 1) % UI_LOG_LINES;
            g_log[g_log_head][0] = '\0';
            g_log_len = 0;
        } else if (c >= 32 && c < 127 && g_log_len < UI_LOG_COLS - 1) {
            g_log[g_log_head][g_log_len++] = c;
            g_log[g_log_head][g_log_len] = '\0';
        }
    }
    g_log_seq++;
    unlock(&g_log_lock);
}

extern void (*stdout_hook)(const char *buf, int len);

void ui_log_install(void) {
    stdout_hook = ui_log_hook;
}

unsigned ui_log_get(char out[UI_LOG_LINES][UI_LOG_COLS]) {
    lock(&g_log_lock);
    for (int i = 0; i < UI_LOG_LINES; i++) {
        int k = (g_log_head + 1 + i) % UI_LOG_LINES;   /* mais antiga primeiro */
        memcpy(out[i], g_log[k], UI_LOG_COLS);
    }
    unsigned s = g_log_seq;
    unlock(&g_log_lock);
    return s;
}

#else
typedef int xcloud360_menu_ui_disabled;   /* MENU=0: unidade vazia */
#endif /* XCLOUD360_MENU */
