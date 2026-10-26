/*
 * XCloud360 — XUI <- menu_model: monta o xui_home_data_t (fileiras de
 * id+nome) e o diálogo de confirmação a partir do modelo. C puro (host e
 * Xbox). As imagens e a sessão aberta vêm de quem chama (catálogo / main).
 */
#ifndef XCLOUD360_XUI_MM_H
#define XCLOUD360_XUI_MM_H

#include "xui.h"
#include "../menu_model.h"

typedef struct {
    xui_row_t  rows[MM_MAX_ROWS];
    xui_item_t items[MM_MAX_ROWS][MM_MAX_GAMES];
    unsigned   built_gen, built_cat;
    int        built;
    xui_confirm_t confirm;
} xui_mm_t;

/* Atualiza d->rows/nrows/rows_gen/row/col a partir de m (refaz os itens só
 * quando as fileiras mudaram). flags(u, id) dá XUI_IT_GAMEPASS etc. (pode
 * ser NULL). Não mexe em cover/hero/u/img_gen/session/hint/notice. */
void xui_mm_sync(xui_mm_t *a, const menu_model_t *m, xui_home_data_t *d,
                 unsigned (*flags)(void *u, const char *id), void *fu);
/* Diálogo aberto no modelo -> xui_confirm_t (NULL = fechado). */
const xui_confirm_t *xui_mm_confirm(xui_mm_t *a, const menu_model_t *m);

#endif
