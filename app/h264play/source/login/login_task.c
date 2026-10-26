/* XCloud360 v0.17 — ver login_task.h. */
#include "login_task.h"
#include "login_flow.h"
#include "xc_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppc/atomic.h>
#include <xenon_soc/xenon_power.h>

static uint8_t g_stack[256 * 1024] __attribute__((aligned(128)));

static unsigned int  g_lock = 0;
static lt_snap_t     g_st;
static xa_title_t   *g_games = NULL;
static int           g_ngames = 0;
static volatile int  g_running = 0;
static volatile int  g_cancel = 0;

static inline void lwsync(void) { __asm__ __volatile__("lwsync" ::: "memory"); }

/* Gancho extra (xc_catalog: parar a thread das capas no meio de um pedido).
 * Só é consultado; quem registra garante que ele volta 0 fora da sua thread. */
static int (*volatile g_cancel_hook)(void) = 0;
void xcn_set_cancel_hook(int (*fn)(void)) { g_cancel_hook = fn; lwsync(); }

int xcn_cancelled(void) {
    int (*h)(void) = g_cancel_hook;
    return g_cancel || (h && h());
}

static void ui_status(const char *t) {
    lock(&g_lock);
    snprintf(g_st.status, sizeof(g_st.status), "%s", t);
    g_st.seq++;
    unlock(&g_lock);
}

static void ui_code(const char *uri, const char *code) {
    lock(&g_lock);
    if (uri) {
        /* "https://www.microsoft.com/link" -> "microsoft.com/link" (cabe na TV) */
        const char *u = strstr(uri, "://");
        u = u ? u + 3 : uri;
        if (!strncmp(u, "www.", 4)) u += 4;
        snprintf(g_st.uri, sizeof(g_st.uri), "%s", u);
        snprintf(g_st.code, sizeof(g_st.code), "%s", code ? code : "");
    } else {
        g_st.uri[0] = g_st.code[0] = 0;
    }
    g_st.seq++;
    unlock(&g_lock);
}

static void task_main(void) {
    static int rtc_logged = 0;
    if (!rtc_logged) { rtc_logged = 1; xc_time_log_rtc(); }
    lf_ui_t ui = { ui_status, ui_code };
    xa_title_t *games = NULL;
    int n = 0;
    xc_err_t e;
    uint64_t t0 = xcn_now_ms();
    int r = lf_run(&ui, &games, &n, &e);
    lock(&g_lock);
    g_st.uri[0] = g_st.code[0] = 0;
    if (r == 0) {
        free(g_games);
        g_games = games;
        g_ngames = n;
        g_st.ngames = n;
        g_st.phase = LT_DONE;
        snprintf(g_st.status, sizeof(g_st.status), "catalogo: Xbox (%d jogos)", n);
        g_st.err[0] = 0;
    } else {
        free(games);
        g_st.phase = LT_FAILED;
        snprintf(g_st.err, sizeof(g_st.err), "%s", e.msg[0] ? e.msg : "erro desconhecido");
    }
    g_st.runs++;
    g_st.seq++;
    unlock(&g_lock);
    if (r == 0) xc_log("[login] pronto: %d jogos em %us", n, (unsigned)((xcn_now_ms() - t0) / 1000));
    else xc_log("[login] FALHOU: %s", e.msg);
    lwsync();
    g_running = 0;
    /* retornar libera a hw thread (o startup do libxenon zera o slot) */
}

int lt_start(void) {
    if (g_running || xenon_is_thread_task_running(LT_HW_THREAD)) return 0;
    lock(&g_lock);
    g_st.phase = LT_RUNNING;
    g_st.err[0] = 0;
    g_st.uri[0] = g_st.code[0] = 0;
    snprintf(g_st.status, sizeof(g_st.status), "Iniciando o login do Xbox...");
    g_st.seq++;
    unlock(&g_lock);
    g_cancel = 0;
    g_running = 1;
    lwsync();
    int r = xenon_run_thread_task(LT_HW_THREAD, g_stack + sizeof(g_stack) - 0x100, (void *)task_main);
    if (r != 0) {
        g_running = 0;
        lock(&g_lock);
        g_st.phase = LT_FAILED;
        snprintf(g_st.err, sizeof(g_st.err), "nao consegui subir a thread do login (%d)", r);
        g_st.runs++;
        g_st.seq++;
        unlock(&g_lock);
        return -1;
    }
    return 0;
}

int  lt_running(void) { return g_running || xenon_is_thread_task_running(LT_HW_THREAD); }
/* v0.20.2: só o login (lt_running também é verdade com o worker das capas na hw 3). */
int  lt_login_running(void) { return g_running; }
void lt_cancel(void) { g_cancel = 1; }
/* stage 5: xcn_cancelled() also gates the session thread's HTTPS (https.c);
 * a login skipped with B left g_cancel = 1, which would cancel every
 * session request. Main calls this before xst_start (login not running). */
void lt_clear_cancel(void) { if (!g_running) g_cancel = 0; }

void lt_snapshot(lt_snap_t *s) {
    lock(&g_lock);
    *s = g_st;
    unlock(&g_lock);
}

int lt_take_games(xa_title_t **games, int *n) {
    lock(&g_lock);
    *games = g_games;
    *n = g_ngames;
    g_games = NULL;
    g_ngames = 0;
    unlock(&g_lock);
    return *games ? 0 : -1;
}
