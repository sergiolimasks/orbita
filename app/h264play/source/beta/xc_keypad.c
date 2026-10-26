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
