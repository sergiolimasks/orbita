/*
 * XCloud360 — testes do host da camada de capas (source/catalog), sem rede:
 * respostas gravadas (fixtures/) servidas por um fetch falso.
 *   parse SIGL v2/v3, displaycatalog (preferência Poster/SuperHeroArt, host),
 *   URL de tamanho, nome 8.3, decode JPEG (stb_image) + escala/corte,
 *   fileiras (MRU, populares, novos, todos), cache no "pendrive" (XC_STORE),
 *   fila de prioridade (visíveis antes do fundo), LRU, trailer, falha de
 *   rede / 404 / produto sem imagem, e a thread (start/stop com cancelamento).
 * uso: catalog_unit <pasta fixtures> <pasta temporária>
 */
#define _GNU_SOURCE
#include "../../source/catalog/xc_catalog.h"
#include "../../source/catalog/xcc_port.h"
#include "../../source/login/xbox_auth.h"
#include "../../source/login/xc_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void xccp_test_set_busy(int b);

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *FX;
static char g_tmp[512];

static char *slurp(const char *rel, size_t *len) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", FX, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    b[n] = 0;
    if (len) *len = (size_t)n;
    return b;
}

static int file_exists(const char *store, const char *name) {
    char p[1024];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", store, name);
    return stat(p, &st) == 0 && st.st_size > 0;
}

/* --------------------------------------------------------- fetch falso */
static int g_fail_all = 0, g_img_status = 0, g_slow = 0;
static int g_calls_sigl, g_calls_dc, g_calls_img;
static char g_log[512][64];
static int g_nlog;
static xcc_imgref_t *g_refs;
static int g_nrefs;
static char g_last_dc_ids[512];

static int fake_fetch(const char *host, const char *path, size_t max, int *status, char **body, size_t *len) {
    (void)max;
    if (g_slow) {
        for (int i = 0; i < 300; i++) {   /* 3 s, mas larga se cancelarem */
            if (xcn_cancelled()) return -1;
            usleep(10000);
        }
    }
    if (g_fail_all) return -1;
    *status = 200;
    if (!strcmp(host, "catalog.gamepass.com")) {
        g_calls_sigl++;
        const char *f = strstr(path, "6a589fa0") ? "sigl_popular_v3_BR.json" : "sigl_new_v3_BR.json";
        if (strncmp(path, "/sigls/v3?", 10) || !strstr(path, "market=BR") || !strstr(path, "language=pt-BR") ||
            !strstr(path, "platformContext=Cloud%3AXGPUWEB")) { *status = 400; *body = strdup("bad"); *len = 3; return 0; }
        *body = slurp(f, len);
        return *body ? 0 : -1;
    }
    if (!strcmp(host, "displaycatalog.mp.microsoft.com")) {
        g_calls_dc++;
        const char *b = strstr(path, "bigIds="), *e = b ? strchr(b, '&') : NULL;
        if (b && e) snprintf(g_last_dc_ids, sizeof(g_last_dc_ids), "%.*s", (int)(e - b - 7), b + 7);
        if (g_nlog < 512) snprintf(g_log[g_nlog++], 64, "DC");
        *body = slurp("displaycatalog_BR.json", len);
        return *body ? 0 : -1;
    }
    if (!strcmp(host, "store-images.s-microsoft.com")) {
        g_calls_img++;
        const char *q = strchr(path, '?');
        size_t pl = q ? (size_t)(q - path) : strlen(path);
        int hero = strstr(path, "w=1280") != NULL;   /* v0.20.0: hero 1280x720 */
        for (int i = 0; i < g_nrefs; i++) {
            const char *rp = hero ? g_refs[i].hero : g_refs[i].cover;
            if (strlen(rp) != pl || strncmp(rp, path, pl)) continue;
            if (g_nlog < 512) snprintf(g_log[g_nlog++], 64, "%s %s", hero ? "HERO" : "IMG", g_refs[i].product);
            if (g_img_status) { *status = g_img_status; *body = strdup("not found"); *len = 9; return 0; }
            char rel[96];
            snprintf(rel, sizeof(rel), "%s/%s.jpg", hero ? "hero" : "covers", g_refs[i].product);
            *body = slurp(rel, len);
            if (!*body) { *status = 404; *body = strdup("x"); *len = 1; }
            return 0;
        }
        *status = 404;
        *body = strdup("x");
        *len = 1;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------- helpers */

static void set_store(const char *sub) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", g_tmp, sub);
    mkdir(g_tmp, 0700);
    mkdir(p, 0700);
    setenv("XC_STORE", p, 1);
}

static const char *store(void) { return getenv("XC_STORE"); }

static void init_fake(int max_covers) {
    xcc_config_t c;
    xcc_config_defaults(&c);
    c.fetch = fake_fetch;
    c.bg_pace_ms = 0;
    c.max_covers = max_covers;
    c.max_heroes = 2;
    CHECK(xcc_init(&c) == 0);
}

/* catálogo sintético como o lf_run entrega (finalize = jogáveis, por nome) */
static xa_title_t *g_cat;
static int g_ncat;
static void load_catalog(void) {
    char *tj = slurp("titles_synthetic.json", NULL), *dc = slurp("displaycatalog_BR.json", NULL);
    CHECK(tj && dc);
    CHECK(xa_parse_titles(tj, &g_cat, &g_ncat) == 0 && g_ncat == 39);
    CHECK(xa_parse_names(dc, g_cat, g_ncat) == 39);
    g_ncat = xa_finalize(g_cat, g_ncat);
    CHECK(g_ncat == 39);
    free(tj);
    free(dc);
}

static int run_steps(int max) {
    int k = 0;
    while (k < max && xcc_worker_step()) k++;
    return k;
}

static int row_has(const xcc_row_t *r, const char *product) {
    for (int i = 0; i < r->n; i++) if (!strcasecmp(r->items[i].product, product)) return 1;
    return 0;
}

static xcc_rows_t g_rows, g_rows_saved;

/* --------------------------------------------------------------- testes */

static void test_parse(void) {
    char (*ids)[24] = NULL;
    int n = 0;
    char title[64];
    char *s = slurp("sigl_popular_v3_BR.json", NULL);
    CHECK(xcc_parse_sigl(s, &ids, &n, title, sizeof(title)) == 0);
    CHECK(n > 3000);
    CHECK(n > 0 && !strcmp(ids[0], "BT5P2X999VH2"));
    CHECK(!strcmp(title, "Mais populares na nuvem"));
    free(ids); free(s);
    s = slurp("sigl_new_v3_BR.json", NULL);
    CHECK(xcc_parse_sigl(s, &ids, &n, title, sizeof(title)) == 0 && n >= 15 && n <= 60);
    CHECK(!strcmp(title, "Adicionados recentemente"));
    free(ids); free(s);
    s = slurp("sigl_new_v2_BR.json", NULL);
    CHECK(xcc_parse_sigl(s, &ids, &n, NULL, 0) == 0 && n >= 15);
    free(ids); free(s);
    CHECK(xcc_parse_sigl("{\"products\":[{\"id\":\"9nblggh4r315\"},{\"id\":\"bad id\"}]}", &ids, &n, NULL, 0) == 0 &&
          n == 1 && !strcmp(ids[0], "9NBLGGH4R315"));
    free(ids);
    CHECK(xcc_parse_sigl("<html>", &ids, &n, NULL, 0) == -1 && ids == NULL);

    xcc_imgref_t *r = NULL;
    s = slurp("displaycatalog_raw3_BR.json", NULL);
    CHECK(xcc_parse_images(s, &r, &n) == 0 && n == 3);
    for (int i = 0; i < n; i++) {
        CHECK(!strncmp(r[i].cover, "/image/apps.", 12));
        CHECK(!strncmp(r[i].hero, "/image/apps.", 12));
        CHECK(strchr(r[i].cover, '?') == NULL);
    }
    free(r); free(s);
    s = slurp("displaycatalog_BR.json", NULL);
    CHECK(xcc_parse_images(s, &r, &n) == 0 && n == 39);
    int nc = 0;
    for (int i = 0; i < n; i++) nc += r[i].cover[0] != 0;
    CHECK(nc == 39);
    free(r); free(s);
    /* preferência + host */
    const char *pj = "{\"Products\":[{\"ProductId\":\"9abcdefgh123\",\"LocalizedProperties\":[{\"Images\":["
        "{\"ImagePurpose\":\"BoxArt\",\"Uri\":\"//store-images.s-microsoft.com/image/box\"},"
        "{\"ImagePurpose\":\"Poster\",\"Uri\":\"//images-eds-ssl.xboxlive.com/image/eds\"},"
        "{\"ImagePurpose\":\"Poster\",\"Uri\":\"https://store-images.s-microsoft.com/image/poster?x=1\"},"
        "{\"ImagePurpose\":\"TitledHeroArt\",\"Uri\":\"//store-images.s-microsoft.com/image/titled\"},"
        "{\"ImagePurpose\":\"SuperHeroArt\",\"Uri\":\"//store-images.s-microsoft.com/image/super\"}]}]},"
        "{\"ProductId\":\"9NOIMAGES001\",\"LocalizedProperties\":[{\"Images\":["
        "{\"ImagePurpose\":\"Poster\",\"Uri\":\"//images-eds-ssl.xboxlive.com/image/eds\"}]}]}]}";
    CHECK(xcc_parse_images(pj, &r, &n) == 0 && n == 2);
    CHECK(!strcmp(r[0].product, "9ABCDEFGH123"));
    CHECK(!strcmp(r[0].cover, "/image/poster"));
    CHECK(!strcmp(r[0].hero, "/image/super"));
    CHECK(r[1].cover[0] == 0 && r[1].hero[0] == 0);
    free(r);

    char u[256];
    xcc_image_path(u, sizeof(u), "/image/apps.1.2", XCC_COVER);
    CHECK(!strcmp(u, "/image/apps.1.2?w=176&h=264&q=75&mode=crop&format=jpg"));
    xcc_image_path(u, sizeof(u), "/image/apps.1.2", XCC_HERO);
    CHECK(!strcmp(u, "/image/apps.1.2?w=1280&h=720&q=70&mode=crop&format=jpg"));
    char f1[32], f2[32], f3[32];
    xcc_file_name(f1, sizeof(f1), "9NBLGGH4R315", XCC_COVER);
    xcc_file_name(f2, sizeof(f2), "9nblggh4r315", XCC_COVER);
    xcc_file_name(f3, sizeof(f3), "9NBLGGH4R315", XCC_HERO);
    CHECK(strlen(f1) == 19 && !strncmp(f1, "COVERS/", 7) && !strcmp(f1 + 15, ".JPG"));
    CHECK(!strcmp(f1, f2));
    CHECK(!strncmp(f3, "HERO/", 5) && !strcmp(f1 + 7, f3 + 5));
}

static void avg(const uint32_t *px, int n, int *r, int *g, int *b) {
    long long R = 0, G = 0, B = 0;
    for (int i = 0; i < n; i++) { B += px[i] >> 24; G += (px[i] >> 16) & 255; R += (px[i] >> 8) & 255; }
    *r = (int)(R / n); *g = (int)(G / n); *b = (int)(B / n);
}

static void test_decode(void) {
    size_t len = 0;
    char *j = slurp("covers/BT5P2X999VH2.jpg", &len);
    CHECK(j != NULL);
    uint32_t *a = xcc_decode_jpeg((uint8_t *)j, len, XCC_COVER_W, XCC_COVER_H);
    uint32_t *h = xcc_decode_jpeg((uint8_t *)j, len, 88, 132);
    CHECK(a && h);
    if (a && h) {
        int low = 0, diff = 0;
        for (int i = 0; i < XCC_COVER_W * XCC_COVER_H; i++) { low |= (int)(a[i] & 0xFF); diff |= a[i] != a[0]; }
        CHECK(low == 0);       /* formato UI_RGB: byte baixo zero */
        CHECK(diff);           /* não é uma cor só */
        int r1, g1, b1, r2, g2, b2;
        avg(a, XCC_COVER_W * XCC_COVER_H, &r1, &g1, &b1);
        avg(h, 88 * 132, &r2, &g2, &b2);
        CHECK(abs(r1 - r2) < 6 && abs(g1 - g2) < 6 && abs(b1 - b2) < 6);   /* média de área */
    }
    free(a); free(h);
    /* truncado / lixo: sem crash */
    uint32_t *t = xcc_decode_jpeg((uint8_t *)j, len / 3, XCC_COVER_W, XCC_COVER_H);
    free(t);
    CHECK(xcc_decode_jpeg((const uint8_t *)"not a jpeg at all", 17, 10, 10) == NULL);
    CHECK(xcc_decode_jpeg(NULL, 0, 10, 10) == NULL);
    free(j);
    j = slurp("hero/BT5P2X999VH2.jpg", &len);
    a = j ? xcc_decode_jpeg((uint8_t *)j, len, XCC_HERO_W, XCC_HERO_H) : NULL;
    CHECK(a != NULL);
    free(a); free(j);
    /* quadrada 176x176 -> retrato 176x264 (corta no centro) */
    j = slurp("boxart_square_176.jpg", &len);
    a = j ? xcc_decode_jpeg((uint8_t *)j, len, XCC_COVER_W, XCC_COVER_H) : NULL;
    CHECK(a != NULL);
    free(a); free(j);
}

static void test_flow(void) {
    set_store("flow");
    init_fake(6);
    xcc_hook_start();   /* como o lf_run: cache primeiro (vazio) */
    xcc_status_t st;
    xcc_status(&st);
    CHECK(st.loaded_cache == 1);
    CHECK(xcc_rows_copy(&g_rows) == 1);
    for (int r = 0; r < XCC_NROWS; r++) CHECK(g_rows.row[r].n == 0);
    CHECK(!strcmp(g_rows.row[XCC_ROW_RECENT].title, "Voltar a jogar"));
    CHECK(!strcmp(g_rows.row[XCC_ROW_ALL].title, "Todos os jogos"));

    /* MRU (on_cloud) chega antes da lista (on_titles), como no lf_run */
    char *mj = slurp("mru_synthetic.json", NULL);
    xa_title_t *mru = NULL;
    int nm = 0;
    CHECK(xa_parse_titles(mj, &mru, &nm) == 0 && nm == 5);   /* 1 repetido some no parse */
    xcc_set_mru(mru, nm);
    xcc_hook_titles(g_cat, g_ncat);
    CHECK(file_exists(store(), "ROWS.TXT"));
    CHECK(xcc_rows_copy(&g_rows) == 1);
    CHECK(xcc_rows_copy(&g_rows) == 0);   /* nada mudou */
    const xcc_row_t *all = &g_rows.row[XCC_ROW_ALL], *rec = &g_rows.row[XCC_ROW_RECENT];
    CHECK(all->n == 39);
    CHECK(strcasecmp(all->items[0].name, all->items[38].name) < 0);
    /* recentes: na ordem do MRU, sem o que não está no catálogo, sem repetir */
    CHECK(rec->n == 4);
    if (rec->n == 4) {
        CHECK(!strcmp(rec->items[0].id, mru[0].id));
        CHECK(!strcmp(rec->items[1].id, mru[1].id));
        CHECK(!strcmp(rec->items[2].id, mru[3].id));   /* mru[2] = TNOTINCATALOG1 */
        CHECK(!strcmp(rec->items[3].id, mru[4].id));
        CHECK(rec->items[0].name[0] != 0 && rec->items[0].product[0] != 0);
    }
    CHECK(!strcmp(mru[2].id, "TNOTINCATALOG1"));
    free(mru); free(mj);

    /* worker: SIGLs */
    g_calls_sigl = g_calls_dc = g_calls_img = 0;
    CHECK(xcc_worker_step() == 1);
    CHECK(g_calls_sigl == 2);
    CHECK(xcc_rows_copy(&g_rows) == 1);
    const xcc_row_t *pop = &g_rows.row[XCC_ROW_POPULAR], *nw = &g_rows.row[XCC_ROW_NEW];
    char (*pids)[24] = NULL, (*nids)[24] = NULL;
    int np = 0, nn = 0;
    char *s = slurp("sigl_popular_v3_BR.json", NULL);
    xcc_parse_sigl(s, &pids, &np, NULL, 0);
    free(s);
    s = slurp("sigl_new_v3_BR.json", NULL);
    xcc_parse_sigl(s, &nids, &nn, NULL, 0);
    free(s);
    /* populares = catálogo na ordem da SIGL (os 20 primeiros dela estão no catálogo) */
    CHECK(pop->n == 39);
    for (int i = 0; i < 20 && i < pop->n; i++) CHECK(!strcmp(pop->items[i].product, pids[i]));
    int prev = -1, order_ok = 1;
    for (int i = 0; i < pop->n; i++) {
        int k = -1;
        for (int q = 0; q < np; q++) if (!strcmp(pids[q], pop->items[i].product)) { k = q; break; }
        if (k <= prev) order_ok = 0;
        prev = k;
    }
    CHECK(order_ok);
    int expect_new = 0;
    for (int q = 0; q < nn; q++)
        for (int i = 0; i < g_ncat; i++) if (!strcasecmp(g_cat[i].product, nids[q])) { expect_new++; break; }
    CHECK(nw->n == expect_new && expect_new >= 10);
    if (nw->n > 0) CHECK(!strcmp(nw->items[0].product, nids[0]));
    for (int i = 0; i < nw->n; i++) CHECK(row_has(all, nw->items[i].product));
    CHECK(run_steps(1) == 1);   /* grava ROWS.TXT com as SIGLs */

    /* prioridade: 5 visíveis vão antes de qualquer fundo */
    const char *vis[5];
    for (int i = 0; i < 5; i++) vis[i] = pop->items[i].product;
    xcc_want(XCC_COVER, vis, 5);
    g_nlog = 0;
    for (int i = 0; i < 6; i++) CHECK(xcc_worker_step() == 1);   /* 1 lote DC + 5 imagens */
    CHECK(g_nlog >= 6);
    if (g_nlog >= 6) {
        CHECK(!strcmp(g_log[0], "DC"));
        for (int i = 0; i < 5; i++) {
            char e[64];
            snprintf(e, sizeof(e), "IMG %s", vis[i]);
            CHECK(!strcmp(g_log[1 + i], e));
        }
    }
    for (int i = 0; i < 5; i++) CHECK(strstr(g_last_dc_ids, vis[i]) != NULL || g_calls_dc > 1);
    for (int i = 0; i < 5; i++) CHECK(xcc_cover_get(vis[i]) == NULL);   /* ainda na caixa de saída */
    xcc_poll();
    for (int i = 0; i < 5; i++) {
        const xcc_image_t *im = xcc_cover_get(vis[i]);
        CHECK(im && im->w == XCC_COVER_W && im->h == XCC_COVER_H && im->px);
    }
    /* fundo: baixa o resto pro pendrive, sem decodificar */
    int k = run_steps(400);
    CHECK(k < 400);
    xcc_status(&st);
    CHECK(st.n_covers_disk == 39);
    CHECK(st.downloads == 39);
    CHECK(st.decodes == 5);
    CHECK(st.ram_covers == 5);
    CHECK(file_exists(store(), "IMGIDX.TXT"));
    char fn[32];
    xcc_file_name(fn, sizeof(fn), vis[0], XCC_COVER);
    CHECK(file_exists(store(), fn));
    CHECK(xcc_worker_step() == 0);   /* nada a fazer: ocioso */

    /* LRU (6): pedir 10 decodifica só 6 (o que cabe) */
    const char *ten[10];
    for (int i = 0; i < 10; i++) ten[i] = pop->items[10 + i].product;
    xcc_want(XCC_COVER, ten, 10);
    unsigned dl0 = st.downloads;
    for (int it = 0; it < 30; it++) { run_steps(20); xcc_poll(); }
    xcc_status(&st);
    CHECK(st.ram_covers == 6);
    for (int i = 0; i < 6; i++) CHECK(xcc_cover_get(ten[i]) != NULL);
    CHECK(xcc_cover_get(vis[0]) == NULL);   /* despejada */
    CHECK(st.downloads == dl0);             /* tudo veio do pendrive */
    CHECK(st.disk_hits >= 6);
    /* troca o pedido: 3 novas entram, as fora do pedido saem primeiro */
    const char *three[3] = { pop->items[30].product, pop->items[31].product, ten[0] };
    xcc_want(XCC_COVER, three, 3);
    for (int it = 0; it < 10; it++) { run_steps(20); xcc_poll(); }
    for (int i = 0; i < 3; i++) CHECK(xcc_cover_get(three[i]) != NULL);
    xcc_status(&st);
    CHECK(st.ram_covers == 6);

    /* hero do selecionado */
    const char *hv[1] = { "BT5P2X999VH2" };
    xcc_want(XCC_HERO, hv, 1);
    CHECK(xcc_hero_get(hv[0]) == NULL);
    for (int it = 0; it < 5; it++) { run_steps(20); xcc_poll(); }
    const xcc_image_t *he = xcc_hero_get(hv[0]);
    CHECK(he && he->w == XCC_HERO_W && he->h == XCC_HERO_H);
    xcc_file_name(fn, sizeof(fn), hv[0], XCC_HERO);
    CHECK(file_exists(store(), fn));

    /* PLAY -> volta pra frente do "Voltar a jogar" */
    unsigned seq = g_rows.seq;
    xcc_mru_touch(pop->items[7].id);
    CHECK(xcc_rows_copy(&g_rows) == 1 && g_rows.seq != seq);
    CHECK(g_rows.row[XCC_ROW_RECENT].n == 5 && !strcmp(g_rows.row[XCC_ROW_RECENT].items[0].id, g_rows.row[XCC_ROW_POPULAR].items[7].id));
    xcc_mru_touch(g_rows.row[XCC_ROW_RECENT].items[2].id);   /* já estava: só sobe */
    xcc_rows_copy(&g_rows);
    CHECK(g_rows.row[XCC_ROW_RECENT].n == 5);
    run_steps(20);   /* grava ROWS.TXT */
    g_rows_saved = g_rows;
    xcc_worker_finish();
    xcc_shutdown();
    free(pids); free(nids);
}

static void test_cache_offline(void) {
    set_store("flow");   /* mesmo "pendrive" do teste anterior */
    g_fail_all = 1;      /* sem rede nenhuma */
    init_fake(6);
    xcc_hook_start();
    CHECK(xcc_rows_copy(&g_rows) == 1);
    for (int r = 0; r < XCC_NROWS; r++) {
        CHECK(g_rows.row[r].n == g_rows_saved.row[r].n);
        for (int i = 0; i < g_rows.row[r].n && i < g_rows_saved.row[r].n; i++)
            CHECK(!strcmp(g_rows.row[r].items[i].id, g_rows_saved.row[r].items[i].id) &&
                  !strcmp(g_rows.row[r].items[i].name, g_rows_saved.row[r].items[i].name));
    }
    xcc_status_t st;
    xcc_status(&st);
    CHECK(st.n_covers_disk == 39 && st.n_heroes_disk == 1 && st.n_with_urls == 39);
    /* capas do pendrive mesmo sem rede */
    const char *vis[3] = { g_rows.row[XCC_ROW_NEW].items[0].product, g_rows.row[XCC_ROW_NEW].items[1].product,
                           g_rows.row[XCC_ROW_NEW].items[2].product };
    xcc_want(XCC_COVER, vis, 3);
    g_calls_img = 0;
    for (int it = 0; it < 10; it++) { run_steps(10); xcc_poll(); }
    for (int i = 0; i < 3; i++) CHECK(xcc_cover_get(vis[i]) != NULL);
    xcc_status(&st);
    CHECK(st.downloads == 0 && st.disk_hits == 3);
    /* rede fora: tentou as SIGLs, contou erro, e não martela */
    CHECK(st.net_errors >= 1);
    int calls_before = g_calls_sigl + g_calls_dc + g_calls_img, steps = 0;
    for (int i = 0; i < 200; i++) steps += xcc_worker_step();
    CHECK(steps <= 3);
    CHECK(g_calls_sigl + g_calls_dc + g_calls_img == calls_before);   /* fake nem contou: falhou antes */
    /* trailer de outro produto = arquivo inválido -> baixa de novo */
    char fn[32], p[1024];
    const char *victim = g_rows.row[XCC_ROW_NEW].items[3].product;
    xcc_file_name(fn, sizeof(fn), victim, XCC_COVER);
    snprintf(p, sizeof(p), "%s/%s", store(), fn);
    FILE *f = fopen(p, "r+b");
    CHECK(f != NULL);
    if (f) { fseek(f, -28, SEEK_END); fwrite("ZZZZZZZZZZZZ", 1, 12, f); fclose(f); }
    const char *one[1] = { victim };
    xcc_want(XCC_COVER, one, 1);
    xcc_debug_reset_backoff();
    for (int it = 0; it < 5; it++) { run_steps(10); xcc_poll(); }
    CHECK(xcc_cover_get(victim) == NULL);   /* sem rede, não deu */
    g_fail_all = 0;
    xcc_debug_reset_backoff();
    g_nlog = 0;
    for (int it = 0; it < 5; it++) { run_steps(10); xcc_poll(); }
    CHECK(xcc_cover_get(victim) != NULL);
    int redl = 0;
    for (int i = 0; i < g_nlog; i++) if (!strncmp(g_log[i], "IMG ", 4) && !strcmp(g_log[i] + 4, victim)) redl = 1;
    CHECK(redl);
    xcc_worker_finish();
    xcc_shutdown();
}

static void test_failures(void) {
    set_store("fail");
    init_fake(8);
    xcc_hook_start();
    /* catálogo com um produto que o displaycatalog não conhece */
    xa_title_t *t = calloc((size_t)g_ncat + 1, sizeof(*t));
    memcpy(t, g_cat, sizeof(*t) * (size_t)g_ncat);
    snprintf(t[g_ncat].id, sizeof(t[0].id), "TGHOST");
    snprintf(t[g_ncat].product, sizeof(t[0].product), "9ZZZZZZZZZZZ");
    snprintf(t[g_ncat].name, sizeof(t[0].name), "Ghost");
    t[g_ncat].flags = XA_F_SUB | XA_F_INSUB;
    xcc_hook_titles(t, g_ncat + 1);
    free(t);
    const char *v[2] = { "9ZZZZZZZZZZZ", g_cat[0].product };
    xcc_want(XCC_COVER, v, 2);
    g_img_status = 404;
    g_calls_img = 0;
    g_nlog = 0;
    for (int it = 0; it < 6; it++) { run_steps(3); xcc_poll(); }
    CHECK(xcc_cover_get(v[0]) == NULL && xcc_cover_get(v[1]) == NULL);
    int ghost_img = 0, other_img = 0;
    for (int i = 0; i < g_nlog; i++) {
        if (strstr(g_log[i], "9ZZZZZZZZZZZ") && !strncmp(g_log[i], "IMG", 3)) ghost_img++;
        if (!strncmp(g_log[i], "IMG ", 4) && !strcmp(g_log[i] + 4, v[1])) other_img++;
    }
    CHECK(ghost_img == 0);   /* sem imagem: nunca pede */
    CHECK(other_img == 1);   /* 404: uma vez, depois espera o backoff */
    xcc_status_t st;
    xcc_status(&st);
    CHECK(st.n_covers_disk == 0 && st.last_err[0] != 0);
    g_img_status = 0;
    xcc_debug_reset_backoff();
    for (int it = 0; it < 6; it++) { run_steps(3); xcc_poll(); }
    CHECK(xcc_cover_get(v[1]) != NULL);
    xcc_worker_finish();
    xcc_shutdown();
}

static void test_thread(void) {
    set_store("flow");
    init_fake(8);
    xccp_test_set_busy(1);              /* login/sessão na hw 3 */
    CHECK(xcc_bg_start() == -1 && !xcc_bg_running());
    xccp_test_set_busy(0);
    CHECK(xcc_bg_start() == 0 && xcc_bg_running());
    CHECK(xcc_bg_start() == 0);         /* já rodando */
    xcc_rows_t *r = &g_rows;
    for (int i = 0; i < 300 && r->row[XCC_ROW_ALL].n == 0; i++) { usleep(10000); xcc_rows_copy(r); }
    CHECK(r->row[XCC_ROW_ALL].n == 39);   /* veio do ROWS.TXT */
    const char *v[2] = { r->row[XCC_ROW_ALL].items[0].product, r->row[XCC_ROW_ALL].items[1].product };
    xcc_want(XCC_COVER, v, 2);
    int ok = 0;
    for (int i = 0; i < 300 && !ok; i++) {
        usleep(10000);
        xcc_poll();
        ok = xcc_cover_get(v[0]) && xcc_cover_get(v[1]);
    }
    CHECK(ok);
    /* pedido lento em voo: o stop corta pelo xcn_cancelled */
    g_slow = 1;
    const char *h[1] = { r->row[XCC_ROW_ALL].items[5].product };
    xcc_want(XCC_HERO, h, 1);
    usleep(150000);
    uint64_t t0 = xcn_now_ms();
    xcc_bg_stop();
    CHECK(xcc_bg_start() == -1);        /* parando ainda */
    while (xcc_bg_running() && xcn_now_ms() - t0 < 5000) usleep(2000);
    unsigned ms = (unsigned)(xcn_now_ms() - t0);
    CHECK(!xcc_bg_running());
    CHECK(ms < 1000);
    printf("stop do worker com pedido em voo: %u ms\n", ms);
    CHECK(xcn_cancelled() == 0);        /* gancho desregistrado */
    g_slow = 0;
    for (int i = 0; i < 100 && xccp_slot_busy(); i++) usleep(1000);
    CHECK(xcc_bg_start() == 0);         /* sobe de novo */
    usleep(50000);
    xcc_bg_stop();
    while (xcc_bg_running()) usleep(1000);
    for (int i = 0; i < 100 && xccp_slot_busy(); i++) usleep(1000);
    xcc_shutdown();
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <fixtures> <tmp>\n", argv[0]); return 2; }
    FX = argv[1];
    snprintf(g_tmp, sizeof(g_tmp), "%s", argv[2]);
    char *dc = slurp("displaycatalog_BR.json", NULL);
    if (!dc || xcc_parse_images(dc, &g_refs, &g_nrefs) != 0) { printf("FAIL fixtures\n"); return 1; }
    free(dc);
    load_catalog();
    test_parse();
    test_decode();
    test_flow();
    test_cache_offline();
    test_failures();
    test_thread();
    free(g_refs);
    free(g_cat);
    printf("%s: %d/%d checks\n", g_fail ? "CATALOG UNIT FAILED" : "CATALOG UNIT PASSED", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
