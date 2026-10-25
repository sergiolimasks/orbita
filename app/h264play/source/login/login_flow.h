/*
 * XCloud360 v0.17 — fluxo completo do login no Xbox (portável: só usa
 * xc_port.h + xbox_auth.h). Roda na thread do login (login_task.c).
 */
#ifndef XC360_LOGIN_FLOW_H
#define XC360_LOGIN_FLOW_H
#include "xbox_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*status)(const char *txt);                       /* linha de status (pt-BR, ASCII) */
    void (*login_code)(const char *uri, const char *code); /* uri NULL = tirar da tela */
} lf_ui_t;

#define LF_TOKEN_FILE "TOKEN.TXT"
#define LF_NAMES_FILE "NAMES.TXT"
#define LF_SEED_FILE  "SEED.BIN"

/* 0 = ok: *games (malloc, só jogáveis, ordenados por nome), *n. */
int lf_run(const lf_ui_t *ui, xa_title_t **games, int *n, xc_err_t *e);

#ifdef __cplusplus
}
#endif
#endif
