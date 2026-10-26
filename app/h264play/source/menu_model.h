/*
 * XCloud360 — modelo do menu de jogos (v0.16).
 *
 * C puro, sem libxenon nem lwIP: guarda o catálogo (linhas GAME/END que o
 * main monta a partir da lista do login do Xbox), o STATE da sessão do Xbox
 * (xn_state_line, mesmos códigos de sempre) e a navegação do menu novo
 * (fileiras + botões do topo + confirmação). Separado da UI pra poder ser
 * testado no host (hosttest/menu_test.c). Só o main thread mexe nele.
 * v0.20.0: o modo PC (launcher :9002) saiu; a lista de texto antiga
 * (mm_move/mm_page/mm_letter) e o menu de energia do Start também.
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
    MM_EV_NONE = 0, MM_EV_STATE, MM_EV_GAME, MM_EV_CATALOG, MM_EV_IGNORED
};

/* v0.20 (menu novo, source/ui): fileiras horizontais como no site do xCloud.
 * Cada fileira é uma lista de índices em games[]; o foco é (row, col) e cada
 * fileira lembra a sua coluna (cima/baixo volta onde estava). row = -1 são
 * os botões do topo à direita (Reiniciar / Desligar). */
#define MM_MAX_ROWS   8
#define MM_ROW_TITLE  48
typedef struct {
    char  title[MM_ROW_TITLE];       /* UTF-8 */
    short idx[MM_MAX_GAMES];
    int   n;
    int   col;                       /* coluna lembrada */
} mm_row_t;

/* v0.20.1: + Atualizar (à esquerda; A abre a tela de atualização, sem
 * confirmação). O foco ao subir pro topo começa no Reiniciar. */
/* v0.20.3: + Configurações (engrenagem, mais à esquerda), só com chave beta
 * válida (settings_on); sem ela o foco nunca chega nela. */
enum { MM_HB_SETTINGS = 0, MM_HB_UPDATE, MM_HB_REBOOT, MM_HB_SHUTDOWN, MM_HB_COUNT };   /* = XUI_HB_* */
#define MM_ROW_HEADER (-1)

/* Retorno de mm_press_a. */
enum { MM_A_NONE = 0, MM_A_PLAY, MM_A_CONFIRM, MM_A_REBOOT, MM_A_SHUTDOWN, MM_A_UPDATE, MM_A_SETTINGS };

typedef struct {
    mm_game_t games[MM_MAX_GAMES];   /* catálogo mostrado */
    int       ngames;
    mm_game_t pend[MM_MAX_GAMES];    /* GAME... chegando, vira o mostrado no END */
    int       npend;
    int       receiving;
    int       cursor;                /* jogo em foco (índice em games) */
    int       state;
    char      state_code[24];
    char      state_text[MM_TEXT_LEN];
    unsigned  catalogs;              /* ENDs recebidos */
    unsigned  changes;               /* ++ a cada linha que mudou algo (redesenho) */
    /* v0.20: fileiras + botões do topo + confirmação */
    mm_row_t  rows[MM_MAX_ROWS];
    int       nrows;
    int       row;                   /* MM_ROW_HEADER = botões do topo */
    int       hcol;                  /* MM_HB_* em foco no topo */
    int       confirm;               /* 0 ou MM_PWR_REBOOT/MM_PWR_SHUTDOWN aberto */
    int       confirm_yes;           /* foco no diálogo: 0 = Cancelar (padrão) */
    unsigned  rows_gen;              /* ++ quando as fileiras são refeitas */
    int       settings_on;           /* v0.20.3: botão Configurações disponível */
} menu_model_t;

/* v0.19.3: ações de energia (v0.20.0: botões do topo + confirmação). */
enum { MM_PWR_BACK = 0, MM_PWR_REBOOT, MM_PWR_SHUTDOWN };

/* Como terminou o mm_power_run (só volta no host; no Xbox o SMC desliga). */
enum { MM_PWR_NO_SESSION = 0, MM_PWR_CLEAN, MM_PWR_TIMEOUT };

#define MM_PWR_TIMEOUT_MS 3000

/* v0.19.3: Reiniciar/Desligar. O main fornece a plataforma; aqui só a
 * ordem (testável no host, hosttest/menu_test.c):
 *   sessão aberta -> show("Encerrando sessao...") + stop() e espera done()
 *   até timeout_ms (DELETE travado não segura o console) -> show + reboot()
 *   ou shutdown(). Sem sessão vai direto. */
typedef struct {
    void              *u;
    int                (*session_open)(void *u);   /* 1 = há o que encerrar */
    void               (*session_stop)(void *u);   /* pede o DELETE / STOP */
    int                (*session_done)(void *u);   /* 1 = encerrou */
    unsigned long long (*now_us)(void *u);
    void               (*wait)(void *u);           /* ~10 ms (e serviços do main) */
    void               (*show)(void *u, const char *l1, const char *l2);
    void               (*reboot)(void *u);
    void               (*shutdown)(void *u);
} mm_power_ops_t;

void             mm_init(menu_model_t *m);
/* `line` sem "\n"; é modificada (split por TAB). */
int              mm_handle_line(menu_model_t *m, char *line);
const mm_game_t *mm_current(const menu_model_t *m);
/* `action` = MM_PWR_REBOOT/SHUTDOWN; retorna MM_PWR_NO_SESSION/CLEAN/TIMEOUT
 * (-1 = ação inválida, nada feito). */
int              mm_power_run(const mm_power_ops_t *o, int action, unsigned timeout_ms);

/* ---- v0.20: fileiras (menu novo). O END do catálogo chama
 * mm_rows_default ("Voltar a jogar" = recent, "Todos os jogos" = tudo) e
 * mantém o foco no mesmo jogo; quem tiver listas melhores (catálogo:
 * populares, recentes) refaz com clear/add/push_id e chama mm_rows_fix. */
void             mm_rows_clear(menu_model_t *m);
int              mm_rows_add(menu_model_t *m, const char *title_utf8);   /* -1 = cheio */
int              mm_rows_push(menu_model_t *m, int r, int game);         /* 0 = não coube */
int              mm_rows_push_id(menu_model_t *m, int r, const char *id); /* 0 = id não existe */
void             mm_rows_default(menu_model_t *m);
/* Tira fileiras vazias, prende row/col e sincroniza o cursor. */
void             mm_rows_fix(menu_model_t *m);
/* Fim de montar fileiras (clear/add/push): tira as vazias e devolve o foco
 * ao jogo do cursor, na mesma fileira se ele estiver nela. */
void             mm_rows_done(menu_model_t *m);
/* Direcional: dx = esquerda/direita na fileira (ou nos botões do topo / no
 * diálogo), dy = troca de fileira (subir da 1ª vai pro topo). */
void             mm_nav(menu_model_t *m, int dx, int dy);
/* LB/RB: pula `vis` cards na fileira. */
void             mm_nav_page(menu_model_t *m, int dir, int vis);
/* A: MM_A_PLAY (jogo em mm_current), MM_A_CONFIRM (abriu a confirmação),
 * MM_A_REBOOT/MM_A_SHUTDOWN (confirmado: chamar mm_power_run com
 * MM_PWR_REBOOT/MM_PWR_SHUTDOWN) ou MM_A_NONE. */
int              mm_press_a(menu_model_t *m);
/* B: fecha a confirmação ou desce do topo. 1 = consumiu o B. */
int              mm_press_b(menu_model_t *m);
/* Jogo em foco nas fileiras (NULL no topo / catálogo vazio). */
const mm_game_t *mm_focus_game(const menu_model_t *m);

#ifdef __cplusplus
}
#endif

#endif
