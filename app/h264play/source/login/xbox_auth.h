/*
 * XCloud360 v0.17 — login Microsoft/Xbox + catálogo xCloud, em C.
 * Porte de ~/green-nx/src/core/auth.cpp + catalog.cpp:
 *   device code (login.microsoftonline.com/consumers) -> refresh token ->
 *   user.auth.xboxlive.com -> xsts (RP http://gssv.xboxlive.com/) ->
 *   xgpuweb(.f2p).gssv-play-prod.xboxlive.com/v2/login/user (gsToken + região)
 *   -> <região>/v2/titles -> displaycatalog (nomes).
 * Tokens só em memória (malloc, zerados no free) e o refresh no pendrive;
 * nada de token nos logs.
 */
#ifndef XC360_XBOX_AUTH_H
#define XC360_XBOX_AUTH_H
#include <stddef.h>
#include "https.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XA_CLIENT_ID "1f907974-e22b-4810-a9de-d9647380c97e"
#define XA_SCOPE     "xboxlive.signin openid profile offline_access"

enum { XA_OK = 0, XA_PENDING = 1, XA_SLOWDOWN = 2, XA_EXPIRED = 3, XA_DENIED = 4,
       XA_INVALID_GRANT = 5, XA_ERR = -1 };

typedef struct {
    char  user_code[32];
    char  uri[128];
    char *device_code;      /* malloc */
    int   interval;
    int   expires_in;
} xa_devcode_t;

#define XA_F_OWNED     0x01
#define XA_F_SUB       0x02
#define XA_F_INSUB     0x04
#define XA_F_ADSUP     0x08
#define XA_F_ADGRANT   0x10
#define XA_F_ADPLAY    0x20

typedef struct {
    char     id[48];        /* titleId (o launcher/capture-rtp usam este) */
    char     product[24];   /* bigId da Store */
    char     name[64];      /* UTF-8 (v0.20.0, xa_name_clean), vazio = sem nome */
    unsigned flags;
} xa_title_t;

typedef struct {
    char  host[128];        /* ex. "brs.core.gssv-play-prod.xboxlive.com" */
    char *token;            /* gsToken, malloc */
} xa_endpoint_t;

void xa_devcode_free(xa_devcode_t *d);
void xa_endpoint_free(xa_endpoint_t *p);
void xa_secret_free(char **s);   /* zera e libera */

int xa_request_device_code(xa_devcode_t *dc, xc_err_t *e);
/* XA_OK: *refresh = novo refresh token (malloc). */
int xa_poll_device_code(const xa_devcode_t *dc, char **refresh, xc_err_t *e);
/* XA_OK: *refresh_new (rotacionado; pode vir NULL), *access. */
int xa_refresh(const char *refresh, char **refresh_new, char **access, xc_err_t *e);
int xa_user_authenticate(const char *access, char **user_token, xc_err_t *e);
int xa_xsts_authorize(const char *user_token, const char *relying_party, char **xsts, xc_err_t *e);
int xa_streaming_login(const char *offering, const char *gssv_token, xa_endpoint_t *ep, xc_err_t *e);
/* /v2/titles -> lista (malloc). */
int xa_fetch_titles(const xa_endpoint_t *ep, xa_title_t **out, int *n, xc_err_t *e);
/* /v2/titles/mru?mr=<max> ("Voltar a jogar" do xbox.com/play): mesmo formato
 * de /v2/titles, na ordem do mais recente. Lista malloc (pode vir vazia). */
int xa_fetch_mru(const xa_endpoint_t *ep, int max, xa_title_t **out, int *n, xc_err_t *e);
/* Nomes via displaycatalog pros títulos jogáveis sem nome. Melhor esforço:
 * retorna quantos nomes resolveu. `progress` pode ser NULL. */
int xa_fetch_names(xa_title_t *t, int n, void (*progress)(int done, int total), xc_err_t *e);

/* ---- partes puras (testadas no host) ---- */
int  xa_parse_titles(const char *json, xa_title_t **out, int *n);
/* Preenche nomes dos títulos cujo product aparece em `json` (displaycatalog). */
int  xa_parse_names(const char *json, xa_title_t *t, int n);
/* f2p: dobra a lista do xgpuwebf2p na principal (como merge_ad_supported). */
int  xa_merge_f2p(xa_title_t **main, int *nmain, const xa_title_t *ads, int nads);
int  xa_playable(const xa_title_t *t);
/* Remove os não jogáveis e ordena por nome (sem caixa), como o launcher. */
int  xa_finalize(xa_title_t *t, int n);
/* UTF-8 -> ASCII imprimível (acentos latinos viram a letra base, (TM)/(R)
 * somem), espaços colapsados, corta em cap-1 com "...". */
void xa_ascii_fold(char *dst, size_t cap, const char *utf8);
/* x-www-form-urlencoded (malloc). */
char *xa_urlencode(const char *s);

/* v0.20.0: nome de jogo em UTF-8 (acentos ficam; inválido, TM/R/C e
 * controle saem; espaços colapsados; corta em cap-1 com "..." sem partir
 * um caractere). O menu novo desenha UTF-8. */
void xa_name_clean(char *dst, size_t cap, const char *utf8);

/* Cache de nomes: 1a linha XA_NAMES_MAGIC, depois productId\tnome (UTF-8)
 * por linha. Cache sem o cabeçalho (o ASCII de antes da v0.20.0) = vazio. */
#define XA_NAMES_MAGIC "XCNAMES 2"
int  xa_names_from_cache(const char *txt, xa_title_t *t, int n);
char *xa_names_to_cache(const xa_title_t *t, int n);   /* malloc */

#ifdef __cplusplus
}
#endif
#endif
