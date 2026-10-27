/* orbita v0.21.7 — vigia de travamento por thread. Ver xc_wdog.h. */
#include "xc_wdog.h"

#include <stdio.h>
#include <string.h>

static const char *const k_name[XWD_N] = { "main (hw0)", "rede (hw1)", "tarefa (hw3)" };

typedef struct {
    volatile uint64_t t;          /* ms do último batimento (0 = nunca) */
    const char *volatile label;
    char     arg[2][40];          /* complemento copiado (dois buffers: escrita sem rasgar a leitura) */
    volatile int argi;            /* qual arg vale (-1 = nenhum) */
    const char *volatile crumb;   /* v0.21.9: sub-passo */
    int      stalled;             /* já reportado nesta parada */
    uint64_t stall_t;
} xwd_slot_t;

static xwd_slot_t   g_s[XWD_N];
static volatile int g_hw3_on = 0;
static volatile int g_usb_holder = -1;
static int          g_both = 0;           /* v0.21.8: main+rede paradas já reportadas */
static uint64_t     g_both_t = 0;
static uint64_t     g_obs_last = 0;

#ifdef XWD_HOSTTEST
__attribute__((weak)) void xwd_trip_report(const char *line, int slot) { (void)line; (void)slot; }
int xwd_test_pir = 0;
static int pir(void) { return xwd_test_pir; }
/* outros testes do host (catálogo) linkam sem plataforma: padrões fracos */
__attribute__((weak)) uint64_t xwd_now_ms(void) { return 0; }
__attribute__((weak)) void xwd_report(const char *line, int raw_ok) { (void)line; (void)raw_ok; }
#else
static int pir(void) {
    unsigned v;
    __asm__ __volatile__("mfspr %0, 1023" : "=r"(v));
    return (int)v;
}
#endif

int xwd_slot_of_pir(void) {
    int p = pir();
    return p == 0 ? XWD_MAIN : p == 1 ? XWD_NET : p == 3 ? XWD_HW3 : -1;
}

void xwd_beat_slot(int slot, const char *label) {
    if (slot < 0 || slot >= XWD_N) return;
    xwd_slot_t *s = &g_s[slot];
    s->label = label;
    s->argi = -1;
    s->crumb = NULL;
    s->t = xwd_now_ms();
}

void xwd_crumb(const char *step) {
    int slot = xwd_slot_of_pir();
    if (slot < 0) return;
    g_s[slot].crumb = step;
}

/* v0.21.9: quando cada um conferiu pela última vez (se quem confere também
 * ficou parado, o aviso diz: é a pista de travamento do núcleo inteiro) */
static uint64_t g_last_check[XWD_N];

void xwd_beat(const char *label) { xwd_beat_slot(xwd_slot_of_pir(), label); }

void xwd_beat2(const char *label, const char *arg) {
    int slot = xwd_slot_of_pir();
    if (slot < 0) return;
    xwd_slot_t *s = &g_s[slot];
    int k = s->argi == 0 ? 1 : 0;
    snprintf(s->arg[k], sizeof(s->arg[k]), "%s", arg ? arg : "");
    s->label = label;
    s->argi = k;
    s->t = xwd_now_ms();
}

void xwd_set_hw3_active(int on) {
    if (on) g_s[XWD_HW3].t = xwd_now_ms();
    g_hw3_on = on;
}

static volatile uint64_t g_usb_hold_t = 0;   /* v0.21.12: desde quando alguém segura o pendrive */
void xwd_set_usb_holder(int slot) {
    g_usb_hold_t = slot >= 0 ? xwd_now_ms() : 0;
    g_usb_holder = slot;
}
int  xwd_usb_holder(void) { return g_usb_holder; }

uint64_t xwd_age_ms(int slot) {
    if (slot < 0 || slot >= XWD_N || !g_s[slot].t) return 0;
    uint64_t now = xwd_now_ms(), t = g_s[slot].t;
    return now > t ? now - t : 0;
}

static void describe(int slot, char *o, size_t n) {
    const xwd_slot_t *s = &g_s[slot];
    const char *l = s->label ? s->label : "?";
    int ai = s->argi;
    const char *cr = s->crumb;
    if (ai == 0 || ai == 1) snprintf(o, n, "%s %s%s%s", l, s->arg[ai], cr ? " / passo " : "", cr ? cr : "");
    else snprintf(o, n, "%s%s%s", l, cr ? " / passo " : "", cr ? cr : "");
}

void xwd_check(int self, unsigned mask) {
    uint64_t now = xwd_now_ms();
    uint64_t prev = self >= 0 && self < XWD_N ? g_last_check[self] : 0;
    if (self >= 0 && self < XWD_N) g_last_check[self] = now;
    unsigned self_gap = prev && now > prev ? (unsigned)(now - prev) : 0;
    for (int i = 0; i < XWD_N; i++) {
        if (i == self || !(mask & (1u << i))) continue;
        xwd_slot_t *s = &g_s[i];
        if (!s->t) continue;                       /* nunca bateu: ainda não começou */
        if (i == XWD_HW3 && !g_hw3_on) { s->stalled = 0; continue; }
        uint64_t age = now > s->t ? now - s->t : 0;
        char what[160], line[320];
        if (age > XWD_STALL_MS) {
            int holds = g_usb_holder == i;
            /* 1º aviso aos 5 s; de novo aos 15 s (aí a escrita direta vale mesmo
             * se esta thread segura o pendrive: ela não está mais andando) */
            int lvl = age > 15000 ? 2 : 1;
            if (s->stalled < lvl) {
                s->stalled = lvl;
                if (lvl == 1) s->stall_t = s->t;
                describe(i, what, sizeof(what));
                snprintf(line, sizeof(line), "[wdog] %s parada há %u s em \"%s\" (USB do pendrive com: %s; visto por %s, "
                         "que não conferia havia %u ms)",
                         k_name[i], (unsigned)(age / 1000), what,
                         g_usb_holder >= 0 ? k_name[g_usb_holder] : "ninguém", self >= 0 && self < XWD_N ? k_name[self] : "?",
                         self_gap);
                /* escrita direta só sem ninguém no pendrive, ou se quem segura é
                 * a própria thread parada há 15 s */
                xwd_report(line, g_usb_holder < 0 || (holds && lvl == 2));
            }
        } else if (s->stalled) {
            describe(i, what, sizeof(what));
            snprintf(line, sizeof(line), "[wdog] %s voltou depois de %u s (agora em \"%s\")", k_name[i],
                     (unsigned)((now - s->stall_t) / 1000), what);
            s->stalled = 0;
            xwd_report(line, g_usb_holder < 0);
        }
    }
}

/* v0.21.8: main (hw0) e rede (hw1) paradas juntas. Cada uma vigia a outra,
 * então esse caso só um terceiro vê. A linha vai primeiro pro HANG.TXT (a
 * plataforma escreve o setor antes do log, que pode depender de quem parou). */
void xwd_check_both(int self) {
    const xwd_slot_t *m = &g_s[XWD_MAIN], *n = &g_s[XWD_NET];
    if (!m->t || !n->t) return;
    uint64_t now = xwd_now_ms();
    uint64_t am = now > m->t ? now - m->t : 0, an = now > n->t ? now - n->t : 0;
    char wm[96], wn[96], line[420];
    if (am > XWD_STALL_MS && an > XWD_STALL_MS) {
        uint64_t a = am < an ? am : an;
        int lvl = a > 15000 ? 2 : 1;
        if (g_both >= lvl) return;
        g_both = lvl;
        if (lvl == 1) g_both_t = now - a;
        describe(XWD_MAIN, wm, sizeof(wm));
        describe(XWD_NET, wn, sizeof(wn));
        int h = g_usb_holder;
        snprintf(line, sizeof(line), "[wdog] main (hw0) e rede (hw1) paradas: main há %u s em \"%s\", rede há %u s em \"%s\" "
                 "(USB do pendrive com: %s; visto por %s)", (unsigned)(am / 1000), wm, (unsigned)(an / 1000), wn,
                 h >= 0 && h < XWD_N ? k_name[h] : "ninguém", self >= 0 && self < XWD_N ? k_name[self] : "?");
        /* escrita direta: pendrive livre, ou quem segura é uma das paradas
         * há 15 s (não está mais no meio de uma transferência que ande) */
        xwd_report(line, h < 0 || ((h == XWD_MAIN || h == XWD_NET) && lvl == 2));
    } else if (g_both) {
        g_both = 0;
        snprintf(line, sizeof(line), "[wdog] main e rede voltaram depois de %u s", (unsigned)((now - g_both_t) / 1000));
        xwd_report(line, g_usb_holder < 0);
    }
}

void xwd_observe(void) {
    if (xwd_slot_of_pir() != XWD_HW3 || !g_hw3_on) return;
    uint64_t now = xwd_now_ms();
    if (now - g_obs_last < 500) return;
    g_obs_last = now;
    xwd_check_both(XWD_HW3);
}

/* ------------------------------------------------ v0.21.12: tripwire */
static volatile int g_trip_busy = 0;
static uint64_t     g_trip_last = 0;
static int          g_trip_done[XWD_N];

const char *xwd_label(int slot) { return slot >= 0 && slot < XWD_N ? g_s[slot].label : NULL; }
const char *xwd_crumb_of(int slot) { return slot >= 0 && slot < XWD_N ? g_s[slot].crumb : NULL; }

void xwd_tripwire(void) {
    if (__sync_lock_test_and_set(&g_trip_busy, 1)) return;
    uint64_t now = xwd_now_ms();
    if (now - g_trip_last < 250) { __sync_lock_release(&g_trip_busy); return; }
    g_trip_last = now;
    static const int slots[2] = { XWD_MAIN, XWD_NET };
    for (int k = 0; k < 2; k++) {
        int i = slots[k];
        xwd_slot_t *s = &g_s[i];
        if (!s->t) continue;
        uint64_t age = now > s->t ? now - s->t : 0;
        if (age >= 8000 && !g_trip_done[i]) {
            g_trip_done[i] = 1;
            char what[160], line[300];
            describe(i, what, sizeof(what));
            uint64_t other = g_s[i == XWD_MAIN ? XWD_NET : XWD_MAIN].t;
            unsigned oage = other && now > other ? (unsigned)(now - other) : 0;
            snprintf(line, sizeof(line), "[wdog] TRAVOU: %s parada há %u s em \"%s\"; %s bateu há %u ms (tripwire na hw%d)",
                     k_name[i], (unsigned)(age / 1000), what, k_name[i == XWD_MAIN ? XWD_NET : XWD_MAIN], oage, pir());
            xwd_trip_report(line, i);
        } else if (age < 2000) {
            g_trip_done[i] = 0;
        }
    }
    /* o pendrive preso por alguém há 8 s: sem NETLOG nem HANG.TXT (os dois
     * esperam o USB livre) — pelo menos a tela e a UART dizem quem */
    {
        static int usb_rep = 0;
        int h = g_usb_holder;
        uint64_t t = g_usb_hold_t;
        if (h >= 0 && t && now > t && now - t >= 8000) {
            if (!usb_rep) {
                usb_rep = 1;
                char what[160], line[240];
                describe(h, what, sizeof(what));
                snprintf(line, sizeof(line), "[wdog] TRAVOU: o USB do pendrive está com %s há %u s (em \"%s\")",
                         h < XWD_N ? k_name[h] : "?", (unsigned)((now - t) / 1000), what);
                xwd_trip_report(line, h);
            }
        } else {
            usb_rep = 0;
        }
    }
    __sync_lock_release(&g_trip_busy);
}
