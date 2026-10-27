/* orbita v0.21.14 — locks instrumentados. Ver xc_lock.h. */
#include "xc_lock.h"

#include <stdio.h>
#include <string.h>

#ifdef XENON
#include <ppc/timebase.h>
int xlk_pir(void) {
    unsigned v;
    __asm__ __volatile__("mfspr %0, 1023" : "=r"(v));
    return (int)v;
}
uint64_t xlk_now_ms(void) { return mftb() / (PPC_TIMEBASE_FREQ / 1000ULL); }
static inline void relax(void) { __asm__ __volatile__("db16cyc" ::: "memory"); }
#else
#include <sched.h>
#include <time.h>
/* testes do host: cada thread diz quem é (xlk_test_pir) */
__thread int xlk_test_pir = 0;
int xlk_pir(void) { return xlk_test_pir; }
uint64_t xlk_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}
static inline void relax(void) { sched_yield(); }
#endif

/* os campos de diagnóstico são lidos por outras threads (relatório): atômicos relaxados */
#define ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define LD(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)

#define NPIR   8
#define NLOCK  48
#define NHELD  8

static xlk_t *volatile   g_reg[NLOCK];
static volatile int      g_nreg = 0;
static unsigned char     g_edge[NLOCK][NLOCK];      /* [segurando][pegou] já visto */
static volatile unsigned g_inv = 0, g_tryfail = 0;
static const char       *g_inv_a = NULL, *g_inv_b = NULL;
static const char       *g_inv_site = NULL;

static xlk_t *volatile      g_wait[NPIR];
static const char *volatile g_wait_site[NPIR];
static volatile uint64_t    g_wait_t[NPIR];
static xlk_t               *g_held[NPIR][NHELD];
static int                  g_nheld[NPIR];

static int reg(xlk_t *l) {
    int id = LD(l->id);
    if (id) return id;
    int k = __sync_fetch_and_add(&g_nreg, 1);
    if (k >= NLOCK - 1) return 0;
    ST(g_reg[k + 1], l);
    ST(l->id, k + 1);
    return k + 1;
}

static int pir_ok(int p) { return p >= 0 && p < NPIR; }

static void took(xlk_t *l, const char *site, int p) {
    if (!LD(l->name)) ST(l->name, site);
    ST(l->owner, p);
    ST(l->site, site);
    ST(l->t_ms, xlk_now_ms());
    if (!pir_ok(p)) return;
    int id = reg(l);
    /* ordem: tudo o que esta thread já segura vem antes deste */
    for (int k = 0; k < g_nheld[p]; k++) {
        xlk_t *h = g_held[p][k];
        int hid = LD(h->id);
        if (!hid || !id || hid == id) continue;
        ST(g_edge[hid][id], 1);
        if (LD(g_edge[id][hid])) {
            if (__sync_fetch_and_add(&g_inv, 1) == 0) { ST(g_inv_a, h->name); ST(g_inv_b, l->name); ST(g_inv_site, site); }
        }
    }
    if (g_nheld[p] < NHELD) g_held[p][g_nheld[p]++] = l;
}

void xlk_lock(xlk_t *l, const char *site) {
    int p = xlk_pir();
    if (!__sync_lock_test_and_set(&l->w, 1)) { took(l, site, p); return; }
    if (pir_ok(p)) { ST(g_wait_site[p], site); ST(g_wait_t[p], xlk_now_ms()); ST(g_wait[p], l); }
    while (__sync_lock_test_and_set(&l->w, 1)) {
        while (LD(l->w)) relax();
    }
    if (pir_ok(p)) ST(g_wait[p], (xlk_t *)NULL);
    took(l, site, p);
}

int xlk_try(xlk_t *l, const char *site, int spins) {
    int p = xlk_pir();
    for (int i = 0; i <= spins; i++) {
        if (!__sync_lock_test_and_set(&l->w, 1)) { took(l, site, p); return 1; }
        relax();
    }
    __sync_fetch_and_add(&g_tryfail, 1);
    return 0;
}

void xlk_unlock(xlk_t *l) {
    int p = xlk_pir();
    if (pir_ok(p)) {
        int n = g_nheld[p];
        for (int k = n - 1; k >= 0; k--)
            if (g_held[p][k] == l) {
                for (int j = k; j < n - 1; j++) g_held[p][j] = g_held[p][j + 1];
                g_nheld[p] = n - 1;
                break;
            }
    }
    ST(l->owner, -1);
    __sync_lock_release(&l->w);
}

void xlk_set_name(xlk_t *l, const char *name) { l->name = name; }

void xlk_waiting_text(int p, char *o, size_t n) {
    if (!pir_ok(p)) { snprintf(o, n, "?"); return; }
    xlk_t *l = LD(g_wait[p]);
    if (!l) { snprintf(o, n, "hw%d não espera nenhum lock do app", p); return; }
    uint64_t now = xlk_now_ms(), tw = LD(g_wait_t[p]), th = LD(l->t_ms);
    int ow = LD(l->owner);
    const char *nm = LD(l->name), *ws = LD(g_wait_site[p]), *hs = LD(l->site);
    snprintf(o, n, "hw%d espera \"%s\" há %u ms (pediu em %s); dono hw%d desde há %u ms (pegou em %s)", p,
             nm ? nm : "?", (unsigned)(now > tw ? now - tw : 0), ws ? ws : "?", ow,
             (unsigned)(now > th ? now - th : 0), hs ? hs : "?");
}

void xlk_held_text(char *o, size_t n, unsigned min_ms) {
    size_t w = 0;
    o[0] = 0;
    uint64_t now = xlk_now_ms();
    int nr = LD(g_nreg) < NLOCK - 1 ? LD(g_nreg) : NLOCK - 1;
    for (int i = 1; i <= nr && w + 8 < n; i++) {
        xlk_t *l = LD(g_reg[i]);
        if (!l || !LD(l->w)) continue;
        uint64_t t = LD(l->t_ms);
        unsigned age = (unsigned)(now > t ? now - t : 0);
        if (age < min_ms) continue;
        const char *nm = LD(l->name), *st = LD(l->site);
        int r = snprintf(o + w, n - w, "%s\"%s\" com hw%d há %u ms (%s)", w ? "; " : "", nm ? nm : "?", LD(l->owner), age,
                         st ? st : "?");
        if (r < 0) break;
        w += (size_t)r;
    }
    if (!w) snprintf(o, n, "nenhum lock do app seguro há %u ms ou mais", min_ms);
}

unsigned xlk_inversions(void) { return g_inv; }
const char *xlk_inversion_text(void) {
    static char t[200];
    if (!g_inv) return "nenhuma";
    snprintf(t, sizeof(t), "%u (a 1ª: \"%s\" -> \"%s\" em %s)", g_inv, g_inv_a ? g_inv_a : "?", g_inv_b ? g_inv_b : "?",
             g_inv_site ? g_inv_site : "?");
    return t;
}
unsigned xlk_try_fails(void) { return g_tryfail; }
