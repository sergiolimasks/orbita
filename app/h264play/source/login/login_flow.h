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

/* Ganchos opcionais (menu de capas, source/catalog/xc_catalog.c). Rodam na
 * thread do login, então podem usar o pendrive (xc_store_*) e o HTTPS.
 *   on_start : começo do lf_run (antes de qualquer rede) — cache do menu;
 *   on_cloud : gsToken do xgpuweb ainda válido (ep) — ex. /v2/titles/mru;
 *              NUNCA guardar ep->token;
 *   on_titles: lista final (jogáveis, ordenada) logo antes do lf_run voltar.
 * NULL (padrão) = nenhum gancho: comportamento idêntico ao de antes. */
typedef struct {
    void (*on_start)(void);
    void (*on_cloud)(const xa_endpoint_t *ep);
    void (*on_titles)(const xa_title_t *t, int n);
} lf_hooks_t;
void lf_set_hooks(const lf_hooks_t *h);

#ifdef __cplusplus
}
#endif
#endif
