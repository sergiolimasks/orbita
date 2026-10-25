/*
 * XCloud360 — modelo do menu de jogos (v0.16, make MENU=1).
 *
 * C puro, sem libxenon nem lwIP: interpreta as linhas do launcher (ver
 * ctrl_conn.h) e faz a navegação do cursor. Separado da UI pra poder ser
 * testado no host (hosttest/menu_test.c). Só o main thread mexe nele.
 */
#ifndef XCLOUD360_MENU_MODEL_H
#define XCLOUD360_MENU_MODEL_H

#ifdef __cplusplus
extern "C" {
#endif

#define MM_MAX_GAMES 1024
#define MM_ID_LEN    32
#define MM_NAME_LEN  64
#define MM_TEXT_LEN  160

typedef struct {
    char id[MM_ID_LEN];
    char name[MM_NAME_LEN];
    int  recent;
} mm_game_t;

/* Códigos do STATE (MM_ST_NONE = ainda não recebeu nenhum). */
enum {
    MM_ST_NONE = 0, MM_ST_IDLE, MM_ST_LOGIN, MM_ST_CATALOG, MM_ST_STARTING,
    MM_ST_QUEUE, MM_ST_STREAMING, MM_ST_RECONNECTING, MM_ST_ERROR, MM_ST_UNKNOWN
};

/* Retorno de mm_handle_line. */
enum {
    MM_EV_NONE = 0, MM_EV_STATE, MM_EV_LOGIN, MM_EV_GAME, MM_EV_CATALOG, MM_EV_PONG,
    MM_EV_IGNORED
};

typedef struct {
    mm_game_t games[MM_MAX_GAMES];   /* catálogo mostrado */
    int       ngames;
    mm_game_t pend[MM_MAX_GAMES];    /* GAME... chegando, vira o mostrado no END */
    int       npend;
    int       receiving;
    int       cursor, top;
    int       state;
    char      state_code[24];
    char      state_text[MM_TEXT_LEN];
    char      login_url[MM_TEXT_LEN];
    char      login_code[32];
    int       have_login;
    unsigned  catalogs;              /* ENDs recebidos */
    unsigned  changes;               /* ++ a cada linha que mudou algo (redesenho) */
} menu_model_t;

void             mm_init(menu_model_t *m);
/* `line` sem "\n"; é modificada (split por TAB). */
int              mm_handle_line(menu_model_t *m, char *line);
const mm_game_t *mm_current(const menu_model_t *m);
/* Navegação; `vis` = linhas visíveis da lista (ajusta o topo). */
void             mm_move(menu_model_t *m, int delta, int vis);
void             mm_page(menu_model_t *m, int dir, int vis);
void             mm_letter(menu_model_t *m, int dir, int vis);
void             mm_fix_top(menu_model_t *m, int vis);

#ifdef __cplusplus
}
#endif

#endif
