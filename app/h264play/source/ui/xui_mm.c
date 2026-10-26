/* XCloud360 — XUI <- menu_model. Ver xui_mm.h. */
#include "xui_mm.h"

void xui_mm_sync(xui_mm_t *a, const menu_model_t *m, xui_home_data_t *d,
                 unsigned (*flags)(void *u, const char *id), void *fu) {
    if (!a->built || a->built_gen != m->rows_gen || a->built_cat != m->catalogs) {
        for (int r = 0; r < m->nrows; r++) {
            const mm_row_t *mr = &m->rows[r];
            for (int i = 0; i < mr->n; i++) {
                const mm_game_t *g = &m->games[mr->idx[i]];
                xui_item_t *it = &a->items[r][i];
                it->id = g->id;
                it->name = g->name;
                it->game = mr->idx[i];
                it->flags = flags ? flags(fu, g->id) : 0;
            }
            a->rows[r].title = mr->title;
            a->rows[r].items = a->items[r];
            a->rows[r].n = mr->n;
        }
        a->built = 1;
        a->built_gen = m->rows_gen;
        a->built_cat = m->catalogs;
        d->rows_gen++;
    }
    /* contagem pode mudar sem rows_gen (mm_rows_fix tirou vazias) */
    for (int r = 0; r < m->nrows; r++) a->rows[r].n = m->rows[r].n;
    d->rows = a->rows;
    d->nrows = m->nrows;
    d->row = m->row;
    d->col = m->row == MM_ROW_HEADER ? m->hcol : (m->row < m->nrows ? m->rows[m->row].col : 0);
}

const xui_confirm_t *xui_mm_confirm(xui_mm_t *a, const menu_model_t *m) {
    if (!m->confirm) return 0;
    int reboot = m->confirm == MM_PWR_REBOOT;
    a->confirm.title = reboot ? "Reiniciar o Xbox?" : "Desligar o Xbox?";
    a->confirm.text = "Se houver uma sessão na nuvem aberta, ela é encerrada antes "
                      "(até 3 s).";
    a->confirm.yes = reboot ? "Reiniciar" : "Desligar";
    a->confirm.no = "Cancelar";
    a->confirm.focus = m->confirm_yes;
    return &a->confirm;
}
