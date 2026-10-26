/*
 * XCloud360 — XUI <- camada de catálogo/capas (source/catalog/xc_catalog.h).
 *
 * Só main thread (as funções xcc_* que usa são as "do main"):
 *   - fileiras do catálogo (xcc_rows_copy) -> fileiras do menu_model,
 *     mantendo o foco no mesmo jogo;
 *   - provedor de imagens do xui_home_data_t: id (titleId) -> bigId ->
 *     xcc_cover_get/xcc_hero_get (pixels UI_RGB, convertidos na escala);
 *   - publica o que está na tela (xcc_want) a partir do xui_home_visible.
 * Os ponteiros de pixels do catálogo valem até o próximo xcc_poll(): o
 * img_gen (= xcc_seq()) muda quando o LRU mexe, e o XUI reescala.
 */
#ifndef XCLOUD360_XUI_CAT_H
#define XCLOUD360_XUI_CAT_H

#include "xui.h"
#include "../menu_model.h"
#include "../catalog/xc_catalog.h"

#define XUI_CAT_HASH 4096   /* > 2 * XCC_ALL_MAX */

typedef struct {
    xcc_rows_t  rows;                    /* cópia do main (~150 KB) */
    const xcc_item_t *hitem[XUI_CAT_HASH];   /* titleId -> item (aberto, linear) */
    xui_image_t ring[8];                 /* embrulhos devolvidos ao XUI */
    int         ring_i;
    unsigned    want_sig;
    char        want_buf[64][24];
    const char *want_ptr[64];
} xui_cat_t;

void     xui_cat_init(xui_cat_t *c);
/* Uma vez por quadro, depois do xcc_poll(): se as fileiras mudaram, refaz
 * as do modelo (só títulos que o modelo tem em games[]) e devolve 1. Põe
 * d->img_gen = xcc_seq(). */
int      xui_cat_sync(xui_cat_t *c, menu_model_t *m, xui_home_data_t *d);
/* Liga o provedor de imagens em d (cover/hero/u). */
void     xui_cat_bind(xui_cat_t *c, xui_home_data_t *d);
/* flags pro xui_mm_sync: XUI_IT_GAMEPASS se o título está numa assinatura. */
unsigned xui_cat_flags(void *u, const char *id);
/* Pede ao catálogo as capas visíveis + o hero do foco (só quando muda). */
void     xui_cat_want(xui_cat_t *c, const xui_view_t *v, const xui_home_data_t *d);
/* bigId do titleId (NULL = não está nas fileiras). */
const char *xui_cat_product(const xui_cat_t *c, const char *id);

#endif
