/* XCloud360 stage 3 — host: ver mock_port.c. */
#ifndef XC360_MOCK_PORT_H
#define XC360_MOCK_PORT_H
#include <stddef.h>
#include <stdint.h>

#define MOCK_MAX_REQ 8192
#define MOCK_RTT_MS  40

typedef struct {
    char     method[8];
    char     host[96];
    char     path[256];
    char    *headers;
    char    *body;        /* NULL = sem corpo */
    int      has_body;
    int      status;
    uint64_t t;
    int      nb;          /* v0.19.2: veio do caminho não-bloqueante (hw1) */
    int      conn_id;     /* v0.19.2: conexão keep-alive que levou (0 = avulso) */
} mock_req_t;

/* Devolve 0 e preenche status e body (malloc), ou um HTTPS_E* de transporte. */
typedef int (*mock_handler_t)(void *ctx, const mock_req_t *q, int *status, char **body);

extern uint64_t       g_now_ms;
extern mock_handler_t g_mock_handler;
extern void          *g_mock_ctx;
extern mock_req_t     g_reqs[MOCK_MAX_REQ];
extern int            g_nreq;
extern char          *g_logbuf;
extern int            g_verbose;

/* v0.19.2: conexões keep-alive criadas (cada uma = 1 "handshake"). */
extern int g_mock_conns;
void        mock_reset(void);
void        mock_set_token(const char *t);
const char *mock_get_token(void);

#endif
