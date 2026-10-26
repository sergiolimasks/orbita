/* XCloud360 — camada de dados do menu de capas. Ver xc_catalog.h e
 * app/h264play/CATALOG-COVERS.md. Portável: xc_port.h + xcc_port.h. */
#include "xc_catalog.h"
#include "xcc_port.h"
#include "../login/xc_port.h"
#include "../login/https.h"
#ifndef XCC_NO_LOGIN_HOOKS
#include "../login/login_flow.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ROWS_FILE "ROWS.TXT"
/* v0.20.0: "XCCROWS 2" = nomes em UTF-8. O "XCCROWS 1" de antes tem os nomes
 * dobrados em ASCII: é ignorado (as fileiras voltam com o login). */
#define ROWS_MAGIC "XCCROWS 2"
#define IDX_FILE  "IMGIDX.TXT"
#define SIGL_HOST "catalog.gamepass.com"
#define DC_HOST   "displaycatalog.mp.microsoft.com"
#define IMG_HOST  "store-images.s-microsoft.com"
/* IDs que o xbox.com/pt-BR/play usa hoje (estado inicial da página,
 * chaves "v3_<id>__Cloud:XGPUWEB", conferido em 2026-10-25). */
#define SIGL_POPULAR "6a589fa0-d493-472b-8e20-3813699d7056"   /* Mais populares na nuvem */
#define SIGL_NEW     "06323672-b8c8-43cc-b0de-32d5a9834749"   /* Adicionados recentemente */

#define ENT_MAX    2048
#define HASH_SZ    4096          /* potência de 2, > 2*ENT_MAX */
#define WANT_MAX   64
#define DONE_MAX   6
#define DC_BATCH   20
#define TRAILER    32            /* magia (4) + bigId (28) no fim do .JPG */
/* Magia do trailer por tipo. v0.20.0: hero passou de 640x360 pra 1280x720;
 * um HERO/XXXXXXXX.JPG antigo ("XCC1") não confere e é baixado de novo no tamanho
 * novo (sobrescreve o mesmo arquivo). Capa continua "XCC1". */
static const char k_magic[2][5] = { "XCC1", "XCH2" };
#define IMG_BODY   (768 * 1024)   /* v0.20.0: hero 1280x720 q70 ~100-300 KB */
#define DC_BODY    (8 << 20)
#define SIGL_BODY  (2 << 20)
#define RAW_MAX    5000
#define TITLES_MAX 2048
#define LRU_COVERS_MAX 128
#define LRU_HEROES_MAX 8
#define FAILS_MAX  5
#define ROWS_CAP   (512 * 1024)
#define IDX_CAP    (1024 * 1024)
#define IDX_SAVE_EVERY 24

typedef struct {
    char     product[24];
    char     path[2][160];   /* [XCC_COVER], [XCC_HERO]; "" = sem imagem */
    uint8_t  urls;           /* 1 = o displaycatalog já respondeu */
    uint8_t  disk;           /* bit (1<<kind): .JPG no pendrive */
    uint8_t  ram;            /* bit: no LRU do main */
    uint8_t  busy;           /* bit: decodificando / na caixa de saída */
    uint8_t  fails[2], dc_fails;
    uint64_t retry_at[2], dc_retry_at;
} ent_t;

typedef struct {
    char      product[24];
    int       kind, w, h;
    uint32_t *px;
} done_t;

typedef struct {
    char        product[24];
    unsigned    last;
    xcc_image_t img;
    uint32_t   *px;
} lru_t;

/* ---- estado compartilhado (xccp_lock) ---- */
static int          g_inited;
static xcc_config_t g_cfg;
static char         g_market[8], g_lang[16];
static ent_t        g_ent[ENT_MAX];
static int          g_nent;
static int16_t      g_hash[HASH_SZ];        /* índice+1 em g_ent */
static xcc_item_t  *g_titles;               /* catálogo da conta (malloc) */
static int          g_ntitles;
static char         g_mru[XCC_ROW_MAX][48];
static int          g_nmru;
static char       (*g_raw[2])[24];          /* SIGL: [0] populares, [1] novos */
static int          g_nraw[2];
static xcc_rows_t   g_rows;
static unsigned     g_rows_seq = 1, g_seq = 1;
static int          g_cache_loaded, g_rows_net, g_mru_net, g_rows_dirty, g_idx_dirty, g_bg_done;
static char         g_want[2][WANT_MAX][24];
static int          g_nwant[2];
static done_t       g_done[DONE_MAX];
static int          g_ndone;
static xcc_status_t g_st;
/* ---- só o worker ---- */
static uint64_t     g_sigl_retry_at, g_net_hold_until, g_bg_next;
static int          g_net_errs;
/* ---- só o main ---- */
static lru_t        g_lru_c[LRU_COVERS_MAX], g_lru_h[LRU_HEROES_MAX];
static int          g_nlru[2];
static unsigned     g_tick = 1;
/* ---- controle do worker ---- */
static volatile int g_running, g_stop;
static int          g_spawned;   /* main: subimos o worker e a hw ainda pode estar saindo */

static const char *k_row_title[XCC_NROWS] = {
    "Voltar a jogar", "Mais populares na nuvem", "Adicionados recentemente", "Todos os jogos"
};

/* ------------------------------------------------------------- util */

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

static void upcpy(char *d, size_t cap, const char *s) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    d[i] = 0;
}

/* cópia com corte (sem o falso alarme de -Wformat-truncation do snprintf) */
static void scpy(char *d, size_t cap, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(d, s, n);
    d[n] = 0;
}

static void set_err(const char *fmt, const char *a, int b) {
    snprintf(g_st.last_err, sizeof(g_st.last_err), fmt, a, b);
}

/* lock: */
static ent_t *ent_find(const char *product) {
    if (!product || !product[0]) return NULL;
    uint32_t h = fnv(product) & (HASH_SZ - 1);
    for (int i = 0; i < HASH_SZ; i++) {
        int k = g_hash[(h + (uint32_t)i) & (HASH_SZ - 1)];
        if (!k) return NULL;
        if (!strcasecmp(g_ent[k - 1].product, product)) return &g_ent[k - 1];
    }
    return NULL;
}

/* lock: */
static ent_t *ent_add(const char *product) {
    ent_t *e = ent_find(product);
    if (e || !product || !product[0] || g_nent >= ENT_MAX) return e;
    e = &g_ent[g_nent];
    memset(e, 0, sizeof(*e));
    upcpy(e->product, sizeof(e->product), product);
    uint32_t h = fnv(product) & (HASH_SZ - 1);
    while (g_hash[h]) h = (h + 1) & (HASH_SZ - 1);
    g_hash[h] = (int16_t)(++g_nent);
    return e;
}

static uint64_t backoff_ms(int fails) {
    uint64_t b = 2000ull << (fails > 7 ? 7 : fails);   /* 4s, 8s ... ~4min */
    return b > 300000 ? 300000 : b;
}

/* --------------------------------------------------------- fileiras */

static void item_from(xcc_item_t *d, const xcc_item_t *s) { *d = *s; }

/* lock: refaz as 4 fileiras a partir de títulos + MRU + SIGLs. */
static void rebuild_rows(void) {
    if (g_ntitles <= 0) return;   /* sem catálogo: fica o que veio do cache */
    static int16_t th[HASH_SZ];   /* bigId -> 1º título com ele */
    static uint8_t seen[TITLES_MAX];
    memset(th, 0, sizeof(th));
    for (int i = 0; i < g_ntitles; i++) {
        if (!g_titles[i].product[0]) continue;
        uint32_t h = fnv(g_titles[i].product) & (HASH_SZ - 1);
        int dup = 0;
        while (th[h]) {
            if (!strcasecmp(g_titles[th[h] - 1].product, g_titles[i].product)) { dup = 1; break; }
            h = (h + 1) & (HASH_SZ - 1);
        }
        if (!dup) th[h] = (int16_t)(i + 1);
    }
    xcc_rows_t *R = &g_rows;
    /* Todos */
    int na = g_ntitles < XCC_ALL_MAX ? g_ntitles : XCC_ALL_MAX;
    for (int i = 0; i < na; i++) item_from(&R->all[i], &g_titles[i]);
    R->row[XCC_ROW_ALL].n = na;
    /* Voltar a jogar: titleId, na ordem do MRU */
    int nr = 0;
    for (int m = 0; m < g_nmru && nr < XCC_ROW_MAX; m++) {
        for (int i = 0; i < g_ntitles; i++) {
            if (strcmp(g_titles[i].id, g_mru[m])) continue;
            int dup = 0;
            for (int k = 0; k < nr; k++) if (!strcmp(R->recent[k].id, g_titles[i].id)) { dup = 1; break; }
            if (!dup) item_from(&R->recent[nr++], &g_titles[i]);
            break;
        }
    }
    R->row[XCC_ROW_RECENT].n = nr;
    /* Populares / novos: bigIds da SIGL que a conta pode jogar */
    for (int s = 0; s < 2; s++) {
        xcc_item_t *dst = s == 0 ? R->popular : R->fresh;
        int n = 0;
        memset(seen, 0, sizeof(seen));
        for (int i = 0; i < g_nraw[s] && n < XCC_ROW_MAX; i++) {
            uint32_t h = fnv(g_raw[s][i]) & (HASH_SZ - 1);
            int t = -1;
            while (th[h]) {
                if (!strcasecmp(g_titles[th[h] - 1].product, g_raw[s][i])) { t = th[h] - 1; break; }
                h = (h + 1) & (HASH_SZ - 1);
            }
            if (t < 0 || (t < TITLES_MAX && seen[t])) continue;
            if (t < TITLES_MAX) seen[t] = 1;
            item_from(&dst[n++], &g_titles[t]);
        }
        R->row[s == 0 ? XCC_ROW_POPULAR : XCC_ROW_NEW].n = n;
    }
    for (int r = 0; r < XCC_NROWS; r++)
        for (int i = 0; i < R->row[r].n; i++) ent_add(R->row[r].items[i].product);
    g_rows_seq++;
    g_seq++;
}

static void rows_init_ptrs(xcc_rows_t *R) {
    R->row[XCC_ROW_RECENT].items = R->recent;
    R->row[XCC_ROW_POPULAR].items = R->popular;
    R->row[XCC_ROW_NEW].items = R->fresh;
    R->row[XCC_ROW_ALL].items = R->all;
    for (int r = 0; r < XCC_NROWS; r++) snprintf(R->row[r].title, sizeof(R->row[r].title), "%s", k_row_title[r]);
}

/* ------------------------------------------------------ cache (USB) */

/* lock: ROWS.TXT */
static char *rows_serialize(size_t *len) {
    char *o = (char *)malloc(ROWS_CAP);
    if (!o) return NULL;
    size_t w = (size_t)snprintf(o, ROWS_CAP, "%s\n", ROWS_MAGIC);
    for (int r = 0; r < XCC_NROWS; r++) {
        w += (size_t)snprintf(o + w, ROWS_CAP - w, "#ROW %d\n", r);
        for (int i = 0; i < g_rows.row[r].n && w + 200 < ROWS_CAP; i++) {
            const xcc_item_t *it = &g_rows.row[r].items[i];
            w += (size_t)snprintf(o + w, ROWS_CAP - w, "%s\t%s\t%x\t%s\n", it->id, it->product, it->flags, it->name);
        }
    }
    *len = w;
    return o;
}

static int next_field(char **p, char *dst, size_t cap) {
    char *s = *p;
    size_t n = 0;
    while (s[n] && s[n] != '\t' && s[n] != '\n' && s[n] != '\r') n++;
    size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(dst, s, c);
    dst[c] = 0;
    int more = s[n] == '\t';
    *p = s + n + (s[n] ? 1 : 0);
    return more;
}

static char *next_line(char **p) {
    char *s = *p;
    if (!s || !*s) return NULL;
    char *nl = strchr(s, '\n');
    if (nl) { *nl = 0; *p = nl + 1; }
    else *p = s + strlen(s);
    size_t l = strlen(s);
    if (l && s[l - 1] == '\r') s[l - 1] = 0;
    return s;
}

/* lock: aplica ROWS.TXT (txt é alterado). */
static int rows_apply(char *txt) {
    char *p = txt, *ln = next_line(&p);
    if (!ln || strcmp(ln, ROWS_MAGIC)) return -1;
    int r = -1, got = 0;
    xcc_rows_t *R = &g_rows;
    for (int k = 0; k < XCC_NROWS; k++) R->row[k].n = 0;
    while ((ln = next_line(&p)) != NULL) {
        if (!strncmp(ln, "#ROW ", 5)) { r = atoi(ln + 5); if (r < 0 || r >= XCC_NROWS) r = -1; continue; }
        if (r < 0 || !ln[0]) continue;
        int cap = r == XCC_ROW_ALL ? XCC_ALL_MAX : XCC_ROW_MAX;
        if (R->row[r].n >= cap) continue;
        xcc_item_t it;
        memset(&it, 0, sizeof(it));
        char fl[16];
        char *q = ln;
        if (!next_field(&q, it.id, sizeof(it.id))) continue;
        if (!next_field(&q, it.product, sizeof(it.product))) continue;
        if (!next_field(&q, fl, sizeof(fl))) continue;
        next_field(&q, it.name, sizeof(it.name));
        if (!it.id[0]) continue;
        it.flags = (unsigned)strtoul(fl, NULL, 16);
        R->row[r].items[R->row[r].n++] = it;
        got++;
    }
    /* o cache também alimenta as entradas do rebuild até a rede responder */
    if (g_ntitles == 0 && R->row[XCC_ROW_ALL].n > 0) {
        int n = R->row[XCC_ROW_ALL].n;
        xcc_item_t *t = (xcc_item_t *)malloc(sizeof(*t) * (size_t)n);
        if (t) { memcpy(t, R->all, sizeof(*t) * (size_t)n); g_titles = t; g_ntitles = n; }
    }
    if (g_nmru == 0)
        for (int i = 0; i < R->row[XCC_ROW_RECENT].n && g_nmru < XCC_ROW_MAX; i++)
            snprintf(g_mru[g_nmru++], 48, "%s", R->recent[i].id);
    for (int s = 0; s < 2; s++) {
        const xcc_row_t *row = &R->row[s == 0 ? XCC_ROW_POPULAR : XCC_ROW_NEW];
        if (g_nraw[s] || !row->n) continue;
        g_raw[s] = (char (*)[24])calloc((size_t)row->n, 24);
        if (!g_raw[s]) continue;
        for (int i = 0; i < row->n; i++) upcpy(g_raw[s][i], 24, row->items[i].product);
        g_nraw[s] = row->n;
    }
    for (int k = 0; k < XCC_NROWS; k++)
        for (int i = 0; i < R->row[k].n; i++) ent_add(R->row[k].items[i].product);
    g_rows_seq++;
    g_seq++;
    return got;
}

/* lock: IMGIDX.TXT */
static char *idx_serialize(size_t *len) {
    size_t cap = (size_t)g_nent * 360 + 64;
    char *o = (char *)malloc(cap);
    if (!o) return NULL;
    size_t w = (size_t)snprintf(o, cap, "XCCIMG 1\n");
    for (int i = 0; i < g_nent; i++) {
        const ent_t *e = &g_ent[i];
        if (!e->urls && !e->disk) continue;
        w += (size_t)snprintf(o + w, cap - w, "%s\t%d\t%d\t%s\t%s\n", e->product, e->urls, e->disk & 3,
                              e->path[XCC_COVER], e->path[XCC_HERO]);
    }
    *len = w;
    return o;
}

/* lock: */
static int idx_apply(char *txt) {
    char *p = txt, *ln = next_line(&p);
    if (!ln || strcmp(ln, "XCCIMG 1")) return -1;
    int got = 0;
    while ((ln = next_line(&p)) != NULL) {
        char prod[24], u[8], d[8], c[160], h[160];
        char *q = ln;
        c[0] = h[0] = 0;
        if (!next_field(&q, prod, sizeof(prod))) continue;
        if (!next_field(&q, u, sizeof(u))) continue;
        if (!next_field(&q, d, sizeof(d))) continue;
        next_field(&q, c, sizeof(c));
        next_field(&q, h, sizeof(h));
        if (c[0] && c[0] != '/') continue;
        if (h[0] && h[0] != '/') continue;
        ent_t *e = ent_add(prod);
        if (!e) break;
        e->urls = (uint8_t)(atoi(u) ? 1 : 0);
        e->disk = (uint8_t)(atoi(d) & 3);
        snprintf(e->path[XCC_COVER], sizeof(e->path[0]), "%s", c);
        snprintf(e->path[XCC_HERO], sizeof(e->path[0]), "%s", h);
        got++;
    }
    return got;
}

/* Thread com direito ao USB (login ou worker). */
static void load_cache(void) {
    char *buf = (char *)malloc(IDX_CAP + 1);
    int nr = -1, ni = -1;
    if (buf) {
        int l = xc_store_load(ROWS_FILE, buf, ROWS_CAP);
        if (l > 0) {
            buf[l] = 0;
            xccp_lock();
            nr = rows_apply(buf);
            xccp_unlock();
        }
        l = xc_store_load(IDX_FILE, buf, IDX_CAP);
        if (l > 0) {
            buf[l] = 0;
            xccp_lock();
            ni = idx_apply(buf);
            xccp_unlock();
        }
        free(buf);
    }
    xccp_lock();
    g_cache_loaded = 1;
    g_st.loaded_cache = 1;
    xccp_unlock();
    xc_log("[capas] cache: %d itens nas fileiras, %d no indice de imagens", nr, ni);
}

static void save_rows(void) {
    size_t len = 0;
    xccp_lock();
    char *o = g_ntitles > 0 ? rows_serialize(&len) : NULL;
    g_rows_dirty = 0;
    xccp_unlock();
    if (!o) return;
    if (xc_store_save(ROWS_FILE, o, len) != 0) xc_log("[capas] nao gravei %s", ROWS_FILE);
    free(o);
}

static void save_idx(void) {
    size_t len = 0;
    xccp_lock();
    char *o = idx_serialize(&len);
    g_idx_dirty = 0;
    xccp_unlock();
    if (!o) return;
    if (xc_store_save(IDX_FILE, o, len) != 0) xc_log("[capas] nao gravei %s", IDX_FILE);
    free(o);
}

/* -------------------------------------------------------------- rede */

static int net_ok(uint64_t now) { return now >= g_net_hold_until; }

static void net_result(int ok) {
    if (ok) { g_net_errs = 0; return; }
    xccp_lock();
    g_st.net_errors++;
    xccp_unlock();
    if (++g_net_errs >= 3) {   /* rede fora: não martela */
        g_net_hold_until = xcn_now_ms() + 15000;
        g_net_errs = 0;
    }
}

static int do_fetch(const char *host, const char *path, size_t max, int *status, char **body, size_t *len) {
    xcc_fetch_fn f = g_cfg.fetch ? g_cfg.fetch : xcc_net_fetch;
    int rc = f(host, path, max, status, body, len);
    if (rc != 0) { free(*body); *body = NULL; }
    return rc;
}

static int fetch_one_sigl(const char *id, char (**ids)[24], int *n) {
    char path[256];
    int status = 0;
    char *body = NULL;
    size_t len = 0;
    *ids = NULL;
    *n = 0;
    for (int v = 3; v >= 2; v--) {
        if (v == 3)
            snprintf(path, sizeof(path), "/sigls/v3?id=%s&market=%s&language=%s&subscriptionContext=none"
                     "&platformContext=Cloud%%3AXGPUWEB", id, g_market, g_lang);
        else
            snprintf(path, sizeof(path), "/sigls/v2?id=%s&market=%s&language=%s", id, g_market, g_lang);
        int rc = do_fetch(SIGL_HOST, path, SIGL_BODY, &status, &body, &len);
        net_result(rc == 0);
        if (rc != 0) return -1;   /* transporte: nem tenta a v2 agora */
        if (status == 200 && xcc_parse_sigl(body, ids, n, NULL, 0) == 0 && *n > 0) { free(body); return 0; }
        free(*ids);
        *ids = NULL;
        *n = 0;
        xccp_lock();
        set_err("sigl %s HTTP %d", id, status);
        xccp_unlock();
        free(body);
        body = NULL;
    }
    return -1;
}

static void fetch_sigls(uint64_t now) {
    static const char *ids[2] = { SIGL_POPULAR, SIGL_NEW };
    int ok = 0;
    for (int s = 0; s < 2; s++) {
        char (*l)[24] = NULL;
        int n = 0;
        if (fetch_one_sigl(ids[s], &l, &n) != 0) continue;
        if (n > RAW_MAX) n = RAW_MAX;
        xccp_lock();
        free(g_raw[s]);
        g_raw[s] = l;
        g_nraw[s] = n;
        xccp_unlock();
        ok++;
    }
    xccp_lock();
    if (ok) { rebuild_rows(); g_rows_dirty = g_ntitles > 0; }
    if (ok == 2) { g_rows_net = 1; g_st.rows_net = 1; }
    else g_sigl_retry_at = now + 60000;
    xccp_unlock();
    xc_log("[capas] SIGL populares %d, novos %d (%s)", g_nraw[0], g_nraw[1], ok == 2 ? "ok" : "falhou, tento depois");
}

/* Lote do displaycatalog: `first` + até DC_BATCH-1 que também faltam. */
static void do_dc_batch(const char *first) {
    char list[DC_BATCH][24];
    int n = 0;
    uint64_t now = xcn_now_ms();
    xccp_lock();
    snprintf(list[n++], 24, "%s", first);
#define WANTS_URL(e) ((e) && !(e)->urls && now >= (e)->dc_retry_at && (e)->dc_fails < FAILS_MAX)
#define ADD_PROD(p) do { ent_t *_e = ent_find(p); int _d = 0; \
        if (!WANTS_URL(_e)) break; \
        for (int _k = 0; _k < n; _k++) if (!strcasecmp(list[_k], _e->product)) { _d = 1; break; } \
        if (!_d) snprintf(list[n++], 24, "%s", _e->product); } while (0)
    for (int k = 0; k < 2; k++) for (int i = 0; i < g_nwant[k] && n < DC_BATCH; i++) ADD_PROD(g_want[k][i]);
    for (int r = 0; r < XCC_NROWS; r++)
        for (int i = 0; i < g_rows.row[r].n && n < DC_BATCH; i++) ADD_PROD(g_rows.row[r].items[i].product);
    for (int i = 0; i < g_nent && n < DC_BATCH; i++) ADD_PROD(g_ent[i].product);
#undef ADD_PROD
#undef WANTS_URL
    xccp_unlock();

    char path[128 + DC_BATCH * 24];
    int pl = snprintf(path, sizeof(path), "/v7.0/products?bigIds=");
    for (int i = 0; i < n; i++) pl += snprintf(path + pl, sizeof(path) - (size_t)pl, "%s%s", i ? "," : "", list[i]);
    snprintf(path + pl, sizeof(path) - (size_t)pl, "&market=%s&languages=%s&fieldsTemplate=Browse", g_market, g_lang);
    int status = 0;
    char *body = NULL;
    size_t len = 0;
    int rc = do_fetch(DC_HOST, path, DC_BODY, &status, &body, &len);
    net_result(rc == 0);
    xcc_imgref_t *refs = NULL;
    int nrefs = 0;
    int ok = rc == 0 && status == 200 && xcc_parse_images(body, &refs, &nrefs) == 0;
    free(body);
    xccp_lock();
    for (int i = 0; i < n; i++) {
        ent_t *e = ent_find(list[i]);
        if (!e) continue;
        if (!ok) {
            e->dc_fails++;
            e->dc_retry_at = now + backoff_ms(e->dc_fails);
            continue;
        }
        e->urls = 1;   /* fora da resposta = sem imagem neste mercado */
        e->path[0][0] = e->path[1][0] = 0;
        for (int k = 0; k < nrefs; k++) {
            if (strcasecmp(refs[k].product, e->product)) continue;
            snprintf(e->path[XCC_COVER], sizeof(e->path[0]), "%s", refs[k].cover);
            snprintf(e->path[XCC_HERO], sizeof(e->path[0]), "%s", refs[k].hero);
            break;
        }
    }
    if (ok) g_idx_dirty += n;
    else set_err("displaycatalog HTTP %s%d", "", status);
    g_seq++;
    xccp_unlock();
    free(refs);
}

/* Lê do pendrive (confere o trailer) ou baixa (e grava). jpg malloc. */
static int get_jpeg(const char *product, int kind, const char *ipath, int on_disk, uint8_t **jpg, size_t *jlen,
                    int *from_net) {
    char fname[32];
    xcc_file_name(fname, sizeof(fname), product, kind);
    *jpg = NULL;
    *jlen = 0;
    *from_net = 0;
    if (on_disk) {
        uint8_t *b = (uint8_t *)malloc(IMG_BODY);
        int l = b ? xc_store_load(fname, b, IMG_BODY) : -1;
        if (l > TRAILER + 4 && !memcmp(b + l - TRAILER, k_magic[kind ? 1 : 0], 4) &&
            !strncasecmp((char *)b + l - TRAILER + 4, product, TRAILER - 4) && b[0] == 0xFF && b[1] == 0xD8) {
            *jpg = b;
            *jlen = (size_t)l - TRAILER;
            return 0;
        }
        free(b);
        xccp_lock();
        ent_t *e = ent_find(product);
        if (e) e->disk &= (uint8_t)~(1u << kind);
        g_idx_dirty++;
        xccp_unlock();
    }
    char path[256];
    xcc_image_path(path, sizeof(path), ipath, kind);
    int status = 0;
    char *body = NULL;
    size_t len = 0;
    int rc = do_fetch(IMG_HOST, path, IMG_BODY - TRAILER, &status, &body, &len);
    net_result(rc == 0);
    if (rc != 0) return -1;
    if (status != 200 || len < 4 || (uint8_t)body[0] != 0xFF || (uint8_t)body[1] != 0xD8) {
        xccp_lock();
        set_err("imagem HTTP %s%d", "", status);
        xccp_unlock();
        free(body);
        return -1;
    }
    uint8_t *b = (uint8_t *)realloc(body, len + TRAILER + 1);
    if (!b) { free(body); return -1; }
    memset(b + len, 0, TRAILER);
    memcpy(b + len, k_magic[kind ? 1 : 0], 4);
    memcpy(b + len + 4, product, strlen(product) < TRAILER - 4 ? strlen(product) : TRAILER - 4);
    *from_net = 1;
    int saved = xc_store_save(fname, b, len + TRAILER) == 0;
    xccp_lock();
    g_st.downloads++;
    ent_t *e = ent_find(product);
    if (e && saved) { e->disk |= (uint8_t)(1u << kind); g_idx_dirty++; }
    xccp_unlock();
    *jpg = b;
    *jlen = len;
    return 0;
}

typedef struct {
    int  type;          /* 1 = lote displaycatalog, 2 = imagem */
    int  kind, decode;
    char product[24];
    char path[160];
    int  on_disk;
} job_t;

/* lock: escolhe o próximo pedido visível. */
static int pick_wanted(job_t *j, uint64_t now) {
    for (int k = 0; k < 2; k++) {
        /* só o que cabe no LRU: mais pedidos que vagas = decodificar e
         * despejar pra sempre */
        int lim = g_nwant[k] < (k ? g_cfg.max_heroes : g_cfg.max_covers) ? g_nwant[k]
                                                                         : (k ? g_cfg.max_heroes : g_cfg.max_covers);
        for (int i = 0; i < lim; i++) {
            ent_t *e = ent_find(g_want[k][i]);
            unsigned bit = 1u << k;
            if (!e || ((e->ram | e->busy) & bit)) continue;
            if (!e->urls) {
                if (now < e->dc_retry_at || e->dc_fails >= FAILS_MAX) continue;
                j->type = 1;
                snprintf(j->product, sizeof(j->product), "%s", e->product);
                return 1;
            }
            if (!e->path[k][0] || e->fails[k] >= FAILS_MAX || now < e->retry_at[k]) continue;
            if (g_ndone >= DONE_MAX) continue;   /* main ainda não esvaziou */
            j->type = 2;
            j->kind = k;
            j->decode = 1;
            j->on_disk = (e->disk & bit) != 0;
            snprintf(j->product, sizeof(j->product), "%s", e->product);
            snprintf(j->path, sizeof(j->path), "%s", e->path[k]);
            e->busy |= (uint8_t)bit;
            return 1;
        }
    }
    return 0;
}

/* lock: fundo — URLs e capas que faltam no pendrive, na ordem das fileiras. */
static int pick_background(job_t *j, uint64_t now) {
    for (int r = 0; r < XCC_NROWS; r++) {
        for (int i = 0; i < g_rows.row[r].n; i++) {
            ent_t *e = ent_find(g_rows.row[r].items[i].product);
            if (!e) continue;
            if (!e->urls) {
                if (now < e->dc_retry_at || e->dc_fails >= FAILS_MAX) continue;
                j->type = 1;
                snprintf(j->product, sizeof(j->product), "%s", e->product);
                return 1;
            }
            if (!e->path[XCC_COVER][0] || (e->disk & 1) || (e->busy & 1)) continue;
            if (e->fails[XCC_COVER] >= FAILS_MAX || now < e->retry_at[XCC_COVER]) continue;
            j->type = 2;
            j->kind = XCC_COVER;
            j->decode = 0;
            j->on_disk = 0;
            snprintf(j->product, sizeof(j->product), "%s", e->product);
            snprintf(j->path, sizeof(j->path), "%s", e->path[XCC_COVER]);
            e->busy |= 1;
            return 1;
        }
    }
    return 0;
}

static void do_image(const job_t *j) {
    uint8_t *jpg = NULL;
    size_t jl = 0;
    int from_net = 0;
    int bit = 1 << j->kind;
    int rc = get_jpeg(j->product, j->kind, j->path, j->on_disk, &jpg, &jl, &from_net);
    uint32_t *px = NULL;
    int w = j->kind == XCC_HERO ? XCC_HERO_W : XCC_COVER_W, h = j->kind == XCC_HERO ? XCC_HERO_H : XCC_COVER_H;
    if (rc == 0 && j->decode) px = xcc_decode_jpeg(jpg, jl, w, h);
    free(jpg);
    uint64_t now = xcn_now_ms();
    xccp_lock();
    ent_t *e = ent_find(j->product);
    if (rc == 0 && !from_net) g_st.disk_hits++;
    if (rc == 0 && j->decode && !px) g_st.decode_errors++;
    int failed = rc != 0 || (j->decode && !px);
    if (e) {
        if (failed) {
            e->fails[j->kind]++;
            e->retry_at[j->kind] = now + backoff_ms(e->fails[j->kind]);
        } else {
            e->fails[j->kind] = 0;
        }
    }
    if (px && g_ndone < DONE_MAX) {
        done_t *d = &g_done[g_ndone++];
        snprintf(d->product, sizeof(d->product), "%s", j->product);
        d->kind = j->kind;
        d->w = w;
        d->h = h;
        d->px = px;
        px = NULL;
        g_st.decodes++;   /* busy fica até o main pegar */
    } else if (e) {
        e->busy &= (uint8_t)~bit;
    }
    g_seq++;
    xccp_unlock();
    free(px);
}

int xcc_worker_step(void) {
    if (!g_inited) return 0;
    uint64_t now = xcn_now_ms();
    if (!g_cache_loaded) { load_cache(); return 1; }
    if (g_rows_dirty && g_ntitles > 0) { save_rows(); return 1; }
    if (!g_rows_net && net_ok(now) && now >= g_sigl_retry_at) { fetch_sigls(now); return 1; }
    job_t j;
    memset(&j, 0, sizeof(j));
    xccp_lock();
    int got = pick_wanted(&j, now);
    xccp_unlock();
    if (got && j.type == 1 && !net_ok(now)) got = 0;
    if (got && j.type == 2 && !j.on_disk && !net_ok(now)) {   /* sem rede: só o que está no pendrive */
        xccp_lock();
        ent_t *e = ent_find(j.product);
        if (e) e->busy &= (uint8_t)~(1u << j.kind);
        xccp_unlock();
        got = 0;
    }
    if (got) {
        if (j.type == 1) do_dc_batch(j.product);
        else do_image(&j);
        return 1;
    }
    if (now >= g_bg_next && net_ok(now)) {
        xccp_lock();
        got = pick_background(&j, now);
        g_bg_done = !got;
        xccp_unlock();
        if (got) {
            if (j.type == 1) do_dc_batch(j.product);
            else do_image(&j);
            g_bg_next = xcn_now_ms() + (uint64_t)g_cfg.bg_pace_ms;
            return 1;
        }
    }
    if (g_idx_dirty >= IDX_SAVE_EVERY || (g_idx_dirty && g_bg_done)) { save_idx(); return 1; }
    return 0;
}

void xcc_worker_finish(void) {
    if (!g_inited) return;
    if (g_idx_dirty) save_idx();
    if (g_rows_dirty && g_ntitles > 0) save_rows();
    if (!g_cfg.fetch) xcc_net_close_all();
}

/* ------------------------------------------------------------ ganchos */

void xcc_hook_start(void) {
    if (g_inited && !g_cache_loaded) load_cache();
}

void xcc_set_mru(const xa_title_t *t, int n) {
    xccp_lock();
    g_nmru = 0;
    for (int i = 0; i < n && g_nmru < XCC_ROW_MAX; i++)
        if (t[i].id[0]) snprintf(g_mru[g_nmru++], 48, "%s", t[i].id);
    g_mru_net = 1;
    g_st.mru_net = 1;
    rebuild_rows();
    g_rows_dirty = 1;
    xccp_unlock();
}

void xcc_hook_cloud(const xa_endpoint_t *ep) {
    if (!g_inited || !ep || !ep->token) return;
    xa_title_t *t = NULL;
    int n = 0;
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    if (xa_fetch_mru(ep, g_cfg.mru_max, &t, &n, &e) == XA_OK) {
        xcc_set_mru(t, n);
        xc_log("[capas] MRU: %d titulos", n);
    } else {
        xc_log("[capas] MRU falhou: %s", e.msg);   /* sem token na mensagem */
    }
    free(t);
}

void xcc_hook_titles(const xa_title_t *t, int n) {
    if (!g_inited) return;
    if (n > TITLES_MAX) n = TITLES_MAX;
    xcc_item_t *c = (xcc_item_t *)calloc((size_t)(n > 0 ? n : 1), sizeof(*c));
    if (!c) return;
    for (int i = 0; i < n; i++) {
        snprintf(c[i].id, sizeof(c[i].id), "%s", t[i].id);
        upcpy(c[i].product, sizeof(c[i].product), t[i].product);
        snprintf(c[i].name, sizeof(c[i].name), "%s", t[i].name[0] ? t[i].name : t[i].id);
        c[i].flags = t[i].flags;
    }
    xccp_lock();
    free(g_titles);
    g_titles = c;
    g_ntitles = n;
    rebuild_rows();
    g_rows_dirty = 1;
    xccp_unlock();
    save_rows();   /* thread do login: USB liberado */
}

/* --------------------------------------------------------------- main */

void xcc_config_defaults(xcc_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->market = "BR";
    c->language = "pt-BR";
    c->max_covers = 64;      /* 64 x 186 KB = ~12 MB */
    c->max_heroes = 2;       /* v0.20.0: 2 x 3,7 MB (1280x720); o foco + o anterior */
    c->bg_pace_ms = 250;
    c->mru_max = 25;
}

int xcc_init(const xcc_config_t *cfg) {
    if (g_inited) return 0;
    if (cfg) g_cfg = *cfg;
    else xcc_config_defaults(&g_cfg);
    if (g_cfg.max_covers < 4) g_cfg.max_covers = 4;
    if (g_cfg.max_covers > LRU_COVERS_MAX) g_cfg.max_covers = LRU_COVERS_MAX;
    if (g_cfg.max_heroes < 1) g_cfg.max_heroes = 1;
    if (g_cfg.max_heroes > LRU_HEROES_MAX) g_cfg.max_heroes = LRU_HEROES_MAX;
    if (g_cfg.mru_max <= 0) g_cfg.mru_max = 25;
    if (g_cfg.bg_pace_ms < 0) g_cfg.bg_pace_ms = 0;
    snprintf(g_market, sizeof(g_market), "%s", g_cfg.market ? g_cfg.market : "BR");
    snprintf(g_lang, sizeof(g_lang), "%s", g_cfg.language ? g_cfg.language : "pt-BR");
    rows_init_ptrs(&g_rows);
    g_inited = 1;
#ifndef XCC_NO_LOGIN_HOOKS
    static const lf_hooks_t hooks = { xcc_hook_start, xcc_hook_cloud, xcc_hook_titles };
    lf_set_hooks(&hooks);
#endif
    return 0;
}

static lru_t *lru_arr(int kind) { return kind == XCC_HERO ? g_lru_h : g_lru_c; }
static int lru_cap(int kind) { return kind == XCC_HERO ? g_cfg.max_heroes : g_cfg.max_covers; }

void xcc_poll(void) {
    if (!g_inited) return;
    g_tick++;
    done_t d[DONE_MAX];
    xccp_lock();
    int n = g_ndone;
    memcpy(d, g_done, sizeof(done_t) * (size_t)n);
    g_ndone = 0;
    xccp_unlock();
    for (int i = 0; i < n; i++) {
        int k = d[i].kind;
        lru_t *a = lru_arr(k);
        int slot = -1;
        for (int s = 0; s < g_nlru[k]; s++) if (!strcmp(a[s].product, d[i].product)) { slot = s; break; }
        if (slot < 0 && g_nlru[k] < lru_cap(k)) slot = g_nlru[k]++;
        if (slot < 0) {   /* despeja o menos usado, de preferência fora do pedido atual */
            int best = -1, best_any = 0;
            xccp_lock();
            for (int s = 0; s < g_nlru[k]; s++) {
                if (a[s].last < a[best_any].last) best_any = s;
                int wanted = 0;
                for (int w = 0; w < g_nwant[k]; w++) if (!strcasecmp(g_want[k][w], a[s].product)) { wanted = 1; break; }
                if (!wanted && (best < 0 || a[s].last < a[best].last)) best = s;
            }
            xccp_unlock();
            slot = best >= 0 ? best : best_any;
        }
        char old[24];
        scpy(old, sizeof(old), a[slot].product);
        free(a[slot].px);
        a[slot].px = d[i].px;
        scpy(a[slot].product, sizeof(a[slot].product), d[i].product);
        a[slot].last = g_tick;
        a[slot].img.w = d[i].w;
        a[slot].img.h = d[i].h;
        a[slot].img.px = d[i].px;
        xccp_lock();
        if (old[0] && strcasecmp(old, d[i].product)) {
            ent_t *e = ent_find(old);
            if (e) e->ram &= (uint8_t)~(1u << k);
        }
        ent_t *e = ent_find(d[i].product);
        if (e) { e->ram |= (uint8_t)(1u << k); e->busy &= (uint8_t)~(1u << k); }
        g_seq++;
        xccp_unlock();
    }
}

unsigned xcc_seq(void) {
    xccp_lock();
    unsigned s = g_seq;
    xccp_unlock();
    return s;
}

int xcc_rows_copy(xcc_rows_t *out) {
    if (!g_inited) return 0;
    xccp_lock();
    if (out->seq == g_rows_seq) { xccp_unlock(); return 0; }
    memcpy(out, &g_rows, sizeof(*out));
    out->seq = g_rows_seq;
    xccp_unlock();
    rows_init_ptrs(out);
    return 1;
}

void xcc_want(int kind, const char *const *products, int n) {
    if (!g_inited || kind < 0 || kind > 1) return;
    xccp_lock();
    g_nwant[kind] = 0;
    for (int i = 0; i < n && g_nwant[kind] < WANT_MAX; i++)
        if (products[i] && products[i][0]) upcpy(g_want[kind][g_nwant[kind]++], 24, products[i]);
    xccp_unlock();
}

static const xcc_image_t *img_get(int kind, const char *product) {
    if (!g_inited || !product || !product[0]) return NULL;
    lru_t *a = lru_arr(kind);
    for (int s = 0; s < g_nlru[kind]; s++) {
        if (!strcasecmp(a[s].product, product)) { a[s].last = g_tick; return &a[s].img; }
    }
    xccp_lock();   /* não está: garante que está na fila */
    int found = 0;
    for (int i = 0; i < g_nwant[kind]; i++) if (!strcasecmp(g_want[kind][i], product)) { found = 1; break; }
    if (!found && g_nwant[kind] < WANT_MAX) upcpy(g_want[kind][g_nwant[kind]++], 24, product);
    xccp_unlock();
    return NULL;
}

const xcc_image_t *xcc_cover_get(const char *product) { return img_get(XCC_COVER, product); }
const xcc_image_t *xcc_hero_get(const char *product) { return img_get(XCC_HERO, product); }

void xcc_mru_touch(const char *title_id) {
    if (!g_inited || !title_id || !title_id[0]) return;
    xccp_lock();
    int at = -1;
    for (int i = 0; i < g_nmru; i++) if (!strcmp(g_mru[i], title_id)) { at = i; break; }
    if (at < 0) at = g_nmru < XCC_ROW_MAX ? g_nmru++ : XCC_ROW_MAX - 1;
    memmove(g_mru[1], g_mru[0], (size_t)at * sizeof(g_mru[0]));
    snprintf(g_mru[0], sizeof(g_mru[0]), "%s", title_id);
    rebuild_rows();
    g_rows_dirty = 1;
    xccp_unlock();
}

static int cancel_hook(void) { return g_stop; }

static int (*g_xstep)(void);
static void (*g_xfin)(void);
void xcc_set_worker_extra(int (*step)(void), void (*finish)(void)) { g_xstep = step; g_xfin = finish; }

static void task_main(void) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    if (https_init(&e) != 0) xc_log("[capas] https_init falhou: %s", e.msg);
    xcn_set_cancel_hook(cancel_hook);
    xc_log("[capas] worker ligado");
    while (!g_stop) {
        int w = xcc_worker_step();
        if (!g_stop && g_xstep) w |= g_xstep();   /* v0.20.1: log beta */
        if (!w) xcn_sleep_ms(30);
    }
    xcc_worker_finish();
    if (g_xfin) g_xfin();
    xcn_set_cancel_hook(NULL);
    xc_log("[capas] worker parado");
    xccp_sync();
    g_running = 0;
}

int xcc_bg_start(void) {
    if (!g_inited) return -1;
    if (g_running) return g_stop ? -1 : 0;
    if (xccp_slot_busy()) return -1;
    g_stop = 0;
    g_running = 1;
    xccp_sync();
    if (xccp_spawn(task_main) != 0) { g_running = 0; return -1; }
    g_spawned = 1;
    return 0;
}

void xcc_bg_stop(void) { if (g_running) { g_stop = 1; xccp_sync(); } }
/* 1 até a hw 3 estar livre de verdade: o g_running cai um pouco antes da
 * função da thread voltar, e um xst_start nesse meio-tempo falharia. */
int xcc_bg_running(void) {
    if (g_running) return 1;
    if (g_spawned) {
        if (xccp_slot_busy()) return 1;
        g_spawned = 0;
    }
    return 0;
}

void xcc_status(xcc_status_t *s) {
    xccp_lock();
    *s = g_st;
    s->n_index = g_nent;
    s->n_with_urls = s->n_covers_disk = s->n_heroes_disk = 0;
    for (int i = 0; i < g_nent; i++) {
        s->n_with_urls += g_ent[i].urls && g_ent[i].path[0][0];
        s->n_covers_disk += (g_ent[i].disk & 1) != 0;
        s->n_heroes_disk += (g_ent[i].disk & 2) != 0;
    }
    xccp_unlock();
    s->ram_covers = g_nlru[XCC_COVER];
    s->ram_heroes = g_nlru[XCC_HERO];
}

void xcc_debug_reset_backoff(void) {
    xccp_lock();
    for (int i = 0; i < g_nent; i++) {
        g_ent[i].fails[0] = g_ent[i].fails[1] = g_ent[i].dc_fails = 0;
        g_ent[i].retry_at[0] = g_ent[i].retry_at[1] = g_ent[i].dc_retry_at = 0;
    }
    xccp_unlock();
    g_sigl_retry_at = g_net_hold_until = g_bg_next = 0;
    g_net_errs = 0;
}

void xcc_shutdown(void) {
    for (int k = 0; k < 2; k++) {
        lru_t *a = lru_arr(k);
        for (int s = 0; s < g_nlru[k]; s++) free(a[s].px);
        memset(a, 0, sizeof(lru_t) * (size_t)(k == XCC_HERO ? LRU_HEROES_MAX : LRU_COVERS_MAX));
        g_nlru[k] = 0;
    }
    for (int i = 0; i < g_ndone; i++) free(g_done[i].px);
    g_ndone = 0;
    free(g_titles); g_titles = NULL; g_ntitles = 0;
    for (int s = 0; s < 2; s++) { free(g_raw[s]); g_raw[s] = NULL; g_nraw[s] = 0; }
    memset(g_hash, 0, sizeof(g_hash));
    g_nent = 0;
    g_nmru = 0;
    memset(&g_rows, 0, sizeof(g_rows));
    memset(&g_st, 0, sizeof(g_st));
    g_nwant[0] = g_nwant[1] = 0;
    g_cache_loaded = g_rows_net = g_mru_net = g_rows_dirty = g_idx_dirty = g_bg_done = 0;
    g_sigl_retry_at = g_net_hold_until = g_bg_next = 0;
    g_net_errs = 0;
    g_rows_seq++;
    g_seq++;
    g_inited = 0;
#ifndef XCC_NO_LOGIN_HOOKS
    lf_set_hooks(NULL);
#endif
}
