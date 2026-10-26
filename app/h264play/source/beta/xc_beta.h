/*
 * XCloud360 / orbita v0.20.3 — chave beta OBRIGATÓRIA e presa a um console
 * (build BETA=1). Ver app/h264play/BETA-GATE.md.
 *
 * Identidade do console (xcb_hwid): sha256("orbita-console-id-v1" || fusíveis
 * da CPU (secotp linhas 0..6: tipo + CPU key, as que não mudam com update de
 * dashboard) || MAC da placa de rede (config da NAND)), primeiros 32 hex. Não
 * existe arquivo de ID: trocar de pendrive não leva o ID junto. Fusíveis e MAC
 * nunca vão pro log nem pra tela; só o hash.
 *
 * Validação: GET https://x360.pistatrack.com/v1/beta/check
 *              X-Api-Key: <chave>   X-Console: <id>
 *   204 ok | 401 {"reason":"invalid"} | 403 {"reason":"revoked"} |
 *   403 {"reason":"duplicate"} (chave usada em 2 consoles) | 400 sem X-Console.
 * Última validação OK fica em XC360/BETAOK.TXT com HMAC-SHA256 (chave
 * derivada dos mesmos fusíveis/MAC, nunca mostrada): sem rede/servidor fora,
 * o app entra se ela tiver <= 7 dias. Arquivo editado/copiado de outro
 * console/de outra chave = não vale.
 *
 * THREADS
 *   main (hw 0): xcb_init, xcb_*_start, xcb_running, xcb_snapshot,
 *     xcb_authorized, xcb_key, xcb_console_*. Nada bloqueia.
 *   worker (hw 3, XCB_HW_THREAD): o pedido HTTPS e o pendrive. Só chame os
 *     *_start com a hw 3 livre (sem login, sem worker das capas, sem
 *     atualização, sem sessão); devolvem -1 se ela estiver ocupada.
 */
#ifndef XC360_BETA_H
#define XC360_BETA_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XCB_HW_THREAD
#define XCB_HW_THREAD 3
#endif

#define XCB_HOST      "x360.pistatrack.com"
#define XCB_PATH      "/v1/beta/check"
#define XCB_KEY_FILE  "BETAKEY.TXT"
#define XCB_OK_FILE   "BETAOK.TXT"
#define XCB_GRACE_S   (7LL * 24 * 3600)
#define XCB_SKEW_S    600LL                 /* relógio um pouco atrás ainda vale */
#define XCB_ALPHABET  "23456789ABCDEFGHJKMNPQRSTUVWXYZ"   /* tools/beta_keys.py */
#define XCB_NFUSE     7

/* Resultado de um pedido de validação. */
enum { XCB_R_OK = 0, XCB_R_INVALID, XCB_R_REVOKED, XCB_R_DUPLICATE, XCB_R_NOCONN, XCB_R_ERROR };

/* Fase (o que o menu mostra). */
enum {
    XCB_IDLE = 0,
    XCB_CHECKING,     /* pedido em voo (spinner) */
    XCB_NEED_KEY,     /* sem BETAKEY.TXT (ou ilegível): teclado na tela */
    XCB_OK,           /* 204 agora */
    XCB_OFFLINE_OK,   /* sem conexão, mas validada há <= 7 dias */
    XCB_INVALID,      /* 401: "Chave inválida", volta pra digitação */
    XCB_REVOKED,      /* 403 revoked */
    XCB_DUPLICATE,    /* 403 duplicate */
    XCB_NOCONN,       /* sem conexão e sem validação recente */
    XCB_ERROR         /* resposta estranha (400, 404...) */
};

/* Operações do worker. */
enum { XCB_OP_BOOT = 1, XCB_OP_SUBMIT, XCB_OP_FILE };

typedef struct {
    int       phase;
    unsigned  seq;            /* muda a cada transição (redesenhar) */
    int       busy;           /* worker rodando */
    int       op;             /* última operação (XCB_OP_*) */
    int       authorized;     /* 1 = o app pode abrir (fica 1 depois de um OK) */
    int       http;           /* último status HTTP (0 = sem resposta) */
    long long last_ok;        /* unix da última validação OK (-1 = nenhuma) */
    char      key_masked[24]; /* chave em uso, mascarada ("" = nenhuma) */
    char      msg[128];       /* linha curta pt-BR (UTF-8) */
} xcb_snap_t;

/* Plataforma (Xbox: xcb_port_xenon.c; host: o teste passa a sua). */
typedef struct {
    /* GET https. 0 = houve resposta (status em *status, corpo em body com
     * NUL), < 0 = rede/TLS (sem resposta). */
    int       (*get)(const char *host, const char *path, const char *headers,
                     int *status, char *body, size_t cap);
    /* hora unix em que dá pra confiar (Date do servidor ou RTC sensato), -1 = não sei */
    long long (*now)(void);
    int       (*load)(const char *name, void *buf, size_t cap);   /* >= 0 bytes, -1 */
    int       (*save)(const char *name, const void *buf, size_t n); /* 0 = ok */
    /* thread do worker: spawn = 0 ok / -1 ocupada; NULL = roda na hora (testes) */
    int       (*spawn)(void (*fn)(void));
    void      (*lock)(void);
    void      (*unlock)(void);
    void      (*log)(const char *line);   /* NULL = nada */
    /* 1 = a hw do worker ainda está ocupada (a função volta um pouco depois
     * do fim do trabalho); NULL = nunca */
    int       (*slot_busy)(void);
} xcb_ops_t;

/* ---- identidade (pura) ---- */
/* fuse[XCB_NFUSE] + mac[6] -> id (32 hex minúsculos + NUL) e a chave do HMAC
 * do BETAOK.TXT (32 bytes, nunca sai do app). */
void xcb_hwid(const uint64_t fuse[XCB_NFUSE], const uint8_t mac[6], char id[33], uint8_t hkey[32]);
/* "a1b2c3d4e5f6..." -> "A1B2-C3D4-E5F6" (12 primeiros hex, legível no print) */
void xcb_id_short(const char *id, char out[16]);

/* ---- chave (pura) ---- */
/* Apara pontas, 1a linha; 8-64 ASCII imprimível sem espaço. 0 = ok. */
int  xcb_key_norm(const char *in, char *out, size_t cap);
/* Forma canônica (sem '-'/espaço, maiúscula), como a ponte compara. */
void xcb_key_canon(const char *in, char *out, size_t cap);
/* "ABCDE-FGHJK" -> "*****-**HJK" (mostra só os 3 últimos). */
void xcb_key_mask(const char *key, char *out, size_t cap);
/* rc/status/corpo do GET -> XCB_R_*. */
int  xcb_classify(int rc, int status, const char *body);

/* ---- última validação assinada (pura) ---- */
/* "XCBOK1 <ts> <hmac hex>\n". Retorna o tamanho (< 0 erro). */
int  xcb_okfile_make(const uint8_t hkey[32], const char *id, const char *key, long long ts,
                     char *out, size_t cap);
/* 0 = assinatura confere pra este console + esta chave (ts em *ts). */
int  xcb_okfile_check(const uint8_t hkey[32], const char *id, const char *key,
                      const char *buf, size_t n, long long *ts);
/* 1 = dá pra entrar sem rede (now conhecido, last <= now + skew, now - last <= 7 dias). */
int  xcb_grace_ok(long long last, long long now);

/* ---- estado (main) ---- */
void xcb_init(const xcb_ops_t *ops, const char *id, const uint8_t hkey[32]);
/* Boot: lê BETAKEY.TXT e valida (ou NEED_KEY). 0 = subiu, -1 = ocupada. */
int  xcb_boot_start(void);
/* Chave digitada (XXXXX-XXXXX): valida e só grava se 204. */
int  xcb_submit_start(const char *key);
/* "Ler do pendrive": relê BETAKEY.TXT (colado no PC) e valida. */
int  xcb_file_start(void);
int  xcb_running(void);
int  xcb_authorized(void);
void xcb_snapshot(xcb_snap_t *s);
/* Chave em uso (validada). Retorna o tamanho, 0 = nenhuma. */
int  xcb_key(char *out, size_t cap);
/* ID do console (32 hex) e curto. */
const char *xcb_console_id(void);
const char *xcb_console_short(void);

/* Testes: corpo do worker sem thread. */
void xcb_run(int op, const char *key);

#ifdef __cplusplus
}
#endif
#endif
