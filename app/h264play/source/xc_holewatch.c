/* XCloud360 v0.21.45 — ver xc_holewatch.h. */
#include "xc_holewatch.h"
#include <stdio.h>
#include <string.h>
#include <ppc/timebase.h>

#define XHW_STEP   16384u   /* bytes vigiados por passo */
#define XHW_LINE   128u

extern char orbita_hole_start[], orbita_hole_end[];

static uint32_t g_s, g_e;
static uint32_t g_next;
static uint64_t g_tb0;

/* o que tinha no buraco ao entrar no main (antes de qualquer driver nosso) */
static xhw_scan_t g_early;
/* gravações depois disso (cada uma é zerada de novo) */
static volatile unsigned g_hits = 0, g_reported = 0;
static volatile uint32_t g_lo = 0xffffffffu, g_hi = 0;
static xhw_ev_t g_ev[XHW_NEV];

static void flush(uint32_t a, uint32_t n) {
    for (uint32_t p = a & ~(XHW_LINE - 1); p < a + n; p += XHW_LINE) __asm__ __volatile__("dcbst 0,%0" ::"r"(p) : "memory");
    __asm__ __volatile__("sync" ::: "memory");
}

/* conta palavras != 0 em [a, a+n), anota a faixa e zera */
static void scan_zero(uint32_t a, uint32_t n, xhw_scan_t *o) {
    volatile uint32_t *w = (volatile uint32_t *)a;
    for (uint32_t k = 0; k < n / 4; k++) {
        uint32_t v = w[k];
        if (!v) continue;
        uint32_t ad = a + k * 4;
        if (!o->words) o->first = ad;
        if (o->words < 4) o->sample[o->words] = v;
        o->last = ad;
        o->words++;
        o->blocks |= 1ull << (((ad - g_s) >> 16) & 63);   /* blocos de 64 KB */
        w[k] = 0;
    }
    flush(a, n);
}

void xhw_early(void) {
    g_s = ((uint32_t)orbita_hole_start + 3u) & ~3u;
    g_e = (uint32_t)orbita_hole_end & ~3u;
    g_tb0 = mftb();
    if (g_e <= g_s) { g_s = g_e = 0; return; }
    memset(&g_early, 0, sizeof(g_early));
    scan_zero(g_s, g_e - g_s, &g_early);
    g_next = g_s;
}

const xhw_scan_t *xhw_early_result(void) { return &g_early; }
uint32_t xhw_start(void) { return g_s; }
uint32_t xhw_end(void) { return g_e; }

void xhw_step(void) {
    if (!g_s) return;
    uint32_t a = g_next, n = g_e - a < XHW_STEP ? g_e - a : XHW_STEP;
    volatile uint32_t *w = (volatile uint32_t *)a;
    for (uint32_t k = 0; k < n / 4; k++) {
        uint32_t v = w[k];
        if (!v) continue;
        uint32_t ad = a + k * 4;
        unsigned h = g_hits;
        if (h < XHW_NEV) {
            g_ev[h].addr = ad;
            g_ev[h].word = v;
            g_ev[h].ms = (uint32_t)((mftb() - g_tb0) / (PPC_TIMEBASE_FREQ / 1000));
        }
        if (ad < g_lo) g_lo = ad;
        if (ad > g_hi) g_hi = ad;
        g_hits = h + 1;
        w[k] = 0;
    }
    if (n) flush(a, n);
    g_next = a + n >= g_e ? g_s : a + n;
}

int xhw_line(char *o, size_t n) {
    unsigned h = g_hits, r = g_reported;
    if (r >= h) return 0;
    if (r < XHW_NEV) {
        xhw_ev_t e = g_ev[r];
        snprintf(o, n, "[buraco] gravação em %08x: %08x (t=%u ms; %u até agora, faixa %08x-%08x)", (unsigned)e.addr,
                 (unsigned)e.word, (unsigned)e.ms, h, (unsigned)g_lo, (unsigned)g_hi);
        g_reported = r + 1;
    } else {
        snprintf(o, n, "[buraco] mais %u gravações (total %u, faixa %08x-%08x)", h - r, h, (unsigned)g_lo, (unsigned)g_hi);
        g_reported = h;
    }
    return 1;
}

unsigned xhw_hits(void) { return g_hits; }
