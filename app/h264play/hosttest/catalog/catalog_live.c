/*
 * XCloud360 — teste AO VIVO (host) da camada de capas com o NOSSO HTTPS
 * (https.c + mbedTLS + raízes embutidas, keep-alive por host), só endpoints
 * públicos sem token: catalog.gamepass.com (SIGL v3 BR), displaycatalog
 * (Images) e store-images (capas/hero redimensionadas). O catálogo da conta
 * é o sintético das fixtures (nunca falamos com o GSSV aqui).
 * uso: catalog_live <fixtures> <store>
 */
#include "../../source/catalog/xc_catalog.h"
#include "../../source/login/https.h"
#include "../../source/login/xbox_auth.h"
#include "../../source/login/xc_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *dir, const char *rel) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    setenv("XC_STORE", argv[2], 1);
    int fails = 0;
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    if (https_init(&e) != 0) { printf("FAIL https_init %s\n", e.msg); return 1; }
    char *tj = slurp(argv[1], "titles_synthetic.json"), *dc = slurp(argv[1], "displaycatalog_BR.json");
    xa_title_t *t = NULL;
    int n = 0;
    if (!tj || !dc || xa_parse_titles(tj, &t, &n) != 0) { printf("FAIL fixtures\n"); return 1; }
    xa_parse_names(dc, t, n);
    n = xa_finalize(t, n);
    xcc_config_t c;
    xcc_config_defaults(&c);
    c.bg_pace_ms = 0;
    xcc_init(&c);
    xcc_hook_start();
    xcc_hook_titles(t, n);
    uint64_t t0 = xcn_now_ms();
    xcc_worker_step();   /* SIGLs */
    static xcc_rows_t r;
    xcc_rows_copy(&r);
    printf("%s SIGL ao vivo: populares %d, novos %d (%ums)\n",
           r.row[XCC_ROW_POPULAR].n > 10 && r.row[XCC_ROW_NEW].n > 3 ? "PASS" : "FAIL",
           r.row[XCC_ROW_POPULAR].n, r.row[XCC_ROW_NEW].n, (unsigned)(xcn_now_ms() - t0));
    if (r.row[XCC_ROW_POPULAR].n <= 10) fails++;
    const char *v[4];
    int nv = r.row[XCC_ROW_POPULAR].n < 4 ? r.row[XCC_ROW_POPULAR].n : 4;
    for (int i = 0; i < nv; i++) v[i] = r.row[XCC_ROW_POPULAR].items[i].product;
    xcc_want(XCC_COVER, v, nv);
    xcc_want(XCC_HERO, v, 1);
    t0 = xcn_now_ms();
    int done = 0;
    while (!done && xcn_now_ms() - t0 < 60000) {
        if (!xcc_worker_step()) xcn_sleep_ms(5);
        xcc_poll();
        done = nv > 0 && xcc_hero_get(v[0]) != NULL;
        for (int i = 0; i < nv; i++) done = done && xcc_cover_get(v[i]) != NULL;
    }
    unsigned ms = (unsigned)(xcn_now_ms() - t0);
    for (int i = 0; i < nv; i++) {
        const xcc_image_t *im = xcc_cover_get(v[i]);
        printf("%s capa %-12s %-40s %dx%d\n", im && im->w == 176 && im->h == 264 ? "PASS" : "FAIL", v[i],
               r.row[XCC_ROW_POPULAR].items[i].name, im ? im->w : 0, im ? im->h : 0);
        if (!im) fails++;
    }
    const xcc_image_t *h = nv ? xcc_hero_get(v[0]) : NULL;
    printf("%s hero %s %dx%d\n", h && h->w == XCC_HERO_W && h->h == XCC_HERO_H ? "PASS" : "FAIL", nv ? v[0] : "-", h ? h->w : 0, h ? h->h : 0);
    if (!h) fails++;
    xcc_status_t st;
    xcc_status(&st);
    printf("%s %d capas + 1 hero em %ums (displaycatalog + store-images, keep-alive); downloads %u, erros %u%s%s\n",
           done ? "PASS" : "FAIL", nv, ms, st.downloads, st.net_errors, st.last_err[0] ? ", ultimo: " : "", st.last_err);
    if (!done) fails++;
    xcc_worker_finish();
    xcc_shutdown();
    free(t); free(tj); free(dc);
    printf("%s\n", fails ? "CATALOG LIVE FAILED" : "CATALOG LIVE PASSED");
    return fails ? 1 : 0;
}
