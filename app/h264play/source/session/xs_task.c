/* XCloud360 stage 3 — ver xs_task.h. Só Xbox (libxenon). */
#ifdef XENON
#include "xs_task.h"
#include "xs_port.h"
#include "../login/xc_port.h"
#include "../login/https.h"

#include <stdio.h>
#include <string.h>

#include <xenon_soc/xenon_power.h>

static uint8_t      g_stack[256 * 1024] __attribute__((aligned(128)));
static xsm_t        g_m;
static int          g_inited = 0;
static volatile int g_running = 0;

static inline void lwsync(void) { __asm__ __volatile__("lwsync" ::: "memory"); }

static void task_main(void) {
    {
        /* stage 5: normally the login thread already did it (catalog from the
         * Xbox); no-op then. Without it https_request would use an unseeded
         * DRBG. */
        xc_err_t e;
        memset(&e, 0, sizeof(e));
        if (https_init(&e) != 0) xc_log("[sess] https_init falhou: %s", e.msg);
    }
    /* v0.19.2: volta em DONE/ERROR (credenciais liberadas) ou ao entregar
     * uma fase ao vivo pra thread de rede (owner = NET: não toca mais). */
    xsm_thread_run(&g_m);
    lwsync();
    g_running = 0;
}

static int spawn(void) {
    g_running = 1;
    lwsync();
    int r = xenon_run_thread_task(XST_HW_THREAD, g_stack + sizeof(g_stack) - 0x100, (void *)task_main);
    if (r != 0) {
        g_running = 0;
        xc_log("[sess] nao consegui subir a thread da sessao (%d)", r);
        return -1;
    }
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
    if (g_running || xenon_is_thread_task_running(XST_HW_THREAD)) return -1;
    if (g_inited && xsm_owner(&g_m) != XSM_OWN_DONE) return -1;   /* sessão anterior viva (rede) */
    if (g_inited) xsm_free(&g_m);
    xsm_init(&g_m, cfg, refresh);
    g_inited = 1;
    if (spawn() != 0) {
        xsm_set_owner(&g_m, XSM_OWN_DONE);
        return -1;
    }
    return 0;
}

int xst_resume(void) {
    if (!g_inited || g_running || xenon_is_thread_task_running(XST_HW_THREAD)) return -1;
    if (xsm_owner(&g_m) != XSM_OWN_WANT_THREAD) return -1;
    xsm_set_owner(&g_m, XSM_OWN_THREAD);
    if (spawn() != 0) {
        xsm_set_owner(&g_m, XSM_OWN_WANT_THREAD);
        return -1;
    }
    return 0;
}

int  xst_running(void) { return g_running || xenon_is_thread_task_running(XST_HW_THREAD); }
int  xst_owner(void) { return g_inited ? xsm_owner(&g_m) : XSM_OWN_DONE; }
void xst_stop(void) { if (g_inited) xsm_request_stop(&g_m); }

unsigned xst_state_line(char *out, size_t cap) {
    if (!g_inited) { if (cap) out[0] = 0; return 0; }
    return xsm_state_line(&g_m, out, cap);
}

xsm_t *xst_machine(void) { return g_inited ? &g_m : NULL; }
#endif
