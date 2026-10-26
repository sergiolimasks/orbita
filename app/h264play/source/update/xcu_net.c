/*
 * XCloud360 — GET na ponte de atualização por HTTPS keep-alive
 * (https_conn_*, mesmas raízes/verificação do login). Uma conexão só, do
 * worker do xc_update; xcu_net_close no fim. Usado no Xbox
 * (xcu_port_xenon.c) e no teste live do host.
 */
#include "xcu_port.h"
#include "../login/https.h"

#include <stdio.h>
#include <string.h>

static https_conn_t *g_c = NULL;

int xcu_net_get(const char *host, const char *path, const char *headers, size_t max_body,
                int *status, char **body, size_t *len, char *err, size_t errcap) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    *status = 0;
    *body = NULL;
    *len = 0;
    if (https_init(&e) != 0) { snprintf(err, errcap, "%s", e.msg); return -1; }
    if (g_c && strcmp(https_conn_host(g_c), host)) { https_conn_free(g_c); g_c = NULL; }
    if (!g_c && !(g_c = https_conn_new(host))) { snprintf(err, errcap, "sem memoria"); return -1; }
    https_resp_t r;
    memset(&r, 0, sizeof(r));
    int rc = https_conn_request(g_c, "update", "GET", path, headers, NULL, 0, max_body, &r, &e);
    if (rc != 0) {
        snprintf(err, errcap, "%s", e.msg[0] ? e.msg : "erro de rede");
        https_free(&r);
        return -1;
    }
    *status = r.status;
    *body = r.body;   /* https_free = free(body) */
    *len = r.len;
    return 0;
}

void xcu_net_close(void) {
    if (g_c) { https_conn_free(g_c); g_c = NULL; }
}
