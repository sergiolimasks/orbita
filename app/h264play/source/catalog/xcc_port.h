/*
 * XCloud360 — o que o xc_catalog precisa da plataforma além do xc_port.h.
 *   Xbox: xcc_port_xenon.c (lock() do libxenon + lwsync, hw thread 3).
 *   host: hosttest/catalog/xcc_port_posix.c (pthread).
 */
#ifndef XC360_XCC_PORT_H
#define XC360_XCC_PORT_H
#include <stddef.h>

void xccp_lock(void);
void xccp_unlock(void);
void xccp_sync(void);                 /* barreira (lwsync no Xbox) */
/* Sobe `fn` na thread do worker. 0 = ok, -1 = slot ocupado/falhou. */
int  xccp_spawn(void (*fn)(void));
/* 1 = a hw do worker está ocupada (login, sessão ou o próprio worker). */
int  xccp_slot_busy(void);

/* HTTPS keep-alive por host (xcc_net.c) — só na thread do worker. */
int  xcc_net_fetch(const char *host, const char *path, size_t max_body,
                   int *status, char **body, size_t *len);
void xcc_net_close_all(void);

#endif
