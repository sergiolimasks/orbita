/*
 * XCloud360 / orbita v0.20.3 — plataforma do xc_beta no Xbox.
 *   identidade: fusíveis (xenon_secotp_read_line 0..6) + MAC da config da
 *     NAND (xenon_config_get_mac_addr, a mesma que o enet usa). Lido UMA vez
 *     no boot pelo main, depois da rede subir (a config já está em cache).
 *   worker: hw XCB_HW_THREAD (3) com pilha própria de 256 KB (handshake TLS).
 *   rede: https_request (uma conexão, Connection: close), raízes embutidas.
 *   hora: Date do servidor (xc_time_now) ou, sem ele, o RTC do Xbox se for
 *     posterior à build (o RTC sozinho não é confiável: só serve pro "7 dias").
 *   pendrive: xc_store_* (já fazem o xc_usb_hold).
 */
#include "xc_beta.h"
#include "../login/xc_port.h"
#include "../login/https.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <ppc/atomic.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_soc/xenon_secotp.h>
#include <xenon_nand/xenon_config.h>

void xcbp_hw_material(uint64_t fuse[XCB_NFUSE], uint8_t mac[6]);
const xcb_ops_t *xcbp_ops(void);

void xcbp_hw_material(uint64_t fuse[XCB_NFUSE], uint8_t mac[6]) {
    for (int i = 0; i < XCB_NFUSE; i++) fuse[i] = xenon_secotp_read_line(i);
    xenon_config_get_mac_addr(mac);
}

static uint8_t      g_stack[256 * 1024] __attribute__((aligned(128)));
static unsigned int g_lk = 0;

static void p_lock(void)   { lock(&g_lk); }
static void p_unlock(void) { unlock(&g_lk); }

static int p_spawn(void (*fn)(void)) {
    if (xenon_is_thread_task_running(XCB_HW_THREAD)) return -1;
    return xenon_run_thread_task(XCB_HW_THREAD, g_stack + sizeof(g_stack) - 0x100, (void *)fn) == 0 ? 0 : -1;
}

static int p_get(const char *host, const char *path, const char *headers, int *status, char *body, size_t cap) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    *status = 0;
    if (cap) body[0] = 0;
    if (https_init(&e) != 0) return -1;
    https_resp_t r;
    memset(&r, 0, sizeof(r));
    int rc = https_request("beta", "GET", host, path, headers, NULL, 0, 4096, &r, &e);
    if (rc != 0) {
        xc_log("[beta] %s: %s", host, e.msg);   /* v0.21.43: no log enviado (era só UART) */
        https_free(&r);
        return -1;
    }
    *status = r.status;
    if (cap && r.body) snprintf(body, cap, "%s", r.body);
    https_free(&r);
    return 0;
}

static long long p_now(void) {
    long long t = xc_time_now();
    if (t >= 0) return t;
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return -1;
    return (long long)tv.tv_sec >= xc_time_floor() ? (long long)tv.tv_sec : -1;
}

static void p_log(const char *line) { xc_log("%s", line); }
static int  p_slot_busy(void) { return xenon_is_thread_task_running(XCB_HW_THREAD) ? 1 : 0; }

static const xcb_ops_t g_xops = { p_get, p_now, xc_store_load, xc_store_save, p_spawn, p_lock, p_unlock, p_log,
                                  p_slot_busy };

const xcb_ops_t *xcbp_ops(void) { return &g_xops; }
