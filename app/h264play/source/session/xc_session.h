/*
 * XCloud360 stage 3 — sessão xCloud (GSSV) iniciada pelo próprio Xbox.
 *
 * Porte em C de ~/green-nx/src/core/session.cpp (GPL-3.0) + o uso que o
 * tools/xcloud-capture/rtp/capture-rtp.cpp faz dele:
 *   /v5/sessions/cloud/active (sessão velha) -> /v5/sessions/cloud/play ->
 *   <sessionPath>/state (fila / provisionamento) -> /connect (passport MSA) ->
 *   /sdp (offer POST, answer GET) -> /ice (POST locais, GET remotos) ->
 *   /keepalive -> DELETE <sessionPath>.
 *
 * Duas camadas:
 *   1. xs_*  (este arquivo, xc_session.c): uma chamada = um pedido HTTPS
 *      bloqueante, pelo https.c/xbox_auth.c do login (mesmo transporte
 *      xc_port.h, mesmo cJSON). Roda na thread da sessão (hw 3 no menu),
 *      NUNCA na thread de rede.
 *   2. xsm_* (xs_machine.h): máquina de estados que o menu/WebRTC dirigem.
 *
 * Cabeçalhos iguais aos do green-nx/capture-rtp (tier P720 = osName android,
 * 1280x720). Tokens só em RAM (zerados no free); o refresh rotacionado vai
 * pro TOKEN.TXT como no login_flow.c. Nada de token nos logs.
 */
#ifndef XC360_SESSION_H
#define XC360_SESSION_H
#include <stddef.h>
#include <stdint.h>
#include "../login/xbox_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Retornos (XS_OK = 0, XS_PENDING = "ainda não", negativos = erro). */
enum {
    XS_OK = 0,
    XS_PENDING = 1,       /* 204 / ainda sem resposta: tente de novo */
    XS_ERR = -1,          /* HTTP != 2xx, JSON inválido etc. (e->msg) */
    XS_EAUTH = -2,        /* 401 mesmo depois de renovar o login */
    XS_ELOGIN = -3,       /* sem TOKEN.TXT ou refresh recusado (invalid_grant) */
    XS_ENET = -4,         /* DNS/TCP/TLS */
    XS_ETIMEOUT = -5,     /* timeout de rede */
    XS_ECANCEL = -6,
    XS_ENOTFOUND = -7,    /* 404: sessão não existe mais */
    XS_EFAILED = -8,      /* o servidor disse que falhou (errorDetails) */
    XS_ENEEDCPU = -9,     /* v0.19.2, só no assíncrono: 401 (re-login), DNS ou
                             handshake TLS completo -> thread da sessão */
    XS_INPROGRESS = 2     /* v0.19.2 assíncrono: pedido ainda em voo */
};

#define XS_TOKEN_FILE   "TOKEN.TXT"        /* = LF_TOKEN_FILE */
#define XS_LOCALE       "en-US"            /* = default do GssvSession */
/* v0.21.38: idioma pedido ao xCloud no play (pt-BR, pt-PT, en-US, es-MX) */
void        xs_set_play_locale(const char *loc);
const char *xs_play_locale(void);
#define XS_MAX_REAUTH   3                  /* re-logins por credencial */
#define XS_CAND_LEN     256
#define XS_MAX_CANDS    24
#define XS_MAX_ACTIVE   8

/* ------------------------------------------------------------ credenciais */
typedef struct {
    char  offering[16];     /* "xgpuweb" ou "xgpuwebf2p" */
    char  host[128];        /* região, ex. "brs.core.gssv-play-prod.xboxlive.com" */
    char *gs_token;         /* Bearer do GSSV (malloc) */
    char *refresh;          /* refresh MSA atual (malloc) — passport e re-login */
    char *passport;         /* cache do token do /connect (malloc) */
    int   reauths;          /* quantos 401 já viraram re-login */
    int   no_store;         /* 1 = não grava o refresh rotacionado (testes) */
    /* v0.19.2: conexão keep-alive com `host` (todos os pedidos ao GSSV da
     * região passam por ela; fechada no xs_creds_free). */
    struct https_conn *conn;
} xs_creds_t;

void xs_creds_init(xs_creds_t *c, const char *offering);
void xs_creds_free(xs_creds_t *c);
/* Usa um refresh que o chamador já tem em RAM (copia). */
int  xs_creds_set_refresh(xs_creds_t *c, const char *refresh);
/* Lê o refresh do TOKEN.TXT. XS_ELOGIN se não houver. */
int  xs_creds_load(xs_creds_t *c, xc_err_t *e);
/* refresh -> user.auth -> XSTS (gssv) -> /v2/login/user da `offering`.
 * Rotaciona c->refresh (e grava no TOKEN.TXT). XS_ELOGIN = invalid_grant. */
int  xs_creds_login(xs_creds_t *c, xc_err_t *e);
/* Token do /connect: login.live.com/oauth20_token.srf com o refresh e o
 * escopo PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN (= fetch_passport_token). */
int  xs_creds_passport(xs_creds_t *c, xc_err_t *e);

/* ------------------------------------------------------------------ sessão */
typedef enum {
    XS_SS_NONE = 0, XS_SS_NEW, XS_SS_PROVISIONING, XS_SS_WAITING,
    XS_SS_READY, XS_SS_PROVISIONED, XS_SS_FAILED, XS_SS_UNKNOWN
} xs_sstate_t;

typedef struct {
    xs_sstate_t st;
    char raw[32];           /* "WaitingForResources"... */
    int  queue_pos;         /* -1 = não veio */
    int  wait_s;            /* -1 = não veio */
    char err_code[64];      /* Failed: errorDetails.code (ASCII) */
    char err_msg[128];      /* Failed: errorDetails.message (ASCII) */
} xs_status_t;

typedef struct {
    xs_creds_t *creds;
    char path[192];         /* "/v5/sessions/cloud/<id>" ("" = sem sessão) */
    char title[48];
    char locale[16];
    int  aq;                /* v0.19.2: XS_AQ_* em voo (0 = nenhum) */
} xs_session_t;

typedef struct {
    char c[XS_MAX_CANDS][XS_CAND_LEN];   /* "candidate:..." prontos pro libpeer */
    int  n;
    int  end;                            /* servidor mandou end-of-candidates */
} xs_cands_t;

typedef struct {
    char path[192];
    char title[48];
    char state[32];
} xs_active_t;

void xs_init(xs_session_t *s, xs_creds_t *c);
/* POST /v5/sessions/cloud/play. */
int  xs_start(xs_session_t *s, const char *title_id, xc_err_t *e);
/* Retoma uma sessão existente (path de xs_active_list). */
void xs_attach(xs_session_t *s, const char *path, const char *title_id);
/* GET <path>/state. XS_ENOTFOUND se 404. */
int  xs_get_state(xs_session_t *s, xs_status_t *st, xc_err_t *e);
/* GET /v1/waittime/<titleId> (o que o xbox.com/greenlight mostram na fila).
 * Melhor esforço; *sec = estimatedTotalWaitTimeInSeconds. */
int  xs_wait_time(xs_session_t *s, int *sec, xc_err_t *e);
/* POST <path>/connect {"userToken": passport}. Busca o passport se preciso. */
int  xs_connect(xs_session_t *s, xc_err_t *e);
/* POST <path>/sdp (offer já com o munging do lado WebRTC). */
int  xs_sdp_offer(xs_session_t *s, const char *offer_sdp, xc_err_t *e);
/* GET <path>/sdp. XS_PENDING = 204. XS_OK: *answer (malloc). */
int  xs_sdp_poll(xs_session_t *s, char **answer, xc_err_t *e);
/* POST <path>/ice: candidatos "candidate:..." locais + end-of-candidates. */
int  xs_ice_send(xs_session_t *s, const char *const *cands, int n, const char *ufrag, xc_err_t *e);
/* GET <path>/ice. XS_PENDING = 204. Teredo já vira IPv4 (porta + 9002). */
int  xs_ice_poll(xs_session_t *s, xs_cands_t *out, xc_err_t *e);
/* POST <path>/keepalive (corpo vazio). */
int  xs_keepalive(xs_session_t *s, xc_err_t *e);
/* DELETE <path> (erros ignorados). Limpa s->path. */
void xs_stop(xs_session_t *s);

/* ---- v0.19.2: assíncrono, pra thread de rede durante o jogo ----
 * Um pedido por vez, na conexão keep-alive (s->creds->conn), nunca espera.
 * start: XS_OK = em voo; XS_ENEEDCPU = ainda não há conexão com o host.
 * poll: XS_INPROGRESS até terminar; depois o MESMO retorno da função
 * bloqueante equivalente (xs_keepalive / xs_ice_send / xs_ice_poll), com
 * duas exceções que viram XS_ENEEDCPU: 401 (o re-login é da thread da
 * sessão) e a conexão perdida que só um handshake completo reabre. */
enum { XS_AQ_NONE = 0, XS_AQ_KEEPALIVE, XS_AQ_ICE_SEND, XS_AQ_ICE_POLL };
int  xs_async_start(xs_session_t *s, int kind, const char *const *cands, int n, const char *ufrag, xc_err_t *e);
int  xs_async_poll(xs_session_t *s, xs_cands_t *out, xc_err_t *e);
/* Larga o pedido em voo (derruba a conexão, sem esperar). */
void xs_async_abort(xs_session_t *s);
/* A conexão keep-alive (NULL se não houver) — estatística. */
struct https_conn *xs_conn(const xs_creds_t *c);

/* GET /v5/sessions/cloud/active. 404/204 = nenhuma (n = 0, XS_OK). */
int  xs_active_list(xs_creds_t *c, xs_active_t *out, int max, int *n, xc_err_t *e);
int  xs_delete_path(xs_creds_t *c, const char *path, xc_err_t *e);

/* ---- partes puras (testadas no host) ---- */
char *xs_build_play_body(const char *title_id, const char *locale);          /* malloc */
char *xs_build_connect_body(const char *passport);                           /* malloc */
char *xs_build_sdp_body(const char *offer_sdp);                              /* malloc */
char *xs_build_ice_body(const char *const *cands, int n, const char *ufrag); /* malloc */
/* Cabeçalhos dos pedidos ao GSSV (malloc; contém o token: xa_secret_free). */
char *xs_build_headers(const char *gs_token);
int   xs_parse_play(const char *json, char *path, size_t cap);
int   xs_parse_state(const char *json, xs_status_t *st);
/* 1 se errorDetails tem algum campo não-nulo (o "all-null" é sucesso). */
int   xs_exchange_error(const char *json, char *msg, size_t cap);
int   xs_parse_sdp_answer(const char *json, char **sdp, char *err, size_t cap);
int   xs_parse_ice(const char *json, xs_cands_t *out);
int   xs_parse_active(const char *json, xs_active_t *out, int max);
int   xs_parse_wait_time(const char *json);
/* Teredo (RFC 4380) -> IPv4 + porta. 1 = ok. Só a forma expandida (8 grupos). */
int   xs_teredo_decode(const char *addr, char *ipv4, size_t cap, int *port);
/* a=ice-ufrag: de um SDP ("" se não tiver). */
void  xs_sdp_ufrag(const char *sdp, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
