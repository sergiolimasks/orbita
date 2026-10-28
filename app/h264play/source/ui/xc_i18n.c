/* XCloud360 v0.21.38 — idioma do aplicativo (ver xc_i18n.h). */
#include "xc_i18n.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const char *pt, *en, *es; } xi_ent_t;
static const xi_ent_t k_tab[] = {
#include "xc_i18n_tab.h"
};
#define XI_N (sizeof(k_tab) / sizeof(k_tab[0]))

volatile int xi_lang = XI_PT;

typedef struct { uint32_t h; uint16_t i; } xi_idx_t;
static xi_idx_t g_idx[XI_N];
static volatile int g_ready = 0;

static uint32_t hash(const char *s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}
static int cmp(const void *a, const void *b) {
    uint32_t x = ((const xi_idx_t *)a)->h, y = ((const xi_idx_t *)b)->h;
    return x < y ? -1 : x > y;
}

void xi_init(void) {
    for (unsigned i = 0; i < XI_N; i++) {
        g_idx[i].h = hash(k_tab[i].pt);
        g_idx[i].i = (uint16_t)i;
    }
    qsort(g_idx, XI_N, sizeof(g_idx[0]), cmp);
    __asm__ __volatile__("sync" ::: "memory");
    g_ready = 1;
}

const char *L(const char *pt) {
    int lang = xi_lang;
    if (lang == XI_PT || !pt || !g_ready) return pt;
    uint32_t h = hash(pt);
    int lo = 0, hi = (int)XI_N - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_idx[m].h < h) lo = m + 1;
        else if (g_idx[m].h > h) hi = m - 1;
        else {
            /* mesmo hash: confere o texto (vizinhos com o mesmo hash) */
            for (int k = m; k >= 0 && g_idx[k].h == h; k--)
                if (!strcmp(k_tab[g_idx[k].i].pt, pt)) return lang == XI_EN ? k_tab[g_idx[k].i].en : k_tab[g_idx[k].i].es;
            for (int k = m + 1; k < (int)XI_N && g_idx[k].h == h; k++)
                if (!strcmp(k_tab[g_idx[k].i].pt, pt)) return lang == XI_EN ? k_tab[g_idx[k].i].en : k_tab[g_idx[k].i].es;
            return pt;
        }
    }
    return pt;   /* sem tradução: fica em português */
}

const char *const XI_CODES[XI_NLANG] = { "pt", "en", "es" };
