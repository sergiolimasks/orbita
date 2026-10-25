/*
 * XCloud360 — modelo do menu de jogos (v0.16). Ver menu_model.h.
 */
#ifdef XCLOUD360_MENU

#include "menu_model.h"

#include <string.h>

void mm_init(menu_model_t *m) {
    memset(m, 0, sizeof(*m));
}

/* Copia só ASCII imprimível (o launcher promete ASCII; um byte UTF-8 que
 * escape vira '?' em vez de lixo da fonte). */
static void copy_text(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!src) src = "";
    for (; *src && i + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        dst[i++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    dst[i] = '\0';
}

/* Divide em até `max` campos por TAB, no lugar. */
static int split_tabs(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == '\t') { *p = '\0'; f[n++] = p + 1; }
    }
    return n;
}

static int state_from_code(const char *c) {
    static const struct { const char *name; int st; } k[] = {
        { "IDLE", MM_ST_IDLE }, { "LOGIN", MM_ST_LOGIN }, { "CATALOG", MM_ST_CATALOG },
        { "STARTING", MM_ST_STARTING }, { "QUEUE", MM_ST_QUEUE },
        { "STREAMING", MM_ST_STREAMING }, { "RECONNECTING", MM_ST_RECONNECTING },
        { "ERROR", MM_ST_ERROR },
    };
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (strcmp(c, k[i].name) == 0) return k[i].st;
    return MM_ST_UNKNOWN;
}

/* Catálogo novo chegou inteiro: troca, e mantém o cursor no mesmo jogo se
 * ele ainda existe (Y = atualizar não deve jogar o cursor pro topo). */
static void commit_catalog(menu_model_t *m) {
    char keep[MM_ID_LEN] = "";
    if (m->cursor >= 0 && m->cursor < m->ngames) copy_text(keep, sizeof(keep), m->games[m->cursor].id);
    memcpy(m->games, m->pend, sizeof(mm_game_t) * (size_t)m->npend);
    m->ngames = m->npend;
    m->npend = 0;
    m->receiving = 0;
    m->cursor = 0;
    m->top = 0;
    if (keep[0]) {
        for (int i = 0; i < m->ngames; i++)
            if (strcmp(m->games[i].id, keep) == 0) { m->cursor = i; break; }
    }
    m->catalogs++;
}

int mm_handle_line(menu_model_t *m, char *line) {
    char *f[5];
    if (!line[0]) return MM_EV_IGNORED;
    int n = split_tabs(line, f, 5);
    if (strcmp(f[0], "STATE") == 0 && n >= 2) {
        copy_text(m->state_code, sizeof(m->state_code), f[1]);
        copy_text(m->state_text, sizeof(m->state_text), n >= 3 ? f[2] : "");
        m->state = state_from_code(f[1]);
        if (m->state != MM_ST_LOGIN) m->have_login = 0;
        m->changes++;
        return MM_EV_STATE;
    }
    if (strcmp(f[0], "LOGIN") == 0 && n >= 3) {
        copy_text(m->login_url, sizeof(m->login_url), f[1]);
        copy_text(m->login_code, sizeof(m->login_code), f[2]);
        m->have_login = 1;
        m->state = MM_ST_LOGIN;
        copy_text(m->state_code, sizeof(m->state_code), "LOGIN");
        m->changes++;
        return MM_EV_LOGIN;
    }
    if (strcmp(f[0], "GAME") == 0 && n >= 3) {
        if (!m->receiving) { m->receiving = 1; m->npend = 0; }
        if (m->npend < MM_MAX_GAMES) {
            mm_game_t *g = &m->pend[m->npend++];
            copy_text(g->id, sizeof(g->id), f[1]);
            copy_text(g->name, sizeof(g->name), f[2]);
            g->recent = (n >= 4 && f[3][0] == '1');
        }
        return MM_EV_GAME;
    }
    if (strcmp(f[0], "END") == 0) {
        /* END sem nenhum GAME = catálogo vazio (válido). */
        if (!m->receiving) m->npend = 0;
        commit_catalog(m);
        m->changes++;
        return MM_EV_CATALOG;
    }
    if (strcmp(f[0], "PONG") == 0) return MM_EV_PONG;
    return MM_EV_IGNORED;
}

const mm_game_t *mm_current(const menu_model_t *m) {
    if (m->cursor < 0 || m->cursor >= m->ngames) return 0;
    return &m->games[m->cursor];
}

void mm_fix_top(menu_model_t *m, int vis) {
    if (vis < 1) vis = 1;
    if (m->cursor >= m->ngames) m->cursor = m->ngames - 1;
    if (m->cursor < 0) m->cursor = 0;
    if (m->cursor < m->top) m->top = m->cursor;
    if (m->cursor >= m->top + vis) m->top = m->cursor - vis + 1;
    if (m->top > m->ngames - vis) m->top = m->ngames - vis;
    if (m->top < 0) m->top = 0;
}

void mm_move(menu_model_t *m, int delta, int vis) {
    if (m->ngames <= 0) return;
    m->cursor += delta;
    mm_fix_top(m, vis);
}

void mm_page(menu_model_t *m, int dir, int vis) {
    mm_move(m, dir * (vis > 1 ? vis : 1), vis);
}

/* Grupo pro LB/RB: os recentes (que vêm primeiro) são um grupo só; o resto
 * agrupa pela inicial, e o que não começa com letra vai pro '#'. */
static int letter_key(const mm_game_t *g) {
    if (g->recent) return 1;
    unsigned char c = (unsigned char)g->name[0];
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    return (c >= 'A' && c <= 'Z') ? c : '#';
}

void mm_letter(menu_model_t *m, int dir, int vis) {
    if (m->ngames <= 0) return;
    int i = m->cursor;
    int k = letter_key(&m->games[i]);
    if (dir > 0) {
        while (i < m->ngames && letter_key(&m->games[i]) == k) i++;
        if (i >= m->ngames) return;            /* já no último grupo */
    } else {
        if (i > 0 && letter_key(&m->games[i - 1]) == k) {
            /* meio do grupo: volta pro começo dele */
            while (i > 0 && letter_key(&m->games[i - 1]) == k) i--;
        } else if (i > 0) {
            /* começo do grupo: vai pro começo do anterior */
            i--;
            int k2 = letter_key(&m->games[i]);
            while (i > 0 && letter_key(&m->games[i - 1]) == k2) i--;
        }
    }
    m->cursor = i;
    mm_fix_top(m, vis);
}

#else
typedef int xcloud360_menu_model_disabled;   /* MENU=0: unidade vazia */
#endif /* XCLOUD360_MENU */
