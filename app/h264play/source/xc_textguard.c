/* XCloud360 v0.21.37 — ver xc_textguard.h. */
#include "xc_textguard.h"
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ppc/timebase.h>

#define XTG_BLK     65536u
#define XTG_STEP_MS 20u   /* 1 bloco a cada 20 ms: a volta toda (~57 blocos) em ~1,2 s */
#define XTG_LINE    128u  /* linha de cache do Xenon */

extern char pagetable_end[], orbita_text_end[];
/* dados que o libxenon grava dentro do .text (startup_from_xell.S): a tabela
 * "wait" (entrada de cada hw thread, 6 x 8 B) e o __argv do _start. Ficam de fora. */
extern char xtg_wait_tbl[] __asm__("wait");
extern char _start[];
static int skip(uint32_t a) {
    uint32_t w = (uint32_t)xtg_wait_tbl, s = (uint32_t)_start;
    return (a >= w && a < w + 48) || (a >= s + 8 && a < s + 20);
}

volatile unsigned xtg_bad = 0, xtg_fixed = 0, xtg_sweeps = 0;
volatile uint32_t xtg_last_addr = 0, xtg_last_now = 0, xtg_last_want = 0, xtg_last_words = 0;

static uint32_t g_base = 0, g_len = 0, g_n = 0;
static uint32_t *g_sum = NULL;
static uint8_t  *g_copy = NULL;
static volatile unsigned g_next = 0;
static volatile int g_busy = 0;
static volatile uint64_t g_last_tb = 0;

static uint32_t blk_len(unsigned i) { return i + 1 < g_n ? XTG_BLK : g_len - i * XTG_BLK; }

static uint32_t sum_words(const volatile uint32_t *p, uint32_t bytes) {
    uint32_t a = 0x811c9dc5u, b = 0;
    for (uint32_t i = 0; i < bytes / 4; i++) {
        uint32_t w = p[i];
        a = (a ^ w) * 16777619u;
        b += w ^ i;
    }
    return a ^ (b * 0x9e3779b1u);
}

static void sync_icache(uint32_t a, uint32_t n) {
    uint32_t e = a + n;
    for (uint32_t p = a & ~(XTG_LINE - 1); p < e; p += XTG_LINE) __asm__ __volatile__("dcbst 0,%0" ::"r"(p) : "memory");
    __asm__ __volatile__("sync" ::: "memory");
    for (uint32_t p = a & ~(XTG_LINE - 1); p < e; p += XTG_LINE) __asm__ __volatile__("icbi 0,%0" ::"r"(p) : "memory");
    __asm__ __volatile__("sync; isync" ::: "memory");
}

int xtg_init(void) {
    /* o código do app começa depois do vão de 0x3e000 do orbita.lds (os
     * vetores e a tabela de páginas do libxenon ficam de fora) */
    uint32_t b = ((uint32_t)pagetable_end + 0x3e000u + XTG_BLK - 1) & ~(XTG_BLK - 1);
    uint32_t e = (uint32_t)orbita_text_end & ~3u;
    if (e <= b) return -1;
    g_len = e - b;
    g_n = (g_len + XTG_BLK - 1) / XTG_BLK;
    g_copy = (uint8_t *)memalign(XTG_LINE, g_len);
    g_sum = (uint32_t *)malloc(g_n * sizeof(uint32_t));
    if (!g_copy || !g_sum) {
        free(g_copy);
        free(g_sum);
        g_copy = NULL;
        g_sum = NULL;
        return -1;
    }
    memcpy(g_copy, (const void *)b, g_len);
    for (unsigned i = 0; i < g_n; i++) g_sum[i] = sum_words((const uint32_t *)(g_copy + i * XTG_BLK), blk_len(i));
    __asm__ __volatile__("sync" ::: "memory");
    g_base = b;
    return 0;
}

void xtg_step(void) {
    if (!g_base) return;
    uint64_t now = mftb();
    if (now - g_last_tb < (uint64_t)XTG_STEP_MS * (PPC_TIMEBASE_FREQ / 1000)) return;
    if (__sync_lock_test_and_set(&g_busy, 1)) return;
    g_last_tb = now;
    unsigned i = g_next;
    g_next = i + 1 < g_n ? i + 1 : 0;
    if (!g_next) xtg_sweeps++;
    uint32_t n = blk_len(i), a = g_base + i * XTG_BLK;
    const volatile uint32_t *live = (const volatile uint32_t *)a;
    if (sum_words(live, n) != g_sum[i]) {
        const uint32_t *ref = (const uint32_t *)(g_copy + i * XTG_BLK);
        /* a cópia também pode ter estragado: só restaura se ela bate com a soma do boot */
        int copy_ok = sum_words(ref, n) == g_sum[i];
        uint32_t first = 0, last = 0, words = 0;
        volatile uint32_t *w = (volatile uint32_t *)a;
        for (uint32_t k = 0; k < n / 4; k++) {
            if (live[k] == ref[k] || skip(a + k * 4)) continue;
            if (!words) {
                first = k;
                xtg_last_now = live[k];
                xtg_last_want = ref[k];
            }
            last = k;
            words++;
            if (copy_ok) w[k] = ref[k];   /* só o que mudou */
        }
        if (words) {
            xtg_last_addr = a + first * 4;
            xtg_last_words = words;
            if (copy_ok) {
                sync_icache(a + first * 4, (last - first + 1) * 4);
                xtg_fixed++;
            }
            __asm__ __volatile__("sync" ::: "memory");
            xtg_bad++;
        }
    }
    __sync_lock_release(&g_busy);
}

int xtg_check_addr(uint32_t a) {
    if (!g_base || a < g_base || a + 4 > g_base + g_len || skip(a)) return 0;
    return *(const volatile uint32_t *)a != *(const uint32_t *)(g_copy + (a - g_base));
}
