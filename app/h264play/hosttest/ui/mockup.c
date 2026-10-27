/*
 * XCloud360 — mockup do menu novo no host: monta as telas pelo MESMO
 * caminho do Xbox (catálogo -> xui_cat -> menu_model -> xui_mm -> xui_home)
 * e grava PNGs em hosttest/ui/out/. Também:
 *   - confere a sujeira: depois de cada cena, o canvas desenhado aos pedaços
 *     tem que ser idêntico a um redesenho completo (xui_view_repaint);
 *   - confere o xcc_want (foco primeiro) e o caminho de pixels UI_RGB;
 *   - mede o custo por quadro (desenho + cópia em tiles) -> out/bench.txt.
 * Dados: as fixtures da camada de catálogo (hosttest/catalog/fixtures:
 * covers/INDEX.tsv + JPEGs 176x264, hero/ 1280x720 — v0.20.0), decodificadas pelo
 * xcc_decode_jpeg de verdade. As funções xcc_* "do main" são simuladas aqui
 * (sem worker/rede): o resto é o código do Xbox.
 * Uso: mockup <catalog_fixtures_dir> <out_dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../source/menu_model.h"
#include "../../source/ui/xui.h"
#include "../../source/ui/xui_mm.h"
#include "../../source/ui/xui_cat.h"
#include "xui_png.h"

#define W 1280
#define H 720

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- catálogo simulado (API do main de xc_catalog.h) ---------------- */
typedef struct { char product[24], id[48], name[96], row[8]; xcc_image_t cover, hero; } fx_t;
static fx_t fx[128];
static int nfx;
static char g_missing[16][24];   /* bigIds sem capa nesta cena */
static int  g_nmissing, g_no_images;
static unsigned g_seq = 1, g_rows_seq = 1;
static xcc_rows_t g_rows;        /* o que o "worker" montou */
static char g_want[2][64][24];
static int  g_nwant[2], g_want_calls;

static fx_t *fx_get(const char *product) {
    for (int i = 0; i < nfx; i++) if (!strcmp(fx[i].product, product)) return &fx[i];
    return NULL;
}

unsigned xcc_seq(void) { return g_seq; }
int xcc_rows_copy(xcc_rows_t *out) {
    if (out->seq == g_rows_seq) return 0;
    memcpy(out, &g_rows, sizeof(*out));
    out->row[XCC_ROW_RECENT].items = out->recent;
    out->row[XCC_ROW_POPULAR].items = out->popular;
    out->row[XCC_ROW_NEW].items = out->fresh;
    out->row[XCC_ROW_ALL].items = out->all;
    out->seq = g_rows_seq;
    return 1;
}
void xcc_want(int kind, const char *const *products, int n) {
    g_want_calls++;
    g_nwant[kind] = n < 64 ? n : 64;
    for (int i = 0; i < g_nwant[kind]; i++) snprintf(g_want[kind][i], 24, "%s", products[i]);
}
const xcc_image_t *xcc_cover_get(const char *product) {
    if (g_no_images) return NULL;
    for (int i = 0; i < g_nmissing; i++) if (!strcmp(g_missing[i], product)) return NULL;
    fx_t *f = fx_get(product);
    return f && f->cover.px ? &f->cover : NULL;
}
const xcc_image_t *xcc_hero_get(const char *product) {
    if (g_no_images) return NULL;   /* (capas faltando não tiram o hero) */
    fx_t *f = fx_get(product);
    return f && f->hero.px ? &f->hero : NULL;
}

static int load_jpg(const char *path, int w, int h, xcc_image_t *img) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    uint32_t *px = xcc_decode_jpeg(buf, n, w, h);
    if (!px) return 0;
    img->w = w; img->h = h; img->px = px;
    return 1;
}

static int cmp_name(const void *a, const void *b) {
    return strcasecmp(((const xcc_item_t *)a)->name, ((const xcc_item_t *)b)->name);
}

/* INDEX.tsv -> fileiras como o xc_catalog monta (MRU, populares, novos, todos) */
static void load_catalog(const char *dir) {
    char path[700], line[512];
    snprintf(path, sizeof(path), "%s/covers/INDEX.tsv", dir);
    FILE *f = fopen(path, "r");
    if (!f) { printf("mockup: sem %s (só placeholders)\n", path); }
    while (f && fgets(line, sizeof(line), f) && nfx < 128) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = 0;
        char *c[4] = { line, 0, 0, 0 };
        int n = 1;
        for (char *p = line; *p && n < 4; p++) if (*p == '\t') { *p = 0; c[n++] = p + 1; }
        if (n < 4) continue;
        fx_t *x = &fx[nfx++];
        snprintf(x->product, sizeof(x->product), "%s", c[0]);
        snprintf(x->id, sizeof(x->id), "%s", c[1]);
        snprintf(x->name, sizeof(x->name), "%s", c[2]);
        snprintf(x->row, sizeof(x->row), "%s", c[3]);
        snprintf(path, sizeof(path), "%s/covers/%s.jpg", dir, x->product);
        load_jpg(path, XCC_COVER_W, XCC_COVER_H, &x->cover);
        snprintf(path, sizeof(path), "%s/hero/%s.jpg", dir, x->product);
        load_jpg(path, XCC_HERO_W, XCC_HERO_H, &x->hero);
    }
    if (f) fclose(f);
    if (!nfx) {   /* sem fixtures: catálogo sintético, tudo placeholder */
        static const char *names[] = { "Forza Horizon 5", "Halo Infinite", "Starfield", "Sea of Thieves",
            "Microsoft Flight Simulator", "Gears 5", "Ori and the Will of the Wisps", "Hellblade II",
            "Age of Empires IV", "Pentiment", "Avowed", "Grounded", "Psychonauts 2", "As Dusk Falls",
            "Hi-Fi RUSH", "Ara: História Não Contada" };
        for (int i = 0; i < 16; i++) {
            fx_t *x = &fx[nfx++];
            snprintf(x->product, sizeof(x->product), "9SYN%08d", i);
            snprintf(x->id, sizeof(x->id), "SYN%02d", i);
            snprintf(x->name, sizeof(x->name), "%s", names[i]);
            snprintf(x->row, sizeof(x->row), "%s", i < 10 ? "pop" : "new");
        }
    }
    /* item do catálogo; flags: como no site, quase tudo Game Pass (alguns F2P não) */
    memset(&g_rows, 0, sizeof(g_rows));
    xcc_rows_t *R = &g_rows;
    int npop = 0;
    for (int i = 0; i < nfx; i++) {
        xcc_item_t it;
        memset(&it, 0, sizeof(it));
        snprintf(it.id, sizeof(it.id), "%s", fx[i].id);
        snprintf(it.product, sizeof(it.product), "%s", fx[i].product);
        snprintf(it.name, sizeof(it.name), "%s", fx[i].name);
        it.flags = XA_F_INSUB | ((i % 6 == 0) ? 0 : XA_F_SUB);
        if (!strcmp(fx[i].row, "pop") && R->row[XCC_ROW_POPULAR].n < XCC_ROW_MAX) {
            R->popular[R->row[XCC_ROW_POPULAR].n++] = it;
            /* "Voltar a jogar" simulado: 6 dos populares (MRU sintético) */
            if (npop++ >= 2 && R->row[XCC_ROW_RECENT].n < 6) R->recent[R->row[XCC_ROW_RECENT].n++] = it;
        }
        if (!strcmp(fx[i].row, "new") && R->row[XCC_ROW_NEW].n < XCC_ROW_MAX) R->fresh[R->row[XCC_ROW_NEW].n++] = it;
        R->all[R->row[XCC_ROW_ALL].n++] = it;
    }
    qsort(R->all, (size_t)R->row[XCC_ROW_ALL].n, sizeof(xcc_item_t), cmp_name);
    snprintf(R->row[XCC_ROW_RECENT].title, 32, "Voltar a jogar");
    snprintf(R->row[XCC_ROW_POPULAR].title, 32, "Mais populares na nuvem");
    snprintf(R->row[XCC_ROW_NEW].title, 32, "Adicionados recentemente");
    snprintf(R->row[XCC_ROW_ALL].title, 32, "Todos os jogos");
}

/* ---------------- relógio / bench ---------------- */
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

typedef struct { const char *name; double sum, max; int n; double area; } bench_t;
static bench_t B[12];
enum { BN_FULL, BN_HSCROLL, BN_VSCROLL, BN_FOCUS, BN_HERO, BN_DOTS, BN_TILE_FULL, BN_CARD, BN_FOCCARD,
       BN_HEROIMG, BN_HEROBIG, BN_COUNT };
static void bench_add(int k, double s, xui_rect_t r) {
    B[k].sum += s; B[k].n++; if (s > B[k].max) B[k].max = s; B[k].area += (double)r.w * r.h;
}

/* ---------------- app simulado (o que o main.c faria por quadro) ---------------- */
static menu_model_t mm;
static xui_mm_t ad;
static xui_cat_t cat;
static xui_home_data_t D;
static xui_view_t *V;
static xui_canvas_t C, C2;
static uint32_t *px, *px2, *fb;
static unsigned t_ms = 1000;
static char outdir[512];

static void sync(void) {
    xui_cat_sync(&cat, &mm, &D);               /* fileiras do catálogo -> modelo */
    xui_mm_sync(&ad, &mm, &D, xui_cat_flags, &cat);
}

/* Um quadro da home; kind = categoria do bench pra quadros sujos. */
static xui_rect_t frame(int kind, int *anim) {
    sync();
    double t0 = now_s();
    xui_rect_t r = xui_home_frame(V, &C, &D, xui_mm_confirm(&ad, &mm), t_ms, anim);
    if (!xui_rect_empty(r)) xui_tile_copy(fb, W, W, H, &C, r);
    double t2 = now_s();
    xui_cat_want(&cat, V, &D);
    if (kind >= 0 && !xui_rect_empty(r)) {
        int k = kind;
        if (r.w == W && r.h == H && kind != BN_HERO && kind != BN_VSCROLL) k = BN_FULL;
        bench_add(k, t2 - t0, r);
    }
    return r;
}

static void settle(int kind) {
    int anim = 1, guard = 0;
    while (anim && guard++ < 600) { frame(kind, &anim); t_ms += 16; }
    /* passa do atraso do hero e desenha o que faltar */
    t_ms += 400;
    frame(BN_HERO, &anim);
    while (anim && guard++ < 600) { t_ms += 16; frame(kind, &anim); }
}

static void save(const char *name) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s.png", outdir, name);
    if (xui_png_write(p, C.px, W, H, W) != 0) { printf("  FAIL png %s\n", p); fails++; }
    else printf("  %s\n", p);
}

/* desenho incremental == desenho completo? */
static void check_dirty(const char *name) {
    memcpy(px2, px, (size_t)W * H * 4);
    xui_view_repaint(V);
    int anim;
    sync();
    xui_home_frame(V, &C, &D, xui_mm_confirm(&ad, &mm), t_ms, &anim);
    long diff = 0;
    for (long i = 0; i < (long)W * H; i++) if (px[i] != px2[i]) diff++;
    CHECK(diff == 0, "%s: incremental != full redraw (%ld px)", name, diff);
}

static void scene(const char *name) { settle(-1); check_dirty(name); save(name); }

static void press(int dx, int dy, int kind) {
    mm_nav(&mm, dx, dy);
    int anim;
    frame(kind, &anim);
    t_ms += 16;
}

/* ---------------- status ---------------- */
static void status_scene(const char *name, xui_status_t *s) {
    int anim;
    xui_status_frame(V, &C, s, &D, t_ms, &anim);
    /* pontinhos: quadros parciais medidos */
    for (int i = 0; i < 6; i++) {
        t_ms += 100;
        double t0 = now_s();
        xui_rect_t r = xui_status_frame(V, &C, s, &D, t_ms, &anim);
        if (!xui_rect_empty(r)) xui_tile_copy(fb, W, W, H, &C, r);
        if (!xui_rect_empty(r)) bench_add(BN_DOTS, now_s() - t0, r);
    }
    save(name);
}

/* v0.20.3: chave beta e configurações (sujeira: o quadro seguinte igual não redesenha) */
static void key_scene(const char *name, const xui_key_t *k) {
    int anim;
    xui_key_frame(V, &C, k, &D, t_ms, &anim);
    for (int i = 0; i < 4; i++) {
        t_ms += 100;
        xui_rect_t r = xui_key_frame(V, &C, k, &D, t_ms, &anim);
        if (!k->busy) CHECK(xui_rect_empty(r), "%s: parado não redesenha", name);
    }
    save(name);
}
static void settings_scene(const char *name, const xui_settings_t *st) {
    int anim;
    xui_settings_frame(V, &C, st, &D, t_ms, &anim);
    t_ms += 100;
    xui_rect_t r = xui_settings_frame(V, &C, st, &D, t_ms, &anim);
    CHECK(xui_rect_empty(r) && !anim, "%s: parado", name);
    save(name);
}

/* v0.21.0: Wi-Fi e teclado de texto */
static void wifi_scene(const char *name, const xui_wifi_t *w) {
    int anim;
    xui_wifi_frame(V, &C, w, &D, t_ms, &anim);
    for (int i = 0; i < 4; i++) {
        t_ms += 100;
        xui_rect_t r = xui_wifi_frame(V, &C, w, &D, t_ms, &anim);
        if (w->status_kind != XUI_WS_BUSY && !w->scanning) CHECK(xui_rect_empty(r), "%s: parado não redesenha", name);
        else if (!xui_rect_empty(r)) CHECK(r.w < 600 && r.h <= 40, "%s: só os pontinhos (%dx%d)", name, r.w, r.h);
    }
    /* incremental == completo */
    memcpy(px2, px, (size_t)W * H * 4);
    xui_view_repaint(V);
    xui_wifi_frame(V, &C, w, &D, t_ms, &anim);
    long diff = 0;
    for (long i = 0; i < (long)W * H; i++) if (px[i] != px2[i]) diff++;
    CHECK(diff == 0, "%s: incremental != completo (%ld px)", name, diff);
    save(name);
}
static void textkey_scene(const char *name, const xui_textkey_t *k) {
    int anim;
    xui_textkey_frame(V, &C, k, &D, t_ms, &anim);
    for (int i = 0; i < 4; i++) {
        t_ms += 100;
        xui_rect_t r = xui_textkey_frame(V, &C, k, &D, t_ms, &anim);
        if (!k->busy) CHECK(xui_rect_empty(r), "%s: parado não redesenha", name);
    }
    save(name);
}

int main(int argc, char **argv) {
    const char *fxdir = argc > 1 ? argv[1] : "fixtures";
    snprintf(outdir, sizeof(outdir), "%s", argc > 2 ? argv[2] : "out");
    B[BN_FULL].name = "tela inteira (troca de fileira/hero/catalogo)";
    B[BN_HSCROLL].name = "rolagem horizontal (faixa da fileira)";
    B[BN_VSCROLL].name = "rolagem vertical (tela inteira)";
    B[BN_FOCUS].name = "foco anda 1 card (sem rolar)";
    B[BN_HERO].name = "hero novo (escala+degrade+tela)";
    B[BN_DOTS].name = "status: pontinhos";
    B[BN_TILE_FULL].name = "so a copia em tiles 1280x720";
    B[BN_CARD].name = "escalar 1 capa 176x264->148x222 (cache miss)";
    B[BN_FOCCARD].name = "escalar capa de foco 176x264->160x240";
    B[BN_HEROIMG].name = "hero 1280x720->1280x500 (copia 1:1)";
    B[BN_HEROBIG].name = "hero 1920x1080->1280x500 (bilinear)";

    /* ---- dados: fixtures do catálogo -> modelo (como o login faria) ---- */
    load_catalog(fxdir);
    mm_init(&mm);
    for (int i = 0; i < nfx; i++) {
        char g[300];
        snprintf(g, sizeof(g), "GAME\t%s\t%s\t0", fx[i].id, fx[i].name);
        mm_handle_line(&mm, g);
    }
    mm_handle_line(&mm, (char[]){ "END" });
    xui_cat_init(&cat);
    xui_cat_bind(&cat, &D);
    sync();
    {
        int nc = 0, nh = 0;
        for (int i = 0; i < nfx; i++) { nc += fx[i].cover.px != NULL; nh += fx[i].hero.px != NULL; }
        printf("mockup: %d jogos, %d fileiras, %d capas, %d heroes\n", mm.ngames, mm.nrows, nc, nh);
    }
    CHECK(mm.nrows == 4 || nfx == 16, "4 fileiras do catalogo (got %d)", mm.nrows);

    px = calloc((size_t)W * H, 4);
    px2 = calloc((size_t)W * H, 4);
    fb = calloc((size_t)W * 736, 4);
    xui_canvas_init(&C, px, W, H, W);
    xui_canvas_init(&C2, px2, W, H, W);
    V = xui_view_new(W, H);
    if (!V) { printf("sem memoria\n"); return 1; }
    D.hint = "A:Jogar|Y:Atualizar";
    D.version = "v0.20.1";   /* v0.20.0: versão ao lado do logo (= main.c) */

    /* ---- cenas ---- */
    scene("01_home");
    {   /* o catálogo recebeu as capas da tela, foco primeiro, e o hero do foco */
        const mm_game_t *g = mm_focus_game(&mm);
        const char *p = g ? xui_cat_product(&cat, g->id) : NULL;
        CHECK(p && g_nwant[XCC_COVER] >= 8 && !strcmp(g_want[XCC_COVER][0], p) &&
              g_nwant[XCC_HERO] == 1 && !strcmp(g_want[XCC_HERO][0], p),
              "xcc_want: foco primeiro (%d capas)", g_nwant[XCC_COVER]);
        int calls = g_want_calls;
        int anim; frame(-1, &anim); frame(-1, &anim);
        CHECK(g_want_calls == calls, "xcc_want so quando muda");
    }
    press(0, 1, BN_VSCROLL);
    for (int i = 0; i < 3; i++) press(1, 0, BN_FOCUS);
    scene("02_home_fileira2");
    /* rolagem: anda até o card sair da folga e grava um quadro no meio */
    press(1, 0, BN_HSCROLL);
    press(1, 0, BN_HSCROLL);
    press(1, 0, BN_HSCROLL);
    {
        int anim;
        frame(BN_HSCROLL, &anim); t_ms += 16;
        save("03_rolagem_no_meio");
    }
    scene("04_home_rolada");
    press(0, 1, BN_VSCROLL);
    {   /* quadro no meio da troca de fileira */
        int anim;
        frame(BN_VSCROLL, &anim); t_ms += 16;
        frame(BN_VSCROLL, &anim); t_ms += 16;
        save("05_troca_de_fileira_no_meio");
    }
    press(0, 1, BN_VSCROLL);
    for (int i = 0; i < 9; i++) { press(1, 0, BN_HSCROLL); t_ms += 70; }
    scene("06_todos_os_jogos");
    /* placeholder: capas do foco e vizinhas ainda não chegaram */
    {
        const mm_row_t *r = &mm.rows[mm.row];
        g_nmissing = 0;
        for (int i = r->col - 2; i <= r->col + 3 && g_nmissing < 16; i++)
            if (i >= 0 && i < r->n && i != r->col + 1)
                snprintf(g_missing[g_nmissing++], 24, "%s", xui_cat_product(&cat, mm.games[r->idx[i]].id));
        g_seq++;   /* catálogo: capas mudaram */
        scene("07_placeholder_sem_capa");
        g_nmissing = 0;
        g_seq++;   /* catálogo: capas mudaram */
    }
    /* sessão aberta (jogo continua no servidor) */
    mm.row = 0; mm.rows[0].col = 1; mm_rows_fix(&mm);
    {
        const mm_game_t *g = mm_focus_game(&mm);
        D.session_id = g->id;
        D.session_name = g->name;
        D.hint = "A:Voltar ao jogo|Y:Atualizar";
        scene("08_sessao_aberta");
        press(1, 0, BN_FOCUS);
        D.hint = "A:Jogar|Y:Atualizar";
        D.notice = "Ao abrir outro jogo, a sessão atual é encerrada.";
        scene("09_sessao_aberta_outro_foco");
        D.session_id = D.session_name = NULL;
        D.notice = NULL;
    }
    /* botões do topo + confirmação */
    press(0, -1, BN_FOCUS);
    scene("10_topo_reiniciar");
    press(1, 0, BN_FOCUS);
    scene("11_topo_desligar");
    /* v0.20.1: Atualizar (à esquerda) com selo de versão nova */
    D.update_avail = 1;
    D.update_label = "Atualizar: v0.21.0";
    press(-2, 0, BN_FOCUS);
    scene("23_topo_atualizar_nova_versao");
    press(2, 0, BN_FOCUS);
    D.update_label = NULL;
    mm_press_a(&mm);
    scene("12_confirmar_desligar");
    press(1, 0, BN_FOCUS);
    scene("13_confirmar_desligar_sim");
    mm_press_b(&mm);
    press(-1, 0, BN_FOCUS);
    mm_press_a(&mm);
    scene("14_confirmar_reiniciar");
    mm_press_b(&mm);
    mm_press_b(&mm);
    settle(-1);

    /* telas de estado */
    const char *gid = mm_focus_game(&mm) ? mm_focus_game(&mm)->id : NULL;
    xui_status_t s;
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_LOGIN; s.title = "Entre na sua conta Microsoft";
    s.text = "Pra jogar no orbita, use o celular ou o computador e acesse:"; s.url = "microsoft.com/link"; s.code = "K7Q2-M4XP";
    s.detail = "Aguardando você confirmar o login…  O login fica salvo no pendrive.";
    s.progress = -1;   /* v0.20.0: sem "B: usar o catálogo do PC" (modo PC saiu) */
    status_scene("15_login", &s);
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_LOADING; s.title = "Carregando o catálogo";
    s.text = "Buscando os jogos disponíveis na nuvem para a sua conta."; s.detail = "Nomes e capas: 120 de 412";
    s.progress = 29;
    status_scene("16_carregando_catalogo", &s);
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_QUEUE; s.title = "Na fila";
    s.text = "Os servidores estão cheios. Você entra no jogo assim que chegar a sua vez.";
    s.detail = "Posição 12  ·  espera estimada: 3 min"; s.hint = "B:Sair da fila"; s.progress = -1;
    s.game_id = gid;
    status_scene("17_fila", &s);
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_STARTING; s.title = "Iniciando o jogo";
    s.text = "Preparando o console na nuvem. O vídeo aparece sozinho.";
    s.detail = "No jogo: segure Back + Start por 2 s para voltar ao menu."; s.hint = "B:Cancelar";
    s.progress = -1; s.game_id = gid;
    status_scene("18_iniciando", &s);
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_ERROR; s.title = "Não foi possível iniciar";
    s.text = "O xCloud recusou a sessão (HTTP 403). Verifique se a sua assinatura inclui jogos na nuvem.";
    s.detail = "código: XST-403"; s.hint = "A:Tentar de novo|B:Voltar"; s.progress = -1;
    status_scene("19_erro", &s);
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_BUSY; s.title = "Desligando…";
    s.text = "Encerrando a sessão na nuvem antes de desligar (até 3 s).";
    s.progress = -1;
    status_scene("20_desligando", &s);
    /* v0.20.1: atualização (= main.c update_status) */
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_LOADING; s.title = "Atualizar para v0.21.0 (beta)";
    s.text = "Correções do áudio, menu mais rápido e atualização pelo próprio Xbox.";
    s.detail = "4831 KB  ·  você está na v0.20.1"; s.hint = "A:Instalar|B:Voltar"; s.progress = 0;
    status_scene("24_atualizar_oferta", &s);
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_LOADING; s.title = "Baixando a v0.21.0";
    s.text = "O download continua de onde parou se for interrompido.";
    s.detail = "2304 de 4831 KB"; s.hint = "B:Cancelar"; s.progress = 47;
    status_scene("25_atualizando", &s);
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_STARTING; s.title = "Atualizado para v0.21.0";
    s.text = "Reinicie o Xbox para usar a versão nova."; s.hint = "A:Reiniciar|B:Depois"; s.progress = 100;
    status_scene("26_atualizado", &s);
    D.update_avail = 0;

    /* ---- v0.20.3: atualização obrigatória / opcional (popup da entrada, = main.c) ---- */
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_LOADING; s.title = "Atualização obrigatória (beta)";
    s.text = "Chave beta presa ao console, Configurações no topo e correções do áudio.";
    s.detail = "v0.20.3  »  v0.21.0  ·  4831 KB"; s.hint = "A:Atualizar agora"; s.progress = 0;
    status_scene("35_atualizacao_obrigatoria", &s);
    s.title = "Nova versão disponível (beta)"; s.hint = "A:Atualizar agora|B:Depois";
    status_scene("36_atualizacao_opcional", &s);
    memset(&s, 0, sizeof(s));
    s.kind = XUI_ST_STARTING; s.title = "Atualizado para v0.21.0";
    s.text = "Reinicie o Xbox para usar a versão nova."; s.hint = "A:Reiniciar"; s.progress = 100;
    status_scene("37_obrigatoria_reinicie", &s);

    /* ---- v0.20.3: chave beta ---- */
    {
        static const char *AL = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
        xui_key_t k;
        memset(&k, 0, sizeof(k));
        k.title = "Chave beta";
        k.text = "Pegue sua chave na área do testador em x360.pistatrack.com e digite aqui "
                 "(ela fica presa a este console). Ou grave XC360/BETAKEY.TXT no pendrive pelo PC e aperte Y.";
        k.alphabet = AL; k.console_id = "A1B2-C3D4-E5F6";
        k.typed = "7KQ2M-4X"; k.len = 7; k.row = 2; k.col = 5;
        k.hint = "A:Digitar|B:Apagar|Y:Ler do pendrive|Start:Confirmar";
        key_scene("28_chave_beta", &k);
        k.typed = ""; k.len = 0; k.row = 0; k.col = 0;
        k.error = "Chave inválida. Confira e digite de novo.";
        key_scene("29_chave_beta_invalida", &k);
        k.error = NULL; k.typed = "7KQ2M-4XP9H"; k.len = 10; k.row = 4; k.col = 2;
        key_scene("30_chave_beta_completa", &k);
        k.busy = 1; k.status = "Validando…"; k.hint = NULL;
        key_scene("31_chave_validando", &k);

        memset(&s, 0, sizeof(s));
        s.kind = XUI_ST_LOADING; s.title = "Validando sua chave beta";
        s.text = "Conferindo a chave deste console no servidor do orbita.";
        s.detail = "ID deste console: A1B2-C3D4-E5F6"; s.progress = -1;
        status_scene("32_validando_boot", &s);
        memset(&s, 0, sizeof(s));
        s.kind = XUI_ST_ERROR; s.title = "Chave bloqueada";
        s.text = "Esta chave é exclusiva de um console e foi bloqueada por uso em outro aparelho. "
                 "Fale com o administrador.";
        s.detail = "ID deste console: A1B2-C3D4-E5F6"; s.hint = "A:Digitar outra chave"; s.progress = -1;
        status_scene("33_chave_duplicada", &s);
        s.title = "Chave revogada."; s.text = "Fale com o administrador.";
        status_scene("34_chave_revogada", &s);
        s.title = "Não foi possível validar sua chave (sem conexão)";
        s.text = "Confira o cabo de rede e tente de novo. Sem conexão o orbita só abre se a chave "
                 "foi validada neste console nos últimos 7 dias.";
        s.hint = "A:Tentar de novo|Y:Outra chave";
        status_scene("38_chave_sem_conexao", &s);

        xui_settings_t st;
        memset(&st, 0, sizeof(st));
        st.console_short = "A1B2-C3D4-E5F6"; st.console_full = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        st.key_masked = "*****-**P9H"; st.key_status = "validada em 25/10/2026 22:56";
        st.version = "orbita v0.20.3  ·  versão beta";
        st.note = "Para reportar um bug, envie um print desta tela junto.";
        st.button = "Trocar chave"; st.hint = "A:Trocar chave|B:Voltar";
        settings_scene("39_configuracoes", &st);
        st.notice = "Chave trocada";
        settings_scene("40_configuracoes_chave_trocada", &st);
        /* v0.21.0: Wi-Fi (WIFI.md) */
        st.notice = NULL;
        st.button2 = "Wi-Fi";
        st.button2_note = "Wi-Fi: Casa do Sérgio";
        st.focus = 1;
        st.hint = "A:Abrir|B:Voltar";
        settings_scene("41_configuracoes_wifi", &st);
        {
            static const xui_wifi_net_t nets[] = {
                { "Casa do Sérgio", 4, 1, "Conectada", 0 },
                { "Café da Esquina", 3, 0, NULL, 0 },
                { "VIVO-8F2A", 3, 1, NULL, 0 },
                { "NET_2G5A1B", 2, 1, "Salva", 0 },
                { "Vizinho 5G WPA3", 2, 1, "Não suportada", 1 },
                { "Escritório (802.1X)", 1, 1, "Não suportada", 1 },
                { "Claro_WIFI", 1, 1, NULL, 0 },
                { "TP-Link_Extender", 1, 0, NULL, 0 },
            };
            xui_wifi_t w;
            memset(&w, 0, sizeof(w));
            w.status_title = "Wi-Fi: Casa do Sérgio";
            w.status_text = "Conectado · IP 192.168.0.23 · sinal ótimo (-48 dBm)";
            w.status_kind = XUI_WS_OK;
            w.saved = "Casa do Sérgio";
            w.can_forget = 1;
            w.nets = nets;
            w.nnets = 8;
            w.focus = 1;
            w.hint = "A:Conectar|Y:Procurar|B:Voltar";
            wifi_scene("42_wifi_lista", &w);
            w.focus = 7;
            wifi_scene("43_wifi_lista_rolada", &w);
            w.focus = -1;
            wifi_scene("44_wifi_foco_esquecer", &w);
            w.nnets = 0;
            w.scanning = 1;
            w.focus = -2;
            w.status_title = "Desconectado";
            w.status_text = "Escolha uma rede para conectar.";
            w.status_kind = XUI_WS_NEUTRAL;
            w.saved = NULL;
            w.can_forget = 0;
            wifi_scene("45_wifi_procurando", &w);
            w.scanning = 0;
            w.nets = nets;
            w.nnets = 4;
            w.focus = 0;
            w.status_title = "Conectando a VIVO-8F2A";
            w.status_text = "Handshake WPA2…";
            w.status_kind = XUI_WS_BUSY;
            wifi_scene("46_wifi_conectando", &w);
            w.status_title = "Não conectou";
            w.status_text = "Senha incorreta (ou o roteador recusou o WPA2). Escolha a rede e digite de novo.";
            w.status_kind = XUI_WS_ERROR;
            wifi_scene("47_wifi_senha_errada", &w);
            w.nnets = 0;
            w.focus = -2;
            w.status_title = "Cabo de rede";
            w.status_text = "O cabo pegou rede no boot e continua sendo usado.";
            w.status_kind = XUI_WS_NEUTRAL;
            w.empty = "Wi-Fi não encontrado neste console (só Xbox 360 S/E com Wi-Fi interno).";
            w.hint = "B:Voltar";
            wifi_scene("48_wifi_sem_adaptador", &w);

            xui_textkey_t k;
            memset(&k, 0, sizeof(k));
            static const char *lower[4] = { "1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm-_." };
            static const char *sym[4] = { "1234567890", "!\"#$%&'()*", "+,/:;<=>?[", "\\]^`{|}~-_" };
            k.title = "Senha do Wi-Fi";
            k.text = "Rede: VIVO-8F2A. A senha não é gravada: o pendrive guarda só a chave derivada (WPA2).";
            for (int i = 0; i < 4; i++) k.rows[i] = lower[i];
            k.shift_label = "ABC";
            k.sym_label = "#+=";
            k.typed = "minhasenha";
            k.len = 10; k.minlen = 8; k.maxlen = 63;
            k.row = 1; k.col = 4;
            k.hint = "A:Digitar|B:Apagar|Y:Maiúsculas|X:Mostrar|Start:Conectar";
            textkey_scene("49_wifi_senha", &k);
            k.show = 1;
            for (int i = 0; i < 4; i++) k.rows[i] = sym[i];
            k.sym_label = "abc";
            k.typed = "Minha senha d0 W1-Fi !@#";
            k.len = 24;
            k.row = 4; k.col = 1;
            textkey_scene("50_wifi_senha_visivel_simbolos", &k);
            k.show = 0;
            k.typed = "abc";
            k.len = 3;
            k.row = 5; k.col = 1;
            textkey_scene("51_wifi_senha_curta", &k);
            k.typed = "minhasenha";
            k.len = 10;
            k.busy = 1;
            k.status = "Conectando a VIVO-8F2A…";
            k.hint = NULL;
            textkey_scene("52_wifi_senha_conectando", &k);

            /* v0.21.5: boot sem cabo (wifi/xw_boot.h) */
            xui_status_t bs;
            memset(&bs, 0, sizeof(bs));
            bs.kind = XUI_ST_LOADING;
            bs.title = "Conectando ao Wi-Fi “Casa do Sérgio”";
            bs.text = "Etapa: conferindo a senha (WPA2).";
            bs.detail = "Sem internet pelo cabo · 6 s (até 30 s, depois a lista de redes)";
            bs.hint = "B:Escolher outra rede";
            bs.progress = -1;
            status_scene("53_boot_conectando_wifi", &bs);
            memset(&w, 0, sizeof(w));
            w.status_title = "Não conectou";
            w.status_text = "Rede não encontrada. Escolha uma rede para conectar.";
            w.status_kind = XUI_WS_ERROR;
            w.saved = "Casa do Sérgio";
            w.can_forget = 1;
            w.nets = nets;
            w.nnets = 4;
            w.focus = 2;
            w.hint = "A:Conectar|Y:Procurar|B:Seguir sem internet";
            wifi_scene("54_boot_escolher_wifi", &w);
            memset(&bs, 0, sizeof(bs));
            bs.kind = XUI_ST_ERROR;
            bs.title = "Sem internet: conecte o cabo de rede";
            bs.text = "Este Xbox não tem Wi-Fi interno (só o Xbox 360 S/E tem). Conecte o cabo de rede no Xbox e no "
                      "roteador e aperte A.";
            bs.detail = "Cabo: desconectado";
            bs.hint = "A:Tentar de novo|B:Seguir sem internet";
            bs.progress = -1;
            status_scene("55_boot_sem_internet", &bs);
            memset(&bs, 0, sizeof(bs));
            bs.kind = XUI_ST_LOADING;
            bs.title = "Procurando internet pelo cabo…";
            bs.text = "Conecte o cabo de rede no Xbox e no roteador.";
            bs.progress = -1;
            status_scene("56_boot_procurando_cabo", &bs);
            memset(&bs, 0, sizeof(bs));
            bs.kind = XUI_ST_LOADING;
            bs.title = "Procurando o Wi-Fi interno";
            bs.text = "Sem internet pelo cabo. Ligando o adaptador Wi-Fi do Xbox…";
            bs.progress = -1;
            status_scene("57_boot_procurando_wifi", &bs);
        }
    }
    /* engrenagem no topo (só com chave válida) */
    xui_view_invalidate(V);
    mm.settings_on = 1; D.settings_avail = 1;
    for (int i = 0; i < 8 && mm.row != MM_ROW_HEADER; i++) press(0, -1, -1);
    for (int i = 0; i < 4; i++) press(-1, 0, -1);
    scene("27_topo_configuracoes");
    press(0, 1, -1);
    mm.settings_on = 0; D.settings_avail = 0; mm_rows_fix(&mm);
    settle(-1);

    /* catálogo vazio + sem nenhuma imagem */
    xui_view_invalidate(V);
    {
        static const char *e[] = { "GAME\tE1\tAvowed\t1", "GAME\tE2\tHi-Fi RUSH\t1", "GAME\tE3\tPentiment\t0",
                                   "GAME\tE4\tSouth of Midnight\t0", "GAME\tE5\tAra: História Não Contada\t0" };
        g_no_images = 1;
        for (int i = 0; i < 5; i++) { char b[128]; snprintf(b, sizeof(b), "%s", e[i]); mm_handle_line(&mm, b); }
        mm_handle_line(&mm, (char[]){ "END" });
        g_seq++;   /* catálogo: capas mudaram */
        scene("21_sem_imagens");
        mm_handle_line(&mm, (char[]){ "END" });
        scene("22_catalogo_vazio");
    }

    /* ---- bench: tela inteira forçada e só a cópia em tiles ---- */
    for (int i = 0; i < 20; i++) {
        xui_view_repaint(V);
        double t0 = now_s();
        int anim;
        sync();
        xui_rect_t r = xui_home_frame(V, &C, &D, NULL, t_ms, &anim);
        xui_tile_copy(fb, W, W, H, &C, r);
        bench_add(BN_FULL, now_s() - t0, r);
        xui_rect_t all = { 0, 0, W, H };
        t0 = now_s();
        xui_tile_copy(fb, W, W, H, &C, all);
        bench_add(BN_TILE_FULL, now_s() - t0, all);
    }
    /* peças caras isoladas: escalar 1 capa (cache miss) e a imagem do hero */
    for (int i = 0; i < nfx && i < 20; i++) {
        static uint32_t card[160 * 240];
        static uint32_t hero[1280 * 720];
        static uint32_t big[1920 * 1080];
        xui_rect_t a1 = { 0, 0, 148, 222 }, a2 = { 0, 0, 1280, 500 }, a3 = { 0, 0, 160, 240 };
        if (fx[i].cover.px) {
            xui_image_t ci = { fx[i].cover.px, fx[i].cover.w, fx[i].cover.h, fx[i].cover.w, XUI_FMT_UIRGB };
            double t0 = now_s();
            xui_scale_cover(card, 148, 222, &ci);
            bench_add(BN_CARD, now_s() - t0, a1);
            t0 = now_s();
            xui_scale_cover(card, 160, 240, &ci);
            bench_add(BN_FOCCARD, now_s() - t0, a3);
        }
        if (fx[i].hero.px) {
            xui_image_t hi = { fx[i].hero.px, fx[i].hero.w, fx[i].hero.h, fx[i].hero.w, XUI_FMT_UIRGB };
            double t0 = now_s();
            xui_scale_cover_part(hero, 1280, 720, &hi, 500);
            bench_add(BN_HEROIMG, now_s() - t0, a2);
            /* hero 1920x1080 (se o catálogo mudar o tamanho): bilinear 1.5x */
            xui_scale_cover(big, 1920, 1080, &hi);
            xui_image_t bi = { big, 1920, 1080, 1920, XUI_FMT_XRGB };
            t0 = now_s();
            xui_scale_cover_part(hero, 1280, 720, &bi, 500);
            bench_add(BN_HEROBIG, now_s() - t0, a2);
        }
    }
    xui_stats_t st;
    xui_view_stats(V, &st);
    char bp[700];
    snprintf(bp, sizeof(bp), "%s/bench.txt", outdir);
    FILE *bf = fopen(bp, "w");
    for (int pass = 0; pass < 2; pass++) {
        FILE *o = pass ? bf : stdout;
        if (!o) continue;
        fprintf(o, "custo por quadro no host (desenho + copia em tiles), -O2:\n");
        fprintf(o, "%-48s %6s %9s %9s %10s\n", "quadro", "n", "media ms", "max ms", "area px");
        for (int k = 0; k < BN_COUNT; k++) {
            if (!B[k].n) continue;
            fprintf(o, "%-48s %6d %9.3f %9.3f %10.0f\n", B[k].name, B[k].n, B[k].sum / B[k].n * 1e3,
                    B[k].max * 1e3, B[k].area / B[k].n);
        }
        fprintf(o, "quadros desenhados %u (inteiros %u, parciais %u), heros %u, capas escaladas %u\n",
                st.frames, st.full, st.partial, st.hero_builds, st.card_scales);
    }
    if (bf) fclose(bf);
    xui_view_free(V);
    printf("mockup: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
