/*
 * XCloud360 v0.20.1 — log/STAT dos testadores beta pra nuvem (substitui o
 * UDP :9003 pro PC). Ver app/h264play/LOGS-UPLOAD.md.
 *
 * Opt-in: só envia com a chave do testador no pendrive, XC360/BETAKEY.TXT
 * (uma linha, ASCII imprimível de 8 a 64 caracteres; espaços/quebras nas
 * pontas são ignorados, maiúsculas/minúsculas ficam como estão). Sem o
 * arquivo (ou vazio/inválido) o app não envia nada e guarda só as últimas
 * XLU_MIN_BYTES de log (pra uma chave digitada depois ter contexto).
 *
 *   POST https://x360.pistatrack.com/v1/logs
 *   X-Api-Key: <BETAKEY.TXT>
 *   X-Console: <id de hardware, 32 hex>   (v0.20.3; ver beta/xc_beta.h)
 *   {"console":"<o mesmo id>","app":"v0.20.1","game":"<nome>",
 *    "lines":["...", ...]}
 *   2xx = ok; 401/403 = chave recusada (XC_BETA_INVALID: para até o próximo
 *   boot); 413 = grande (lote menor); 429 / 5xx / rede = backoff.
 *
 * THREADS
 *   qualquer uma: xlu_line (np_log_line / xc_log caem aqui), xlu_set_game,
 *     xlu_flush_soon, xlu_add_secret, xc_beta_* — só memória, com lock.
 *   worker (hw 3, o do catálogo/capas, só no menu): xlu_step lê a chave e o
 *     ID no 1º passo, grava a chave nova (xc_beta_save_key) e manda no
 *     máximo um lote por chamada (a cada ~60 s, logo depois de
 *     xlu_flush_soon — fim de sessão, erro — ou com o buffer em 3/4). TLS
 *     persistente (https_conn). No jogo o worker não roda: os lotes esperam
 *     a volta pro menu (nada de HTTPS disputando CPU com decoder/WebRTC).
 * Buffer em RAM de XLU_BUF_BYTES; cheio = descarta as linhas mais antigas.
 * Filtro antes de guardar: segredos registrados (código de login, a própria
 * chave), sequências longas com cara de token e e-mails viram marcadores.
 */
#ifndef XC360_XC_LOGUP_H
#define XC360_XC_LOGUP_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XLU_HOST        "x360.pistatrack.com"
#define XLU_PATH        "/v1/logs"
#define XLU_KEY_FILE    "BETAKEY.TXT"
#define XLU_BUF_BYTES   (200 * 1024)
#define XLU_MIN_BYTES   (16 * 1024)    /* sem chave: só o fim do log */
#define XLU_PERIOD_MS   60000
#define XLU_MIN_GAP_MS  4000           /* servidor: <= 30 lotes/min por console */
#define XLU_BATCH_MAX   (256 * 1024)   /* corpo sem compressão (servidor: <= 512 KB) */
#define XLU_LINE_MAX    600

/* ---------------------------------------------------- estado do beta */
enum {
    XC_BETA_UNKNOWN = 0,   /* o worker ainda não leu o pendrive */
    XC_BETA_NONE,          /* sem BETAKEY.TXT (ou inválido): não envia */
    XC_BETA_ACTIVE,        /* chave presente e não recusada: envia */
    XC_BETA_INVALID        /* a ponte respondeu 401/403: parado até o boot */
};
/* Qualquer thread. 1 = chave presente e não recusada. */
int  xc_beta_active(void);
/* Qualquer thread: XC_BETA_* (pro menu mostrar "chave beta inválida"). */
int  xc_beta_state(void);
/* Qualquer thread: copia o ID do console (32 hex; "" até o main chamar
 * xlu_set_console_id). Retorna o tamanho. v0.20.3: o ID é de HARDWARE
 * (xcb_hwid: fusíveis + MAC); o CONSOLE.ID aleatório do pendrive saiu. */
int  xc_console_id(char *out, size_t cap);
/* Boot (main): ID de hardware usado no campo "console" e no X-Console. Sem
 * ele nada é enviado. */
void xlu_set_console_id(const char *id32);
/* v0.20.3 (gate beta): a chave já validada pelo xc_beta (que grava o
 * BETAKEY.TXT ele mesmo): liga o envio com ela sem regravar o arquivo. */
void xlu_use_key(const char *key);
/* Qualquer thread (o teclado na tela vai usar): valida (8-64 ASCII
 * imprimível depois de aparar as pontas) e agenda a gravação em
 * XC360/BETAKEY.TXT; o worker grava no próximo passo e passa a enviar com
 * ela (uma chave nova também tira o XC_BETA_INVALID). 0 = aceita, -1 =
 * formato inválido. */
int  xc_beta_save_key(const char *key);

/* ------------------------------------------------------------ log */
/* Transporte (testes do host trocam por um falso). Retorna 0 se houve
 * resposta HTTP (status em *status), < 0 se não. */
typedef int (*xlu_post_fn)(const char *host, const char *path, const char *headers,
                           const char *body, size_t len, int *status);

/* Uma vez, no boot (antes de qualquer log). app = "v0.20.1". */
void xlu_init(const char *app);
/* Testes: transporte e relógio próprios (NULL = HTTPS / xcn_now_ms). */
void xlu_set_transport(xlu_post_fn post);
void xlu_set_clock(unsigned long long (*now_ms)(void));
/* Qualquer thread: uma linha (sem "\n"). Filtrada antes de guardar. */
void xlu_line(const char *text);
/* v0.21.14: linhas descartadas porque o lock estava ocupado */
unsigned xlu_line_drops(void);
/* Qualquer thread: título atual ("" = nenhum) pro campo "game". */
void xlu_set_game(const char *name_utf8);
/* Qualquer thread: manda o próximo lote assim que o worker puder. */
void xlu_flush_soon(void);
/* Qualquer thread: valor que nunca pode sair (código de login...): toda
 * ocorrência vira "[x]". Até 8; o mais novo substitui o mais velho. */
void xlu_add_secret(const char *s);

/* Worker (hw 3): no máximo uma ação. 1 = trabalhou, 0 = nada a fazer. */
int  xlu_step(void);
/* Worker parando: fecha a conexão. */
void xlu_finish(void);

typedef struct {
    unsigned batches, lines_sent, lines_dropped, errors, rejected_413;
    int      last_status, beta;
    size_t   buffered;
    unsigned long long backoff_until_ms;
} xlu_stats_t;
void xlu_stats(xlu_stats_t *s);

/* Parte pura (testada no host): filtra uma linha no lugar. */
void xlu_redact(char *line, size_t cap);
/* Testes: volta ao estado do boot (buffer, beta, ID, backoff). */
void xlu_reset(void);

#ifdef __cplusplus
}
#endif
#endif
