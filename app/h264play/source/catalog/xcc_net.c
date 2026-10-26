/* XCloud360 — GETs públicos do xc_catalog sobre o https.c, uma conexão
 * keep-alive por host (catalog.gamepass.com, displaycatalog, store-images):
 * um handshake ECDHE (~75 ms de CPU no Xenon) serve dezenas de capas.
 * Só a thread do worker usa; xcc_net_close_all no fim dela. */
#include "xcc_port.h"
#include "../login/https.h"
#include "../login/xc_port.h"

#include <stdlib.h>
#include <string.h>

#define NCONN 3

static https_conn_t *g_conn[NCONN];

static https_conn_t *conn_for(const char *host) {
    for (int i = 0; i < NCONN; i++)
        if (g_conn[i] && !strcmp(https_conn_host(g_conn[i]), host)) return g_conn[i];
    int slot = -1;
    for (int i = 0; i < NCONN; i++) if (!g_conn[i]) { slot = i; break; }
    if (slot < 0) { https_conn_free(g_conn[0]); g_conn[0] = NULL; slot = 0; }
    g_conn[slot] = https_conn_new(host);
    return g_conn[slot];
}

static void drop(https_conn_t *c) {
    for (int i = 0; i < NCONN; i++) if (g_conn[i] == c) { https_conn_free(c); g_conn[i] = NULL; }
}

int xcc_net_fetch(const char *host, const char *path, size_t max_body,
                  int *status, char **body, size_t *len) {
    *status = 0;
    *body = NULL;
    *len = 0;
    https_conn_t *c = conn_for(host);
    if (!c) return -1;
    https_resp_t r;
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    int rc = https_conn_request(c, "capas", "GET", path, "Accept: */*\r\n", NULL, 0, max_body, &r, &e);
    if (rc != 0) {
        xc_log("[capas] %s: %s", host, e.msg);
        drop(c);
        return -1;
    }
    *status = r.status;
    *body = r.body;
    *len = r.len;
    return 0;
}

void xcc_net_close_all(void) {
    for (int i = 0; i < NCONN; i++) if (g_conn[i]) { https_conn_free(g_conn[i]); g_conn[i] = NULL; }
}
