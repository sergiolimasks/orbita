/*
 * XCloud360 stage 5 — host twin of source/session/xs_task.c (pthread instead
 * of a libxenon hardware thread). Same body (xsm_thread_run: the 50 ms
 * reaction slices, the "credentials freed when the session ends" rule and,
 * v0.19.2, the hand-off of the live phases to the network thread), so the
 * E2E exercises the session thread exactly as the Xbox runs it.
 * xst_hw3_busy_hook: the E2E's "decoder" can hold the session's hw thread
 * (like the 4th libavcodec frame thread on hw3): xst_resume fails then.
 */
#include "../../source/session/xs_task.h"
#include "../../source/session/xs_port.h"
#include "../../source/login/https.h"
#include "../../source/login/xc_port.h"

#include <pthread.h>
#include <string.h>

static xsm_t        g_m;
static int          g_inited = 0;
static volatile int g_running = 0;
static pthread_t    g_th;
static int          g_have_th = 0;
int (*xst_hw3_busy_hook)(void) = 0;

static void *task_main(void *arg) {
    (void)arg;
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    https_init(&e);
    xsm_thread_run(&g_m);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_running = 0;
    return NULL;
}

static int spawn(void) {
    if (g_have_th) { pthread_join(g_th, NULL); g_have_th = 0; }
    g_running = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (pthread_create(&g_th, NULL, task_main, NULL) != 0) {
        g_running = 0;
        return -1;
    }
    g_have_th = 1;
    return 0;
}

int xst_start(const char *title_id, const char *name, int f2p, int active_policy, const char *refresh) {
    xsm_cfg_t cfg;
    xsm_cfg_defaults(&cfg, title_id, name);
    cfg.f2p = f2p;
    cfg.active_policy = active_policy;
    return xst_start_cfg(&cfg, refresh);
}

int xst_start_cfg(const xsm_cfg_t *cfg, const char *refresh) {
    if (g_running) return -1;
    if (xst_hw3_busy_hook && xst_hw3_busy_hook()) return -1;
    if (g_inited && xsm_owner(&g_m) != XSM_OWN_DONE) return -1;
    if (g_have_th) { pthread_join(g_th, NULL); g_have_th = 0; }
    if (g_inited) xsm_free(&g_m);
    xsm_init(&g_m, cfg, refresh);
    g_inited = 1;
    if (spawn() != 0) { xsm_set_owner(&g_m, XSM_OWN_DONE); return -1; }
    return 0;
}

int xst_resume(void) {
    if (!g_inited || g_running) return -1;
    if (xst_hw3_busy_hook && xst_hw3_busy_hook()) return -1;
    if (xsm_owner(&g_m) != XSM_OWN_WANT_THREAD) return -1;
    xsm_set_owner(&g_m, XSM_OWN_THREAD);
    if (spawn() != 0) { xsm_set_owner(&g_m, XSM_OWN_WANT_THREAD); return -1; }
    return 0;
}

int  xst_running(void) { return g_running; }
int  xst_owner(void) { return g_inited ? xsm_owner(&g_m) : XSM_OWN_DONE; }
void xst_stop(void) { if (g_inited) xsm_request_stop(&g_m); }

unsigned xst_state_line(char *out, size_t cap) {
    if (!g_inited) { if (cap) out[0] = 0; return 0; }
    return xsm_state_line(&g_m, out, cap);
}

xsm_t *xst_machine(void) { return g_inited ? &g_m : NULL; }
