/*
 * XCloud360 — modelo do menu de jogos (v0.16). Ver menu_model.h.
 */
#include "menu_model.h"

#include <string.h>

void mm_init(menu_model_t *m) {
    memset(m, 0, sizeof(*m));
    m->hcol = MM_HB_REBOOT;   /* v0.20.1: Atualizar fica à esquerda */
}


/* Copia só ASCII imprimível (ids e textos de STATE; um byte fora disso
 * vira '?'). */
static void copy_text(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!src) src = "";
    for (; *src && i + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        dst[i++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    dst[i] = '\0';
}

/* v0.20: nome de jogo mantém UTF-8 válido (o menu novo desenha acentos);
 * byte inválido vira '?' e o corte nunca parte uma sequência ao meio. */
static void copy_name(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p && i + 1 < cap) {
        unsigned c = *p;
        int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        int ok = n > 0 && !(n == 1 && c < 32);
        for (int k = 1; ok && k < n; k++) if ((p[k] & 0xC0) != 0x80) ok = 0;
        if (!ok) { dst[i++] = '?'; p++; continue; }
        if (i + (size_t)n >= cap) break;
        for (int k = 0; k < n; k++) dst[i++] = (char)p[k];
        p += n;
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
    if (keep[0]) {
        for (int i = 0; i < m->ngames; i++)
            if (strcmp(m->games[i].id, keep) == 0) { m->cursor = i; break; }
    }
    m->catalogs++;
    mm_rows_default(m);
}

int mm_handle_line(menu_model_t *m, char *line) {
    char *f[5];
    if (!line[0]) return MM_EV_IGNORED;
    int n = split_tabs(line, f, 5);
    if (strcmp(f[0], "STATE") == 0 && n >= 2) {
        copy_text(m->state_code, sizeof(m->state_code), f[1]);
        copy_text(m->state_text, sizeof(m->state_text), n >= 3 ? f[2] : "");
        m->state = state_from_code(f[1]);
        m->changes++;
        return MM_EV_STATE;
    }
    if (strcmp(f[0], "GAME") == 0 && n >= 3) {
        if (!m->receiving) { m->receiving = 1; m->npend = 0; }
        if (m->npend < MM_MAX_GAMES) {
            mm_game_t *g = &m->pend[m->npend++];
            copy_text(g->id, sizeof(g->id), f[1]);
            copy_name(g->name, sizeof(g->name), f[2]);
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
    return MM_EV_IGNORED;
}

const mm_game_t *mm_current(const menu_model_t *m) {
    if (m->cursor < 0 || m->cursor >= m->ngames) return 0;
    return &m->games[m->cursor];
}

/* Encerra a sessão (se houver) e só então chama o SMC. O timeout vale a
 * partir do pedido: um DELETE preso num HTTPS em voo (até ~20 s) não pode
 * segurar o botão de desligar. */
int mm_power_run(const mm_power_ops_t *o, int action, unsigned timeout_ms) {
    if (action != MM_PWR_REBOOT && action != MM_PWR_SHUTDOWN) return -1;
    int r = MM_PWR_NO_SESSION;
    if (o->session_open(o->u)) {
        o->show(o->u, "Encerrando sessao...", "Aguarde, o xCloud esta sendo avisado.");
        o->session_stop(o->u);
        unsigned long long t0 = o->now_us(o->u);
        r = MM_PWR_TIMEOUT;
        for (;;) {
            if (o->session_done(o->u)) { r = MM_PWR_CLEAN; break; }
            if (o->now_us(o->u) - t0 >= (unsigned long long)timeout_ms * 1000ULL) break;
            o->wait(o->u);
        }
    }
    o->show(o->u, action == MM_PWR_REBOOT ? "Reiniciando..." : "Desligando...",
            r == MM_PWR_TIMEOUT ? "A sessao nao respondeu a tempo - seguindo mesmo assim." : "");
    if (action == MM_PWR_REBOOT) o->reboot(o->u);
    else o->shutdown(o->u);
    return r;
}

/* ---- v0.20: fileiras + botões do topo (menu novo, source/ui) ---- */

void mm_rows_clear(menu_model_t *m) {
    m->nrows = 0;
    m->rows_gen++;
    m->changes++;
}

int mm_rows_add(menu_model_t *m, const char *title) {
    if (m->nrows >= MM_MAX_ROWS) return -1;
    mm_row_t *r = &m->rows[m->nrows];
    copy_name(r->title, sizeof(r->title), title);
    r->n = 0;
    r->col = 0;
    m->rows_gen++;
    return m->nrows++;
}

int mm_rows_push(menu_model_t *m, int r, int game) {
    if (r < 0 || r >= m->nrows || game < 0 || game >= m->ngames) return 0;
    mm_row_t *w = &m->rows[r];
    if (w->n >= MM_MAX_GAMES) return 0;
    w->idx[w->n++] = (short)game;
    return 1;
}

int mm_rows_push_id(menu_model_t *m, int r, const char *id) {
    for (int i = 0; i < m->ngames; i++)
        if (strcmp(m->games[i].id, id) == 0) return mm_rows_push(m, r, i);
    return 0;
}

/* cursor (índice em games) -> (row, col): fica na fileira atual se o jogo
 * estiver nela; senão a primeira que o tiver; senão (0, 0). */
void mm_rows_done(menu_model_t *m) {
    int want = m->cursor, best_r = -1, best_c = 0;
    mm_rows_fix(m);                         /* tira vazias (ajusta row) */
    m->cursor = want;
    if (m->row == MM_ROW_HEADER) return;
    for (int pass = 0; pass < 2 && best_r < 0; pass++) {
        for (int r = 0; r < m->nrows && best_r < 0; r++) {
            if (pass == 0 && r != m->row) continue;
            for (int c = 0; c < m->rows[r].n; c++)
                if (m->rows[r].idx[c] == want) { best_r = r; best_c = c; break; }
        }
    }
    if (best_r < 0) { best_r = 0; best_c = 0; }
    m->row = best_r;
    if (best_r < m->nrows) m->rows[best_r].col = best_c;
    mm_rows_fix(m);
}

void mm_rows_default(menu_model_t *m) {
    int keep_row = m->row;
    m->nrows = 0;
    int any_recent = 0;
    for (int i = 0; i < m->ngames; i++) if (m->games[i].recent) { any_recent = 1; break; }
    if (any_recent) {
        int r = mm_rows_add(m, "Voltar a jogar");
        for (int i = 0; i < m->ngames; i++) if (m->games[i].recent) mm_rows_push(m, r, i);
    }
    int r = mm_rows_add(m, "Todos os jogos");
    for (int i = 0; i < m->ngames; i++) mm_rows_push(m, r, i);
    m->row = keep_row;
    mm_rows_done(m);
}

void mm_rows_fix(menu_model_t *m) {
    /* tira fileiras vazias */
    int w = 0;
    for (int r = 0; r < m->nrows; r++) {
        if (m->rows[r].n <= 0) {
            if (m->row > w) m->row--;
            continue;
        }
        if (w != r) m->rows[w] = m->rows[r];
        w++;
    }
    m->nrows = w;
    if (m->row != MM_ROW_HEADER) {
        if (m->row >= m->nrows) m->row = m->nrows > 0 ? m->nrows - 1 : 0;
        if (m->row < 0) m->row = m->nrows > 0 ? 0 : MM_ROW_HEADER;
    }
    if (m->nrows == 0 && m->row != MM_ROW_HEADER) m->row = 0;
    for (int r = 0; r < m->nrows; r++) {
        mm_row_t *x = &m->rows[r];
        if (x->col >= x->n) x->col = x->n - 1;
        if (x->col < 0) x->col = 0;
    }
    if (m->hcol < (m->settings_on ? MM_HB_SETTINGS : MM_HB_UPDATE)) m->hcol = m->settings_on ? MM_HB_SETTINGS : MM_HB_UPDATE;
    if (m->hcol >= MM_HB_COUNT) m->hcol = MM_HB_COUNT - 1;
    if (m->row >= 0 && m->row < m->nrows) m->cursor = m->rows[m->row].idx[m->rows[m->row].col];
    m->changes++;
}

const mm_game_t *mm_focus_game(const menu_model_t *m) {
    if (m->row < 0 || m->row >= m->nrows) return 0;
    const mm_row_t *r = &m->rows[m->row];
    if (r->col < 0 || r->col >= r->n) return 0;
    int g = r->idx[r->col];
    return (g >= 0 && g < m->ngames) ? &m->games[g] : 0;
}

void mm_nav(menu_model_t *m, int dx, int dy) {
    if (m->confirm) {                       /* diálogo: só esquerda/direita */
        if (dx < 0) m->confirm_yes = 0;
        if (dx > 0) m->confirm_yes = 1;
        m->changes++;
        return;
    }
    if (m->row == MM_ROW_HEADER) {
        if (dy > 0 && m->nrows > 0) { m->row = 0; mm_rows_fix(m); return; }
        m->hcol += dx;
        mm_rows_fix(m);
        return;
    }
    if (dy < 0 && m->row == 0) {            /* sobe além da 1ª fileira */
        m->row = MM_ROW_HEADER;
        m->changes++;
        return;
    }
    if (dy) {
        m->row += dy;                       /* a coluna de cada fileira fica lembrada */
        if (m->row < 0) m->row = 0;
        if (m->row >= m->nrows) m->row = m->nrows > 0 ? m->nrows - 1 : 0;
    }
    if (dx && m->row >= 0 && m->row < m->nrows) m->rows[m->row].col += dx;   /* não dá a volta */
    mm_rows_fix(m);
}

void mm_nav_page(menu_model_t *m, int dir, int vis) {
    if (m->confirm || m->row < 0) return;
    mm_nav(m, dir * (vis > 1 ? vis : 1), 0);
}

int mm_press_a(menu_model_t *m) {
    if (m->confirm) {
        int act = m->confirm_yes ? (m->confirm == MM_PWR_REBOOT ? MM_A_REBOOT : MM_A_SHUTDOWN) : MM_A_NONE;
        m->confirm = 0;
        m->confirm_yes = 0;
        m->changes++;
        return act;
    }
    if (m->row == MM_ROW_HEADER) {
        if (m->hcol == MM_HB_UPDATE) return MM_A_UPDATE;   /* v0.20.1: tela própria */
        if (m->hcol == MM_HB_SETTINGS) return m->settings_on ? MM_A_SETTINGS : MM_A_NONE;   /* v0.20.3 */
        m->confirm = m->hcol == MM_HB_REBOOT ? MM_PWR_REBOOT : MM_PWR_SHUTDOWN;
        m->confirm_yes = 0;                 /* A sem querer não desliga */
        m->changes++;
        return MM_A_CONFIRM;
    }
    return mm_focus_game(m) ? MM_A_PLAY : MM_A_NONE;
}

int mm_press_b(menu_model_t *m) {
    if (m->confirm) { m->confirm = 0; m->confirm_yes = 0; m->changes++; return 1; }
    if (m->row == MM_ROW_HEADER && m->nrows > 0) { m->row = 0; mm_rows_fix(m); return 1; }
    return 0;
}
