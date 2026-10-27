/* XCloud360 / orbita v0.20.3 — teclado da chave beta. Ver xc_keypad.h. */
#include "xc_keypad.h"
#include "xc_beta.h"

#include <string.h>

static const char ALPHA[] = XCB_ALPHABET;   /* 31 */

void xkp_init(xkp_t *k) {
    memset(k, 0, sizeof(*k));
}

void xkp_clear(xkp_t *k) {
    memset(k->raw, 0, sizeof(k->raw));
    k->n = 0;
    k->row = k->col = 0;
    k->changes++;
}

int xkp_row_len(int row) {
    if (row == XKP_ROW_ACT) return XKP_NACT;
    if (row < 0 || row >= XKP_ROWS) return 0;
    int left = (int)(sizeof(ALPHA) - 1) - row * XKP_COLS;
    return left > XKP_COLS ? XKP_COLS : (left > 0 ? left : 0);
}

char xkp_char_at(int row, int col) {
    if (row < 0 || row >= XKP_ROWS || col < 0 || col >= xkp_row_len(row)) return 0;
    return ALPHA[row * XKP_COLS + col];
}

/* coluna da grade <-> ação (Apagar = cols 0-2, Pendrive = 3-5, Confirmar = 6-7) */
static int act_of_col(int c) { return c <= 2 ? XKP_ACT_DEL : c <= 5 ? XKP_ACT_FILE : XKP_ACT_OK; }
static int col_of_act(int a) { return a == XKP_ACT_DEL ? 1 : a == XKP_ACT_FILE ? 4 : 6; }

void xkp_nav(xkp_t *k, int dx, int dy) {
    if (dy > 0 && k->row < XKP_ROW_ACT) {
        k->row++;
        if (k->row == XKP_ROW_ACT) k->col = act_of_col(k->col);
    } else if (dy < 0 && k->row > 0) {
        if (k->row == XKP_ROW_ACT) k->col = col_of_act(k->col);
        k->row--;
    }
    k->col += dx;   /* para nas pontas (não dá a volta) */
    int n = xkp_row_len(k->row);
    if (k->col >= n) k->col = n - 1;
    if (k->col < 0) k->col = 0;
    k->changes++;
}

int xkp_type(xkp_t *k, char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (!ch || !strchr(ALPHA, ch) || k->n >= XKP_LEN) return 0;
    k->raw[k->n++] = ch;
    k->raw[k->n] = 0;
    if (k->n == XKP_LEN) { k->row = XKP_ROW_ACT; k->col = XKP_ACT_OK; }   /* pronto: foco no Confirmar */
    k->changes++;
    return 1;
}

int xkp_backspace(xkp_t *k) {
    if (k->n <= 0) return 0;
    k->raw[--k->n] = 0;
    k->changes++;
    return 1;
}

int xkp_complete(const xkp_t *k) { return k->n == XKP_LEN; }

int xkp_press(xkp_t *k) {
    if (k->row == XKP_ROW_ACT) {
        if (k->col == XKP_ACT_DEL) return xkp_backspace(k) ? XKP_DELETED : XKP_NONE;
        if (k->col == XKP_ACT_FILE) return XKP_FROM_FILE;
        return xkp_complete(k) ? XKP_CONFIRM : XKP_NONE;
    }
    return xkp_type(k, xkp_char_at(k->row, k->col)) ? XKP_TYPED : XKP_NONE;
}

void xkp_text(const xkp_t *k, char *out) {
    int o = 0;
    for (int i = 0; i < k->n; i++) {
        if (i == 5) out[o++] = '-';
        out[o++] = k->raw[i];
    }
    if (k->n == 5) out[o++] = '-';   /* hífen aparece assim que fecha o 1º bloco */
    out[o] = 0;
}

/* ================================================================
 * v0.21.0: teclado de texto (senha do Wi-Fi). Ver xc_keypad.h.
 * ================================================================ */
static const char *const XTK_ROWS_TXT[XTK_NPAGE][XTK_ROWS] = {
    { "1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm-_." },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM-_." },
    { "1234567890", "!\"#$%&'()*", "+,/:;<=>?[", "\\]^`{|}~-_" },
};

void xtk_init(xtk_t *k, int minlen, int maxlen) {
    memset(k, 0, sizeof(*k));
    k->minlen = minlen;
    k->maxlen = maxlen > XTK_MAX ? XTK_MAX : maxlen;
}

static void xtk_zero(volatile char *p, size_t n) { while (n--) *p++ = 0; }

void xtk_clear(xtk_t *k) {
    xtk_zero(k->text, sizeof(k->text));
    k->n = 0;
    k->page = XTK_LOWER;
    k->row = k->col = 0;
    k->show = 0;
    k->changes++;
}

void xtk_wipe(xtk_t *k) {
    int mn = k->minlen, mx = k->maxlen;
    xtk_zero((volatile char *)k, sizeof(*k));
    k->minlen = mn;
    k->maxlen = mx;
}

int xtk_row_len(int row) {
    if (row >= 0 && row < XTK_ROWS) return XTK_COLS;
    if (row == XTK_ROW_A1) return XTK_NA1;
    if (row == XTK_ROW_A2) return XTK_NA2;
    return 0;
}

char xtk_char_at(int page, int row, int col) {
    if (page < 0 || page >= XTK_NPAGE || row < 0 || row >= XTK_ROWS || col < 0 || col >= XTK_COLS) return 0;
    return XTK_ROWS_TXT[page][row][col];
}

/* spans das ações em colunas: A1 = 2,2,3,3; A2 = 5,5 */
void xtk_act_span(int row, int a, int *c0, int *w) {
    static const int s1[XTK_NA1][2] = { { 0, 2 }, { 2, 2 }, { 4, 3 }, { 7, 3 } };
    static const int s2[XTK_NA2][2] = { { 0, 5 }, { 5, 5 } };
    if (row == XTK_ROW_A1 && a >= 0 && a < XTK_NA1) { *c0 = s1[a][0]; *w = s1[a][1]; return; }
    if (row == XTK_ROW_A2 && a >= 0 && a < XTK_NA2) { *c0 = s2[a][0]; *w = s2[a][1]; return; }
    *c0 = 0; *w = 1;
}

static int act_at(int row, int gcol) {
    int n = xtk_row_len(row);
    for (int a = 0; a < n; a++) {
        int c0, w;
        xtk_act_span(row, a, &c0, &w);
        if (gcol >= c0 && gcol < c0 + w) return a;
    }
    return n - 1;
}

/* coluna da grade que representa o foco atual (pra subir/descer) */
static int grid_col(const xtk_t *k) {
    if (k->row < XTK_ROWS) return k->col;
    int c0, w;
    xtk_act_span(k->row, k->col, &c0, &w);
    return c0 + w / 2;
}

void xtk_nav(xtk_t *k, int dx, int dy) {
    if (dy) {
        int g = grid_col(k);
        int r = k->row + (dy > 0 ? 1 : -1);
        if (r >= 0 && r <= XTK_ROW_A2) {
            k->row = r;
            k->col = r < XTK_ROWS ? g : act_at(r, g);
        }
    }
    k->col += dx;   /* para nas pontas */
    int n = xtk_row_len(k->row);
    if (k->col >= n) k->col = n - 1;
    if (k->col < 0) k->col = 0;
    k->changes++;
}

int xtk_type(xtk_t *k, char ch) {
    if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e || k->n >= k->maxlen) return 0;
    k->text[k->n++] = ch;
    k->text[k->n] = 0;
    /* depois de uma maiúscula com o Shift, volta pras minúsculas (como no celular) */
    if (k->page == XTK_UPPER && ch >= 'A' && ch <= 'Z') k->page = XTK_LOWER;
    k->changes++;
    return 1;
}

int xtk_backspace(xtk_t *k) {
    if (k->n <= 0) return 0;
    k->text[--k->n] = 0;
    k->changes++;
    return 1;
}

int xtk_toggle_case(xtk_t *k) {
    k->page = k->page == XTK_LOWER ? XTK_UPPER : XTK_LOWER;
    k->changes++;
    return XTK_PAGE;
}

int xtk_toggle_show(xtk_t *k) {
    k->show = !k->show;
    k->changes++;
    return XTK_SHOWN;
}

int xtk_ready(const xtk_t *k) { return k->n >= k->minlen && k->n <= k->maxlen; }

int xtk_press(xtk_t *k) {
    if (k->row < XTK_ROWS) return xtk_type(k, xtk_char_at(k->page, k->row, k->col)) ? XTK_TYPED : XTK_NONE;
    if (k->row == XTK_ROW_A1) {
        switch (k->col) {
        case XTK_A_SHIFT: return xtk_toggle_case(k);
        case XTK_A_SYM:
            k->page = k->page == XTK_SYM ? XTK_LOWER : XTK_SYM;
            k->changes++;
            return XTK_PAGE;
        case XTK_A_SPACE: return xtk_type(k, ' ') ? XTK_TYPED : XTK_NONE;
        default: return xtk_backspace(k) ? XTK_DELETED : XTK_NONE;
        }
    }
    if (k->col == XTK_A_SHOW) return xtk_toggle_show(k);
    return xtk_ready(k) ? XTK_CONFIRM : XTK_NONE;
}
