/* XCloud360 — XUI <- catálogo/capas. Ver xui_cat.h. */
#ifdef XCLOUD360_LOGIN   /* o catálogo só existe com XBOX_LOGIN=1 */

#include "xui_cat.h"

#include <stdio.h>
#include <string.h>

static unsigned hstr(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void hash_put(xui_cat_t *c, const xcc_item_t *it) {
    unsigned h = hstr(it->id) & (XUI_CAT_HASH - 1);
    for (int k = 0; k < XUI_CAT_HASH; k++, h = (h + 1) & (XUI_CAT_HASH - 1)) {
        if (!c->hitem[h]) { c->hitem[h] = it; return; }
        if (!strcmp(c->hitem[h]->id, it->id)) return;   /* já tem */
    }
}

static const xcc_item_t *hash_get(const xui_cat_t *c, const char *id) {
    unsigned h = hstr(id) & (XUI_CAT_HASH - 1);
    for (int k = 0; k < XUI_CAT_HASH; k++, h = (h + 1) & (XUI_CAT_HASH - 1)) {
        const xcc_item_t *it = c->hitem[h];
        if (!it) return 0;
        if (!strcmp(it->id, id)) return it;
    }
    return 0;
}

void xui_cat_init(xui_cat_t *c) {
    memset(c, 0, sizeof(*c));
}

const char *xui_cat_product(const xui_cat_t *c, const char *id) {
    const xcc_item_t *it = id ? hash_get(c, id) : 0;
    return it && it->product[0] ? it->product : 0;
}

int xui_cat_sync(xui_cat_t *c, menu_model_t *m, xui_home_data_t *d) {
    d->img_gen = xcc_seq();
    if (!xcc_rows_copy(&c->rows)) return 0;
    memset(c->hitem, 0, sizeof(c->hitem));
    for (int r = 0; r < XCC_NROWS; r++)
        for (int i = 0; i < c->rows.row[r].n; i++) hash_put(c, &c->rows.row[r].items[i]);
    /* fileiras do modelo: na ordem do catálogo, só o que o modelo conhece */
    mm_rows_clear(m);
    for (int r = 0; r < XCC_NROWS; r++) {
        const xcc_row_t *cr = &c->rows.row[r];
        if (cr->n <= 0) continue;
        int k = mm_rows_add(m, cr->title);
        if (k < 0) break;
        for (int i = 0; i < cr->n; i++) mm_rows_push_id(m, k, cr->items[i].id);
    }
    mm_rows_done(m);
    if (m->nrows == 0) mm_rows_default(m);   /* catálogo sem fileiras ainda: a lista do modelo */
    c->want_sig = 0;
    return 1;
}

static const xui_image_t *wrap(xui_cat_t *c, const xcc_image_t *x) {
    if (!x || !x->px || x->w <= 0 || x->h <= 0) return 0;
    xui_image_t *w = &c->ring[c->ring_i++ & 7];
    w->px = x->px;
    w->w = x->w;
    w->h = x->h;
    w->stride = x->w;
    w->fmt = XUI_FMT_UIRGB;
    return w;
}

static const xui_image_t *p_cover(void *u, const char *id) {
    xui_cat_t *c = (xui_cat_t *)u;
    const char *p = xui_cat_product(c, id);
    return p ? wrap(c, xcc_cover_get(p)) : 0;
}

static const xui_image_t *p_hero(void *u, const char *id) {
    xui_cat_t *c = (xui_cat_t *)u;
    const char *p = xui_cat_product(c, id);
    return p ? wrap(c, xcc_hero_get(p)) : 0;
}

void xui_cat_bind(xui_cat_t *c, xui_home_data_t *d) {
    d->cover = p_cover;
    d->hero = p_hero;
    d->u = c;
}

unsigned xui_cat_flags(void *u, const char *id) {
    const xcc_item_t *it = hash_get((const xui_cat_t *)u, id);
    return it && (it->flags & XA_F_SUB) ? XUI_IT_GAMEPASS : 0;
}

void xui_cat_want(xui_cat_t *c, const xui_view_t *v, const xui_home_data_t *d) {
    const xui_item_t *vis[64];
    int n = xui_home_visible(v, d, vis, 64), k = 0;
    unsigned sig = 2166136261u;
    for (int i = 0; i < n; i++) {
        const char *p = xui_cat_product(c, vis[i]->id);
        if (!p) continue;
        snprintf(c->want_buf[k], sizeof(c->want_buf[k]), "%s", p);
        c->want_ptr[k] = c->want_buf[k];
        sig = (sig ^ hstr(p)) * 16777619u;
        k++;
    }
    sig ^= (unsigned)k;
    if (sig == c->want_sig) return;
    c->want_sig = sig;
    xcc_want(XCC_COVER, c->want_ptr, k);
    /* hero: o do foco (o primeiro da lista, se o foco está numa fileira) */
    if (k > 0 && d->row >= 0) xcc_want(XCC_HERO, c->want_ptr, 1);
}

#else
typedef int xcloud360_xui_cat_disabled;
#endif
