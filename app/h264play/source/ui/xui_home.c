/*
 * XCloud360 — XUI: tela de jogos (estilo de loja de cloud gaming) + telas de
 * estado. v0.20.1: marca orbita (brand/BRAND.md) no topo; sem marcas da
 * Microsoft na tela (logo "bola com X" e selo GAME PASS saíram).
 *
 * Modelo de desenho: tudo é uma função "render(clip)" que pinta a tela
 * inteira mas só dentro do clip (as primitivas pulam o resto). O *_frame
 * decide o retângulo sujo comparando o estado com o do quadro anterior:
 *   - foco anda na fileira / rolagem horizontal animando: só a faixa da
 *     fileira + o texto do hero (~30% da tela);
 *   - foco nos botões do topo: só o topo;
 *   - troca de fileira (rolagem vertical animada), hero novo, catálogo/imagens
 *     novos, diálogo abrindo: tela inteira.
 * O hero (imagem do jogo em foco escurecida + degradês) é uma camada pronta
 * (bg) refeita só quando o foco PARA num jogo (HERO_DELAY_MS): segurar o
 * direcional não escala imagem nenhuma. Capas são escaladas uma vez pro
 * tamanho do card e ficam num cache LRU.
 */
#include "xc_i18n.h"   /* v0.21.38 */
#include "xui.h"
#include "xui_assets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- layout 1280x720 (área segura de TV ~5%: margem 64) ---- */
#define MX            64
#define LOGO_Y        22
#define HERO_TITLE_Y  210        /* linha de base do nome do jogo */
#define HERO_META_Y   244
#define HERO_NOTE_Y   270
#define ROWS_CLIP_Y   280        /* fileiras não sobem acima disto */
#define ROW_Y0        312        /* linha de base do título da fileira em foco */
#define ROW_PITCH     284
#define CARD_DY       26         /* título -> topo do card (o foco cresce 9 px pra cima) */
#define CARD_W        148
#define CARD_H        222
#define CARD_GAP      14
#define CARD_PITCH    (CARD_W + CARD_GAP)
#define CARD_R        4
#define FOC_W         160        /* foco: +8% */
#define FOC_H         240
#define FOC_R         6
#define FRAME_T       3
#define HB_R          20         /* botões redondos do topo */
#define HB_GAP        14
#define HDR_H         104
#define HERO_DELAY_MS 220
#define ANIM_TAU_MS   55.0f
#define CACHE_N       96         /* capas já escaladas (148x222x4 = 128 KB cada) */
#define PREFETCH_PER_FRAME 2
#define MAX_ROWS      16

typedef struct {
    char            id[40];
    const uint32_t *src;
    unsigned        gen, used;
    uint32_t       *px;
} card_cache_t;

struct xui_view {
    int       w, h;
    uint32_t *bg;               /* camada do hero (w*h) */
    uint32_t *tmp;              /* rascunho p/ blur */
    char      hero_id[40];      /* jogo da camada bg ("" = nenhum) */
    unsigned  hero_gen;
    int       hero_valid;
    card_cache_t cache[CACHE_N];
    card_cache_t focus;         /* capa em tamanho de foco */
    unsigned  tick;
    /* animação */
    float     sx[MAX_ROWS], sx_t[MAX_ROWS];   /* rolagem horizontal (px) */
    float     vy, vy_t;                       /* rolagem vertical (px) */
    unsigned  last_ms;
    int       have_time;
    /* estado do quadro anterior (sujeira) */
    int       valid, force;
    const xui_row_t *rows;
    int       nrows, row, col;
    unsigned  img_gen, rows_gen;
    char      sess[40], notice[160], hint[160], updlab[64];
    int       upd;                       /* v0.20.1: selo do Atualizar desenhado */
    int       set;                       /* v0.20.3: botão Configurações desenhado */
    int       netk;                      /* v0.21.29: ícone da conexão desenhado (0/1/2) */
    int       conf_open, conf_focus;
    const char *conf_title;
    char      focus_id[40];     /* último jogo em foco (hero/título) */
    char      focus_name[96];
    unsigned  focus_flags;
    unsigned  focus_ms;
    /* status */
    int       st_valid, st_kind, st_prog, st_phase;
    char      st_sig[600];
    xui_stats_t stats;
};

xui_view_t *xui_view_new(int w, int h) {
    xui_view_t *v = (xui_view_t *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->w = w; v->h = h;
    v->bg = (uint32_t *)malloc((size_t)w * h * 4);
    v->tmp = (uint32_t *)malloc(64 * 36 * 4);
    v->focus.px = (uint32_t *)malloc(FOC_W * FOC_H * 4);
    int ok = v->bg && v->tmp && v->focus.px;
    for (int i = 0; i < CACHE_N && ok; i++) {
        v->cache[i].px = (uint32_t *)malloc(CARD_W * CARD_H * 4);
        if (!v->cache[i].px) ok = 0;
    }
    if (!ok) { xui_view_free(v); return NULL; }
    return v;
}

void xui_view_free(xui_view_t *v) {
    if (!v) return;
    free(v->bg); free(v->tmp); free(v->focus.px);
    for (int i = 0; i < CACHE_N; i++) free(v->cache[i].px);
    free(v);
}

void xui_view_repaint(xui_view_t *v) { v->force = 1; v->st_valid = 0; }

void xui_view_invalidate(xui_view_t *v) {
    v->valid = 0;
    v->st_valid = 0;
    v->hero_valid = 0;
}

void xui_view_stats(const xui_view_t *v, xui_stats_t *out) { *out = v->stats; }

static void scopy(char *d, size_t n, const char *s) {
    size_t i = 0;
    if (s) for (; s[i] && i + 1 < n; i++) d[i] = s[i];
    d[i] = 0;
}

/* ------------------------------------------------------------ imagens */

static const uint32_t *cached_card(xui_view_t *v, card_cache_t *e, int ecount, const char *id,
                                   const xui_image_t *img, unsigned gen, int w, int h) {
    card_cache_t *hit = NULL, *lru = &e[0];
    for (int i = 0; i < ecount; i++) {
        if (e[i].id[0] && strcmp(e[i].id, id) == 0) { hit = &e[i]; break; }
        if (e[i].used < lru->used) lru = &e[i];
    }
    if (hit && hit->src == img->px && hit->gen == gen) { hit->used = ++v->tick; return hit->px; }
    if (!hit) hit = lru;
    scopy(hit->id, sizeof(hit->id), id);
    hit->src = img->px;
    hit->gen = gen;
    hit->used = ++v->tick;
    xui_scale_cover(hit->px, w, h, img);
    v->stats.card_scales++;
    return hit->px;
}

/* Camada do hero pro jogo id (NULL = só o fundo). */
static void build_hero(xui_view_t *v, const xui_home_data_t *d, const char *id) {
    xui_canvas_t c;
    xui_canvas_init(&c, v->bg, v->w, v->h, v->w);
    const xui_image_t *hero = (id && d && d->hero) ? d->hero(d->u, id) : NULL;
    const xui_image_t *cov = (!hero && id && d && d->cover) ? d->cover(d->u, id) : NULL;
    int img_h = 500;   /* abaixo disto é fundo liso (nem escala) */
    if (hero) {
        xui_scale_cover_part(v->bg, v->w, v->h, hero, img_h);
        xui_fill_a(&c, 0, 0, v->w, img_h, 0, 70);                    /* escurece um pouco */
    } else if (cov) {
        /* sem arte de hero: a capa reduzida a 64x36 e ampliada = borrão de cor */
        xui_scale_cover(v->tmp, 64, 36, cov);
        xui_image_t t = { v->tmp, 64, 36, 64, XUI_FMT_XRGB };
        xui_scale_cover_part(v->bg, v->w, v->h, &t, img_h);
        xui_fill_a(&c, 0, 0, v->w, img_h, XUI_C_BG, 120);
    } else {
        xui_fill(&c, 0, 0, v->w, v->h, XUI_C_BG);
        xui_vgrad(&c, 0, 0, v->w, 360, XUI_C_PANEL, 150, 0);
    }
    if (hero || cov) {
        xui_vgrad(&c, 0, 0, v->w, 130, 0, 150, 0);                    /* topo: lê o cabeçalho */
        xui_hgrad(&c, 0, 0, 820, img_h, XUI_C_BG, 200, 0);           /* esquerda: lê o título */
        xui_vgrad(&c, 0, 190, v->w, img_h - 190, XUI_C_BG, 0, 255);  /* desce pro fundo */
        xui_fill(&c, 0, img_h, v->w, v->h - img_h, XUI_C_BG);
    }
    scopy(v->hero_id, sizeof(v->hero_id), id ? id : "");
    v->hero_gen = d ? d->img_gen : 0;
    v->hero_valid = 1;
    v->stats.hero_builds++;
}

/* ------------------------------------------------------------ pedaços */

static uint32_t btn_color(char b) {
    switch (b) {
    case 'A': return XUI_C_GREEN;
    case 'B': return XUI_C_RED;
    case 'X': return XUI_C_BLUE;
    case 'Y': return XUI_C_YELLOW;
    default:  return XUI_C_BUTTON;
    }
}

/* "A:Jogar|Y:Atualizar" -> bolinhas coloridas + texto. measure=1 só mede. */
static int draw_hints(xui_canvas_t *c, int x, int base, const char *h, int measure) {
    int x0 = x;
    char seg[96];
    while (h && *h) {
        const char *e = strchr(h, '|');
        size_t n = e ? (size_t)(e - h) : strlen(h);
        if (n >= sizeof(seg)) n = sizeof(seg) - 1;
        memcpy(seg, h, n);
        seg[n] = 0;
        h = e ? e + 1 : NULL;
        const char *txt = seg;
        char b = 0;
        if (seg[0] && seg[1] == ':') { b = seg[0]; txt = seg + 2; }
        else if (strncmp(seg, "Start:", 6) == 0) { b = 'S'; txt = seg + 6; }
        else {
            /* v0.21.33: teclas de mais de uma letra numa pílula (LB/RB, Guia, D-pad, Esq/Dir) */
            static const char *const pills[] = { "LB/RB:", "Guia:", "D-pad:", "Esq/Dir:" };
            for (int i = 0; i < 4; i++) {
                size_t pl = strlen(pills[i]);
                if (strncmp(seg, pills[i], pl) == 0) {
                    char key[12];
                    memcpy(key, seg, pl - 1);
                    key[pl - 1] = 0;
                    int pw = xui_text_width(XUI_F_LABEL, key, 0) + 16;
                    if (!measure) {
                        xui_round_rect(c, x, base - 15, pw, 20, 10, XUI_C_BUTTON, 255);
                        xui_text_center(c, XUI_F_LABEL, x, pw, base, XUI_C_TEXT, key, 0);
                    }
                    x += pw + 7;
                    txt = seg + pl;
                    break;
                }
            }
        }
        if (b) {
            if (!measure) {
                if (b == 'S') {
                    xui_round_rect(c, x, base - 15, 36, 20, 10, XUI_C_BUTTON, 255);
                    xui_text_center(c, XUI_F_LABEL, x, 36, base, XUI_C_TEXT, "Start", 0);
                } else {
                    xui_circle(c, x + 10, base - 5, 10, btn_color(b), 255);
                    char l[2] = { b, 0 };
                    xui_text_center(c, XUI_F_LABEL, x, 20, base, b == 'Y' ? 0 : XUI_C_TEXT, l, 0);
                }
            }
            x += (b == 'S' ? 36 : 20) + 7;
        }
        x = measure ? x + xui_text_width(XUI_F_LABEL, txt, 0)
                    : xui_text(c, XUI_F_LABEL, x, base, XUI_C_DIM, txt, 0, 0);
        x += 22;
    }
    return x - x0 - (x > x0 ? 22 : 0);
}

static void draw_logo(xui_canvas_t *c, const char *version) {
    /* v0.20.1: logo orbita, 32 px de altura em (64, 21) (BRAND.md) */
    xui_sprite(c, &xui_logo, MX, LOGO_Y - 1);
    /* v0.20.0: versão do app 12 px depois do logo, em Haze */
    if (version && version[0])
        xui_text(c, XUI_F_LABEL, MX + xui_logo.w + 12, LOGO_Y + 21, XUI_C_HAZE, version, 0, 200);
}

/* v0.21.29: ícone + rótulo da conexão depois da versão, na linha do logo.
 * Cabo: plugue RJ45; Wi-Fi: 3 barras; sem conexão: barras apagadas + ponto vermelho. */
static void draw_net(xui_canvas_t *c, int x, int kind) {
    int base = LOGO_Y + 21;   /* mesma linha de base da versão */
    int y = base - 14;        /* topo do ícone (14 px) */
    const char *lab;
    uint32_t col;
    if (kind == 1) {
        col = XUI_C_ACCENT;
        lab = L("Cabo");
        xui_round_rect(c, x, y + 2, 14, 10, 2, col, 255);      /* corpo do plugue */
        xui_fill(c, x + 4, y + 12, 6, 2, col);                  /* trava */
        for (int i = 0; i < 3; i++) xui_fill(c, x + 3 + i * 3, y + 4, 2, 4, XUI_C_BG);   /* contatos */
    } else {
        col = kind == 2 ? XUI_C_ACCENT : XUI_C_FAINT;
        lab = kind == 2 ? "Wi-Fi" : L("Sem conexão");
        for (int i = 0; i < 3; i++) {
            int h = 5 + i * 4;
            xui_fill(c, x + i * 5, y + 14 - h, 3, h, col);
        }
        if (kind != 2) xui_circle(c, x + 15, y + 3, 3, XUI_C_RED, 255);
    }
    xui_text(c, XUI_F_LABEL, x + 22, base, kind ? XUI_C_HAZE : XUI_C_RED, lab, 0, 0);
}

/* v0.20.1: etiqueta "ASSINATURA" (jogo incluso na assinatura), no lugar do
 * selo GAME PASS: caixa escura + máscara clara com o texto vazado. */
static int draw_sub_label(xui_canvas_t *c, int x, int y) {
    xui_round_rect(c, x, y, xui_icon_sub.w + 8, xui_icon_sub.h + 8, 3, 0, 150);
    xui_mask(c, &xui_icon_sub, x + 4, y + 4, XUI_C_TEXT, 235);
    return xui_icon_sub.w + 8;
}

static int hb_cx(int w, int i) {   /* centro x do botão i (0 = Configurações, à esquerda) */
    return w - MX - HB_R - (XUI_HB_COUNT - 1 - i) * (2 * HB_R + HB_GAP);
}

static void draw_header(xui_canvas_t *c, const xui_home_data_t *d, int focus_btn) {
    draw_logo(c, d ? d->version : NULL);
    if (d) {
        int vx = MX + xui_logo.w + 12;
        if (d->version && d->version[0]) vx += xui_text_width(XUI_F_LABEL, d->version, 0) + 16;
        draw_net(c, vx, d->net_kind);
    }
    int cy = LOGO_Y + 15;
    int first = (d && d->settings_avail) ? XUI_HB_SETTINGS : XUI_HB_UPDATE;   /* v0.20.3 */
    for (int i = first; i < XUI_HB_COUNT; i++) {
        int cx = hb_cx(c->w, i);
        const xui_mask_t *ic = i == XUI_HB_SETTINGS ? &xui_icon_gear
                             : i == XUI_HB_UPDATE ? &xui_icon_update
                             : i == XUI_HB_REBOOT ? &xui_icon_restart : &xui_icon_power;
        int f = (i == focus_btn);
        if (f) {
            xui_circle(c, cx, cy, HB_R + 3, XUI_C_TEXT, 255);
            xui_mask(c, ic, cx - ic->w / 2, cy - ic->h / 2, XUI_C_BG, 255);
            const char *lab = i == XUI_HB_SETTINGS ? L("Configurações")
                            : i == XUI_HB_UPDATE ? (d && d->update_label ? d->update_label : L("Atualizar"))
                            : i == XUI_HB_REBOOT ? L("Reiniciar") : L("Desligar");
            int tw = xui_text_width(XUI_F_LABEL, lab, 0);
            int tx = cx - tw / 2;
            if (tx + tw > c->w - MX + 6) tx = c->w - MX + 6 - tw;
            xui_round_rect(c, tx - 10, cy + HB_R + 10, tw + 20, 26, 5, XUI_C_PANEL, 245);
            xui_text(c, XUI_F_LABEL, tx, cy + HB_R + 28, XUI_C_TEXT, lab, 0, 0);
        } else {
            xui_circle(c, cx, cy, HB_R, 0, 110);
            xui_mask(c, ic, cx - ic->w / 2, cy - ic->h / 2, XUI_C_TEXT, 230);
        }
    }
    /* v0.20.1: versão nova = selo (ponto Flare) no Atualizar */
    if (d && d->update_avail) {
        int cx = hb_cx(c->w, XUI_HB_UPDATE);
        xui_circle(c, cx + HB_R - 4, cy - HB_R + 4, 6, XUI_C_BG, 255);
        xui_circle(c, cx + HB_R - 4, cy - HB_R + 4, 5, XUI_C_FLARE, 255);
    }
    if (d && d->hint && d->hint[0]) {
        int hw = draw_hints(c, 0, 0, d->hint, 1);
        draw_hints(c, hb_cx(c->w, first) - HB_R - 28 - hw, cy + 5, d->hint, 0);
    }
}

static void draw_badge_box(xui_canvas_t *c, int x, int y, int w, int h) {
    xui_round_rect(c, x, y, w, h, 3, 0, 165);
}

static void draw_card(xui_view_t *v, xui_canvas_t *c, const xui_home_data_t *d, const xui_item_t *it,
                      int x, int y, int focused, int in_session) {
    int w = focused ? FOC_W : CARD_W, h = focused ? FOC_H : CARD_H, r = focused ? FOC_R : CARD_R;
    if (focused) { x -= (FOC_W - CARD_W) / 2; y -= (FOC_H - CARD_H) / 2; }
    /* fora do clip: nada (inclusive nada de escalar capa) */
    xui_rect_t box = { x - FRAME_T - 2, y - FRAME_T - 2, w + 2 * FRAME_T + 4, h + 2 * FRAME_T + 4 };
    if (xui_rect_empty(xui_rect_inter(box, c->clip))) return;
    const xui_image_t *img = d->cover ? d->cover(d->u, it->id) : NULL;
    if (img && img->px && img->w > 0 && img->h > 0) {
        const uint32_t *px = focused ? cached_card(v, &v->focus, 1, it->id, img, d->img_gen, w, h)
                                     : cached_card(v, v->cache, CACHE_N, it->id, img, d->img_gen, w, h);
        xui_image_t ci = { px, w, h, w, XUI_FMT_XRGB };
        xui_blit(c, &ci, x, y, r);
    } else {
        /* placeholder: cinza + nome */
        xui_round_rect(c, x, y, w, h, r, XUI_C_PH, 255);
        xui_vgrad(c, x, y + h / 2, w, h / 2, 0, 0, 60);
        /* conta as linhas sem desenhar (clip vazio) pra centralizar o bloco */
        int lh = xui_line_h(XUI_F_BODY);
        xui_canvas_t m = *c;
        xui_rect_t z = { 0, 0, 0, 0 };
        m.clip = z;
        int nl = xui_text_wrap(&m, XUI_F_BODY, x + 12, 0, w - 24, lh, 4, 1, 0, it->name);
        int top = y + (h - 28 - nl * lh) / 2 + 14;   /* 28 = ícone + respiro */
        xui_mask(c, &xui_icon_pad, x + (w - xui_icon_pad.w) / 2, top - 16, XUI_C_FAINT, 255);
        xui_text_wrap(c, XUI_F_BODY, x + 12, top + 12 + xui_face(XUI_F_BODY)->ascent, w - 24, lh, 4, 1,
                      XUI_C_TEXT, it->name);
    }
    /* selos de baixo: ASSINATURA à esquerda (só jogo da assinatura; F2P e
     * "compre para jogar" ficam sem etiqueta), controle à direita */
    if (it->flags & XUI_IT_GAMEPASS) draw_sub_label(c, x + 4, y + h - 28);
    draw_badge_box(c, x + w - 32, y + h - 28, 28, 24);
    xui_mask(c, &xui_icon_pad, x + w - 32 + 5, y + h - 28 + 6, XUI_C_TEXT, 230);
    if (in_session) {
        int tw = xui_text_width(XUI_F_LABEL, L("EM JOGO"), 1);
        xui_round_rect(c, x + 6, y + 6, tw + 16, 22, 4, XUI_C_ACCENT, 255);
        xui_text(c, XUI_F_LABEL, x + 14, y + 22, XUI_C_TEXT, L("EM JOGO"), 1, 0);
    }
    if (focused) xui_round_frame(c, x - FRAME_T - 1, y - FRAME_T - 1, w + 2 * FRAME_T + 2, h + 2 * FRAME_T + 2,
                                 r + FRAME_T + 1, FRAME_T, XUI_C_TEXT);
}

/* ------------------------------------------------------------ home */


static int vis_cards(int w) { return (w - MX) / CARD_PITCH; }

/* rolagem alvo (px) pra deixar a coluna col visível com 1 card de folga */
static float scroll_target(int w, float cur, int col, int n) {
    int vis = vis_cards(w);
    int first = (int)(cur / CARD_PITCH + 0.5f);
    if (col - first < 1) first = col - 1;
    if (col - first > vis - 2) first = col - (vis - 2);
    int maxf = n - (vis - 1);
    if (first > maxf) first = maxf;
    if (first < 0) first = 0;
    return (float)(first * CARD_PITCH);
}

static int row_title_y(const xui_view_t *v, int r) { return ROW_Y0 + (int)(r * ROW_PITCH - v->vy + 0.5f); }

static xui_rect_t row_band(const xui_view_t *v, int r) {
    int ty = row_title_y(v, r);
    xui_rect_t b = { 0, ty - 26, v->w, 26 + CARD_DY + CARD_H + (FOC_H - CARD_H) / 2 + FRAME_T + 6 };
    return b;
}

static const xui_rect_t k_hero_text = { 0, HERO_TITLE_Y - 46, 900, HERO_NOTE_Y + 10 - (HERO_TITLE_Y - 46) };

/* diálogo de confirmação por cima de tudo (home e, v0.21.33, Configurações) */
static void draw_confirm(xui_view_t *v, xui_canvas_t *c, const xui_confirm_t *cf) {
    xui_fill_a(c, 0, 0, v->w, v->h, 0, 175);
    int pw = 560, ph = 250, px = (v->w - pw) / 2, py = (v->h - ph) / 2;
    xui_round_rect(c, px, py, pw, ph, 12, XUI_C_PANEL, 255);
    xui_text(c, XUI_F_H2, px + 36, py + 62, XUI_C_TEXT, cf->title, 0, pw - 72);
    xui_text_wrap(c, XUI_F_BODY, px + 36, py + 100, pw - 72, 0, 3, 0, XUI_C_DIM, cf->text);
    int bw = (pw - 72 - 16) / 2, by = py + ph - 36 - 48;
    for (int i = 0; i < 2; i++) {
        int bx = px + 36 + i * (bw + 16);
        int f = (cf->focus == (i == 0 ? 0 : 1));
        const char *lab = i == 0 ? cf->no : cf->yes;
        xui_round_rect(c, bx, by, bw, 48, 6, f ? XUI_C_TEXT : XUI_C_BUTTON, 255);
        xui_text_center(c, XUI_F_ROW, bx, bw, by + 32, f ? XUI_C_BG : XUI_C_TEXT, lab, 0);
    }
}

static void render_home(xui_view_t *v, xui_canvas_t *c, const xui_home_data_t *d, const xui_confirm_t *cf) {
    xui_copy_from(c, v->bg, v->w, c->clip);
    /* hero: nome + linha de meta */
    if (v->focus_id[0]) {
        xui_text(c, XUI_F_TITLE, MX, HERO_TITLE_Y, XUI_C_TEXT, v->focus_name, 0, 860);
        int insess = d->session_id && strcmp(d->session_id, v->focus_id) == 0;
        int x = MX;
        if (v->focus_flags & XUI_IT_GAMEPASS) x += draw_sub_label(c, x, HERO_META_Y - 17) + 10;
        if (insess) {
            xui_circle(c, x + 6, HERO_META_Y - 5, 6, XUI_C_ACCENT, 255);
            xui_text(c, XUI_F_LABEL, x + 20, HERO_META_Y, XUI_C_TEXT,
                     L("Sessão em andamento  ·  A: voltar ao jogo"), 0, 0);
        } else {
            xui_text(c, XUI_F_LABEL, x, HERO_META_Y, XUI_C_DIM, L("Jogue na nuvem  ·  A: jogar"), 0, 0);
        }
    }
    if (d->notice && d->notice[0])
        xui_text(c, XUI_F_LABEL, MX, HERO_NOTE_Y, XUI_C_YELLOW, d->notice, 0, 1000);
    if (d->session_id && d->session_name && (!v->focus_id[0] || strcmp(d->session_id, v->focus_id) != 0)) {
        /* sessão aberta de outro jogo: lembrete discreto */
        char t[160];
        int n = 0;
        const char *pre = L("Em andamento: ");
        while (*pre && n < 100) t[n++] = *pre++;
        scopy(t + n, sizeof(t) - (size_t)n, d->session_name);
        xui_text(c, XUI_F_LABEL, MX, HERO_NOTE_Y + (d->notice && d->notice[0] ? 22 : 0), XUI_C_ACCENT, t, 0, 900);
    }
    /* fileiras (recortadas abaixo do hero) */
    xui_rect_t saved = c->clip;
    xui_rect_t rows_clip = { 0, ROWS_CLIP_Y, v->w, v->h - ROWS_CLIP_Y };
    c->clip = xui_rect_inter(saved, rows_clip);
    int any = 0;
    for (int r = 0; r < d->nrows && r < MAX_ROWS; r++) {
        const xui_row_t *row = &d->rows[r];
        if (row->n <= 0) continue;
        any = 1;
        int ty = row_title_y(v, r);
        if (ty - 30 > v->h || ty + CARD_DY + FOC_H < ROWS_CLIP_Y) continue;
        xui_rect_t band = row_band(v, r);
        if (xui_rect_empty(xui_rect_inter(band, c->clip))) continue;
        xui_text(c, XUI_F_ROW, MX, ty, XUI_C_TEXT, row->title, 0, 0);
        int cy = ty + CARD_DY;
        int first = (int)(v->sx[r] / CARD_PITCH);
        if (first > 0) first--;
        int fcol = -1;
        for (int i = first; i < row->n; i++) {
            int x = MX + i * CARD_PITCH - (int)(v->sx[r] + 0.5f);
            if (x >= v->w) break;
            if (x + CARD_W <= 0) continue;
            if (r == d->row && i == d->col) { fcol = i; continue; }   /* foco por último (por cima) */
            const xui_item_t *it = &row->items[i];
            draw_card(v, c, d, it, x, cy, 0, d->session_id && strcmp(d->session_id, it->id) == 0);
        }
        if (fcol >= 0) {
            const xui_item_t *it = &row->items[fcol];
            int x = MX + fcol * CARD_PITCH - (int)(v->sx[r] + 0.5f);
            draw_card(v, c, d, it, x, cy, 1, d->session_id && strcmp(d->session_id, it->id) == 0);
        }
    }
    if (!any) {
        xui_text(c, XUI_F_ROW, MX, ROW_Y0, XUI_C_TEXT, L("Nenhum jogo no catálogo"), 0, 0);
        xui_text(c, XUI_F_BODY, MX, ROW_Y0 + 32, XUI_C_DIM, L("Y: atualizar a lista"), 0, 0);
    }
    c->clip = saved;
    draw_header(c, d, d->row < 0 && !cf ? d->col : -1);
    if (cf) draw_confirm(v, c, cf);
}

/* Escala até `budget` capas que ainda não estão no cache (fileira em foco
 * primeiro, depois as vizinhas; só a janela que a rolagem alcança logo).
 * Retorna 1 se ainda falta alguma. */
static int prefetch(xui_view_t *v, const xui_home_data_t *d, int nrows, int budget) {
    if (!d->cover || nrows <= 0) return 0;
    int base = d->row < 0 ? 0 : d->row;
    static const int order[4] = { 0, 1, -1, 2 };   /* a de baixo+1 aparece na troca de fileira */
    int vis = vis_cards(v->w);
    for (int k = 0; k < 4; k++) {
        int r = base + order[k];
        if (r < 0 || r >= nrows) continue;
        const xui_row_t *row = &d->rows[r];
        int first = (int)(v->sx[r] / CARD_PITCH);
        int lo = first - 3, hi = first + vis + 4;
        if (lo < 0) lo = 0;
        if (hi > row->n) hi = row->n;
        for (int i = lo; i < hi; i++) {
            const xui_item_t *it = &row->items[i];
            const xui_image_t *img = d->cover(d->u, it->id);
            if (!img || !img->px || img->w <= 0) continue;
            int hit = 0;
            for (int e = 0; e < CACHE_N; e++)
                if (v->cache[e].src == img->px && v->cache[e].gen == d->img_gen &&
                    strcmp(v->cache[e].id, it->id) == 0) { hit = 1; break; }
            if (hit) continue;
            if (budget-- <= 0) return 1;
            cached_card(v, v->cache, CACHE_N, it->id, img, d->img_gen, CARD_W, CARD_H);
        }
    }
    return 0;
}

static float ease(float cur, float tgt, float dt_ms, int *moving) {
    float d = tgt - cur;
    if (fabsf(d) < 0.75f) return tgt;
    float k = 1.0f - expf(-dt_ms / ANIM_TAU_MS);
    *moving = 1;
    float n = cur + d * k;
    if (fabsf(tgt - n) < 0.75f) n = tgt;
    return n;
}

xui_rect_t xui_home_frame(xui_view_t *v, xui_canvas_t *c, const xui_home_data_t *d,
                          const xui_confirm_t *cf, unsigned now_ms, int *animating) {
    xui_rect_t none = { 0, 0, 0, 0 }, full = { 0, 0, v->w, v->h };
    xui_rect_t dirty = none;
    int fulld = 0;
    float dt = v->have_time ? (float)(now_ms - v->last_ms) : 16.f;
    if (dt > 100.f) dt = 100.f;
    v->last_ms = now_ms;
    v->have_time = 1;
    v->st_valid = 0;   /* sair da home invalida a tela de estado */

    int nrows = d->nrows < MAX_ROWS ? d->nrows : MAX_ROWS;
    if (!v->valid || v->rows != d->rows || v->nrows != nrows || v->rows_gen != d->rows_gen ||
        v->img_gen != d->img_gen) {
        fulld = 1;
        if (v->rows != d->rows || v->nrows != nrows || v->rows_gen != d->rows_gen || !v->valid) {
            /* catálogo novo: rolagens recomeçam na posição do foco */
            for (int r = 0; r < MAX_ROWS; r++) v->sx[r] = v->sx_t[r] = 0;
            for (int r = 0; r < nrows; r++) {
                int col = (r == d->row) ? d->col : 0;
                v->sx[r] = v->sx_t[r] = scroll_target(v->w, 0, col, d->rows[r].n);
            }
            v->vy = v->vy_t = (float)((d->row > 0 ? d->row : 0) * ROW_PITCH);
        }
    }
    /* jogo em foco (o botão do topo mantém o último jogo no hero) */
    const xui_item_t *fit = NULL;
    if (d->row >= 0 && d->row < nrows && d->col >= 0 && d->col < d->rows[d->row].n)
        fit = &d->rows[d->row].items[d->col];
    if (fit && strcmp(fit->id, v->focus_id) != 0) {
        scopy(v->focus_id, sizeof(v->focus_id), fit->id);
        scopy(v->focus_name, sizeof(v->focus_name), fit->name);
        v->focus_flags = fit->flags;
        v->focus_ms = now_ms;
        dirty = xui_rect_union(dirty, k_hero_text);
    } else if (!fit && d->row >= 0 && v->focus_id[0]) {
        v->focus_id[0] = 0;
        fulld = 1;
    }
    /* foco mudou de linha / botões do topo */
    if (v->valid && (d->row != v->row)) {
        if (d->row < 0 || v->row < 0) {
            xui_rect_t hdr = { 0, 0, v->w, HDR_H };
            dirty = xui_rect_union(dirty, hdr);
            if (d->row >= 0 && d->row < nrows) dirty = xui_rect_union(dirty, row_band(v, d->row));
            if (v->row >= 0 && v->row < nrows) dirty = xui_rect_union(dirty, row_band(v, v->row));
        } else {
            fulld = 1;
        }
    }
    if (v->valid && d->row == v->row && d->col != v->col) {
        if (d->row < 0) { xui_rect_t hdr = { 0, 0, v->w, HDR_H }; dirty = xui_rect_union(dirty, hdr); }
        else if (d->row < nrows) dirty = xui_rect_union(dirty, row_band(v, d->row));
    }
    /* texto/sessão/diálogo */
    char sess[40];
    scopy(sess, sizeof(sess), d->session_id);
    if (strcmp(sess, v->sess) || strcmp(d->notice ? d->notice : "", v->notice)) {
        dirty = xui_rect_union(dirty, k_hero_text);
        xui_rect_t rows = { 0, ROWS_CLIP_Y, v->w, v->h - ROWS_CLIP_Y };
        if (strcmp(sess, v->sess)) dirty = xui_rect_union(dirty, rows);
    }
    if (strcmp(d->hint ? d->hint : "", v->hint) || (d->update_avail != 0) != v->upd ||
        (d->settings_avail != 0) != v->set || d->net_kind != v->netk ||
        strcmp(d->update_label ? d->update_label : "", v->updlab)) {
        xui_rect_t hdr = { 0, 0, v->w, HDR_H };
        dirty = xui_rect_union(dirty, hdr);
    }
    int conf_open = cf != NULL;
    if (conf_open != v->conf_open || (cf && cf->title != v->conf_title)) fulld = 1;
    else if (cf && cf->focus != v->conf_focus) {
        xui_rect_t p = { (v->w - 560) / 2, (v->h - 250) / 2, 560, 250 };
        dirty = xui_rect_union(dirty, p);
    }

    /* alvos de rolagem */
    if (d->row >= 0 && d->row < nrows)
        v->sx_t[d->row] = scroll_target(v->w, v->sx_t[d->row], d->col, d->rows[d->row].n);
    v->vy_t = (float)((d->row > 0 ? d->row : 0) * ROW_PITCH);

    /* animações */
    int moving = 0;
    if (v->vy != v->vy_t) {
        v->vy = ease(v->vy, v->vy_t, dt, &moving);
        fulld = 1;
    }
    for (int r = 0; r < nrows; r++) {
        if (v->sx[r] != v->sx_t[r]) {
            int m = 0;
            v->sx[r] = ease(v->sx[r], v->sx_t[r], dt, &m);
            moving |= m;
            dirty = xui_rect_union(dirty, row_band(v, r));
        }
    }
    /* hero: só quando o foco parou num jogo e nada está rolando */
    const char *want = v->focus_id[0] ? v->focus_id : NULL;
    int hero_stale = !v->hero_valid || strcmp(v->hero_id, want ? want : "") != 0 ||
                     (want && v->hero_gen != d->img_gen);
    if (hero_stale) {
        if (!v->hero_valid || !v->valid ||
            (!moving && now_ms - v->focus_ms >= HERO_DELAY_MS)) {
            build_hero(v, d, want);
            fulld = 1;
        } else {
            moving = 1;   /* ainda vai precisar de um quadro */
        }
    }

    if (v->force) { fulld = 1; v->force = 0; }
    if (fulld) dirty = full;
    if (!xui_rect_empty(dirty)) {
        xui_set_clip(c, dirty);
        render_home(v, c, d, cf);
        xui_clip_all(c);
        v->stats.frames++;
        if (fulld) v->stats.full++; else v->stats.partial++;
    }
    /* guarda o estado */
    v->valid = 1;
    v->rows = d->rows; v->nrows = nrows; v->row = d->row; v->col = d->col;
    v->img_gen = d->img_gen;
    v->rows_gen = d->rows_gen;
    scopy(v->sess, sizeof(v->sess), sess);
    scopy(v->notice, sizeof(v->notice), d->notice);
    scopy(v->hint, sizeof(v->hint), d->hint);
    scopy(v->updlab, sizeof(v->updlab), d->update_label);
    v->upd = d->update_avail != 0;
    v->set = d->settings_avail != 0;
    v->netk = d->net_kind;
    v->conf_open = conf_open;
    v->conf_focus = cf ? cf->focus : 0;
    v->conf_title = cf ? cf->title : NULL;
    /* Parado e nada pra desenhar: adianta a escala das capas que a próxima
     * rolagem vai mostrar (fileira em foco e vizinhas), poucas por quadro,
     * pra rolagem não pagar escala de capa (cache miss) no meio. */
    if (!moving && xui_rect_empty(dirty) && prefetch(v, d, nrows, PREFETCH_PER_FRAME)) moving = 1;
    if (animating) *animating = moving;
    return dirty;
}

int xui_home_visible(const xui_view_t *v, const xui_home_data_t *d, const xui_item_t **out, int max) {
    int n = 0, nrows = d->nrows < MAX_ROWS ? d->nrows : MAX_ROWS;
    if (nrows <= 0 || max <= 0) return 0;
    int base = d->row < 0 ? 0 : (d->row >= nrows ? nrows - 1 : d->row);
    const xui_row_t *fr = &d->rows[base];
    if (d->row >= 0 && d->col >= 0 && d->col < fr->n) out[n++] = &fr->items[d->col];
    static const int order[4] = { 0, 1, -1, 2 };
    int vis = vis_cards(v->w);
    for (int k = 0; k < 4 && n < max; k++) {
        int r = base + order[k];
        if (r < 0 || r >= nrows) continue;
        const xui_row_t *row = &d->rows[r];
        float sx = r == base && d->row >= 0 ? v->sx_t[r] : v->sx[r];   /* pra onde a rolagem vai */
        int first = (int)(sx / CARD_PITCH);
        int lo = first - 1, hi = first + vis + 2;
        if (lo < 0) lo = 0;
        if (hi > row->n) hi = row->n;
        for (int i = lo; i < hi && n < max; i++) {
            if (r == base && d->row >= 0 && i == d->col) continue;
            out[n++] = &row->items[i];
        }
    }
    return n;
}

/* ------------------------------------------------------------ status */

#define ST_PW 720

static void st_layout(const xui_view_t *v, const xui_status_t *s, int *px, int *py, int *pw, int *ph) {
    *pw = ST_PW;
    *ph = s->kind == XUI_ST_LOGIN ? 372 : 250;
    *px = (v->w - *pw) / 2;
    *py = (v->h - *ph) / 2 + 30;
}

static xui_rect_t st_dots_rect(const xui_view_t *v, const xui_status_t *s) {
    int px, py, pw, ph;
    st_layout(v, s, &px, &py, &pw, &ph);
    xui_rect_t r = { px + 40, py + ph - 64, pw - 80, 40 };
    return r;
}

static void render_status(xui_view_t *v, xui_canvas_t *c, const xui_status_t *s, const char *version, int phase) {
    /* fundo: a camada do hero atual bem escurecida (continuidade com a home) */
    xui_copy_from(c, v->bg, v->w, c->clip);
    xui_fill_a(c, 0, 0, v->w, v->h, XUI_C_BG, 150);
    draw_logo(c, version);
    int px, py, pw, ph;
    st_layout(v, s, &px, &py, &pw, &ph);
    xui_round_rect(c, px, py, pw, ph, 14, XUI_C_PANEL, 240);
    int x = px + 40, y = py + 64, w = pw - 80;
    if (s->kind == XUI_ST_ERROR) {
        xui_circle(c, x + 16, y - 10, 16, XUI_C_RED, 255);
        xui_text_center(c, XUI_F_ROW, x, 32, y - 2, XUI_C_TEXT, "!", 0);
        xui_text(c, XUI_F_H2, x + 46, y, XUI_C_TEXT, s->title, 0, w - 46);
    } else {
        xui_text(c, XUI_F_H2, x, y, XUI_C_TEXT, s->title, 0, w);
    }
    y += 40;
    if (s->kind == XUI_ST_LOGIN) {
        int n = xui_text_wrap(c, XUI_F_BODY, x, y, w, 0, 2, 0, XUI_C_DIM,
                              s->text ? s->text : L("No celular ou no PC, acesse:"));
        y += n * xui_line_h(XUI_F_BODY) + 8;
        xui_text(c, XUI_F_ROW, x, y, XUI_C_TEXT, s->url, 0, w);
        y += 34;
        xui_text(c, XUI_F_BODY, x, y, XUI_C_DIM, L("e digite o código:"), 0, w);
        y += 18;
        int cw = xui_text_width(XUI_F_CODE, s->code ? s->code : "", 5);
        int bw = cw + 64, bx = px + (pw - bw) / 2;
        xui_round_rect(c, bx, y, bw, 96, 10, XUI_C_BUTTON, 255);
        xui_text(c, XUI_F_CODE, bx + 32, y + 70, XUI_C_TEXT, s->code, 5, 0);
        y += 96 + 30;
        if (s->detail) xui_text(c, XUI_F_LABEL, x, y, XUI_C_FAINT, s->detail, 0, w);
    } else {
        int n = xui_text_wrap(c, XUI_F_BODY, x, y, w, 0, 4, 0, XUI_C_DIM, s->text);
        y += n * xui_line_h(XUI_F_BODY) + 6;
        if (s->detail) xui_text(c, XUI_F_LABEL, x, y + 4, s->kind == XUI_ST_QUEUE ? XUI_C_ACCENT : XUI_C_FAINT,
                                s->detail, 0, w);
        if (s->kind != XUI_ST_ERROR) {
            xui_rect_t dr = st_dots_rect(v, s);
            if (s->progress >= 0) {
                int bw = dr.w, by = dr.y + 16;
                xui_round_rect(c, dr.x, by, bw, 6, 3, XUI_C_BUTTON, 255);
                int fw = bw * (s->progress > 100 ? 100 : s->progress) / 100;
                if (fw > 0) xui_round_rect(c, dr.x, by, fw < 6 ? 6 : fw, 6, 3, XUI_C_ACCENT, 255);
            } else {
                for (int i = 0; i < 3; i++) {
                    int on = (phase % 3) == i;
                    xui_circle(c, dr.x + 7 + i * 22, dr.y + 20, on ? 7 : 5, on ? XUI_C_ACCENT : XUI_C_FAINT, 255);
                }
            }
        }
    }
    if (s->hint && s->hint[0]) {
        int hw = draw_hints(c, 0, 0, s->hint, 1);
        draw_hints(c, px + pw - 36 - hw, py + ph + 40, s->hint, 0);
    }
}

xui_rect_t xui_status_frame(xui_view_t *v, xui_canvas_t *c, const xui_status_t *s,
                            const xui_home_data_t *d, unsigned now_ms, int *animating) {
    xui_rect_t none = { 0, 0, 0, 0 }, full = { 0, 0, v->w, v->h };
    xui_rect_t dirty = none;
    v->valid = 0;   /* voltar pra home redesenha tudo */
    v->last_ms = now_ms;
    v->have_time = 1;
    /* assinatura do conteúdo */
    char sig[600];
    int n = 0;
    const char *parts[7] = { s->title, s->text, s->url, s->code, s->detail, s->hint, s->game_id };
    for (int i = 0; i < 7; i++) {
        const char *p = parts[i] ? parts[i] : "";
        while (*p && n < (int)sizeof(sig) - 2) sig[n++] = *p++;
        sig[n++] = '\x1f';
        if (n >= (int)sizeof(sig) - 2) break;
    }
    sig[n] = 0;
    /* fundo: hero do jogo pedido, se mudou */
    const char *gid = s->game_id && s->game_id[0] ? s->game_id : NULL;
    if (!v->hero_valid || (gid && strcmp(v->hero_id, gid) != 0)) {
        build_hero(v, d, gid ? gid : (v->hero_valid ? v->hero_id : NULL));
        v->st_valid = 0;
    }
    int phase = (int)(now_ms / 300);
    if (!v->st_valid || v->st_kind != s->kind || strcmp(sig, v->st_sig) != 0) {
        dirty = full;
    } else if (s->progress != v->st_prog) {
        dirty = st_dots_rect(v, s);
    } else if (s->progress < 0 && s->kind != XUI_ST_ERROR && phase != v->st_phase) {
        dirty = st_dots_rect(v, s);
    }
    if (!xui_rect_empty(dirty)) {
        xui_set_clip(c, dirty);
        render_status(v, c, s, d ? d->version : NULL, phase);
        xui_clip_all(c);
        v->stats.frames++;
        if (dirty.w == full.w && dirty.h == full.h) v->stats.full++; else v->stats.partial++;
    }
    v->st_valid = 1;
    v->st_kind = s->kind;
    v->st_prog = s->progress;
    v->st_phase = phase;
    scopy(v->st_sig, sizeof(v->st_sig), sig);
    if (animating) *animating = s->kind != XUI_ST_ERROR && s->progress < 0;
    return dirty;
}

/* ================================================================
 * v0.20.3: chave beta (teclado na tela) e configurações
 * ================================================================ */

/* assinatura do conteúdo -> st_sig (mesmo campo das telas de estado, com
 * um tipo próprio em st_kind pra a troca de tela redesenhar tudo) */
#define ST_KIND_KEY      100
#define ST_KIND_SETTINGS 101

static void sig_cat(char *sig, int *n, int cap, const char *p) {
    if (!p) p = "";
    while (*p && *n < cap - 2) sig[(*n)++] = *p++;
    if (*n < cap - 1) sig[(*n)++] = '\x1f';
    sig[*n] = 0;
}

/* layout da tela da chave: painel largo, texto à esquerda, teclado à direita */
#define KP_PX   64
#define KP_PW   1152
#define KP_PH   452
#define KP_PY   150
#define KEY_W   60
#define KEY_H   54
#define KEY_GAP 8
#define KB_W    (XUI_KEY_COLS * KEY_W + (XUI_KEY_COLS - 1) * KEY_GAP)
#define KB_X    (KP_PX + KP_PW - 44 - KB_W)
#define KB_Y    (KP_PY + 44)
#define KL_X    (KP_PX + 44)
#define KL_W    (KB_X - 48 - KL_X)
#define KIN_Y   (KP_PY + 214)    /* caixa da chave */
#define KIN_H   84

static xui_rect_t key_dots_rect(void) {
    xui_rect_t r = { KL_X, KIN_Y + KIN_H + 12, KL_W, 40 };
    return r;
}

static void key_btn(xui_canvas_t *c, int x, int y, int w, int h, const char *lab, int focus, int accent, int dim) {
    if (focus) {
        xui_round_rect(c, x - 3, y - 3, w + 6, h + 6, 9, XUI_C_TEXT, 255);
        xui_text_center(c, w >= 100 ? XUI_F_ROW : XUI_F_H2, x, w, y + h / 2 + (w >= 100 ? 8 : 10), XUI_C_BG, lab, 0);
    } else {
        xui_round_rect(c, x, y, w, h, 7, accent ? XUI_C_ACCENT : XUI_C_BUTTON, dim ? 120 : 255);
        xui_text_center(c, w >= 100 ? XUI_F_ROW : XUI_F_H2, x, w, y + h / 2 + (w >= 100 ? 8 : 10),
                        accent ? XUI_C_BG : (dim ? XUI_C_FAINT : XUI_C_TEXT), lab, 0);
    }
}

static void render_key(xui_view_t *v, xui_canvas_t *c, const xui_key_t *k, const char *version, int phase) {
    xui_copy_from(c, v->bg, v->w, c->clip);
    xui_fill_a(c, 0, 0, v->w, v->h, XUI_C_BG, 170);
    draw_logo(c, version);
    xui_round_rect(c, KP_PX, KP_PY, KP_PW, KP_PH, 14, XUI_C_PANEL, 240);
    /* ---- esquerda: título, explicação, a chave, erro/estado, ID ---- */
    int x = KL_X, y = KP_PY + 64, w = KL_W;
    xui_text(c, XUI_F_H2, x, y, XUI_C_TEXT, k->title ? k->title : L("Chave beta"), 0, w);
    y += 38;
    int n = xui_text_wrap(c, XUI_F_BODY, x, y, w, 0, 4, 0, XUI_C_DIM, k->text);
    (void)n;
    /* caixa da chave: 10 casas + hífen fixo, a casa da vez sublinhada */
    xui_round_rect(c, x, KIN_Y, w, KIN_H, 10, XUI_C_BUTTON, 255);
    const int cell = 36, hy = 24;
    int total = 10 * cell + hy;
    int cx0 = x + (w - total) / 2, base = KIN_Y + KIN_H / 2 + 14;
    const char *t = k->typed ? k->typed : "";
    int li = 0;   /* índice de letra */
    for (const char *p = t; *p; p++) {
        if (*p == '-') continue;
        int cx = cx0 + li * cell + (li >= 5 ? hy : 0);
        char s1[2] = { *p, 0 };
        int gw = xui_text_width(XUI_F_TITLE, s1, 0);   /* sem corte ("M" e "W" passam da casa) */
        xui_text(c, XUI_F_TITLE, cx + (cell - gw) / 2, base, XUI_C_TEXT, s1, 0, 0);
        li++;
    }
    for (int i = li; i < 10; i++) {
        int cx = cx0 + i * cell + (i >= 5 ? hy : 0);
        int cur = (i == li) && !k->busy;
        xui_fill(c, cx + 6, base + 8, cell - 12, cur ? 3 : 2, cur ? XUI_C_ACCENT : XUI_C_FAINT);
    }
    /* o hífen do meio é fixo (entra sozinho) */
    xui_text_center(c, XUI_F_TITLE, cx0 + 5 * cell, hy, base - 2, k->len >= 5 ? XUI_C_TEXT : XUI_C_FAINT, "-", 0);
    /* erro / validando */
    xui_rect_t dr = key_dots_rect();
    if (k->busy) {
        xui_text(c, XUI_F_ROW, x, dr.y + 26, XUI_C_TEXT, k->status ? k->status : L("Validando…"), 0, w - 90);
        int sx = x + xui_text_width(XUI_F_ROW, k->status ? k->status : L("Validando…"), 0) + 18;
        for (int i = 0; i < 3; i++) {
            int on = (phase % 3) == i;
            xui_circle(c, sx + 7 + i * 22, dr.y + 19, on ? 7 : 5, on ? XUI_C_ACCENT : XUI_C_FAINT, 255);
        }
    } else if (k->error && k->error[0]) {
        xui_circle(c, x + 11, dr.y + 19, 11, XUI_C_RED, 255);
        xui_text_center(c, XUI_F_LABEL, x, 22, dr.y + 24, XUI_C_TEXT, "!", 0);
        xui_text_wrap(c, XUI_F_BODY, x + 32, dr.y + 25, w - 32, 0, 2, 0, XUI_C_TEXT, k->error);
    }
    if (k->console_id && k->console_id[0]) {
        xui_text(c, XUI_F_LABEL, x, KP_PY + KP_PH - 58, XUI_C_FAINT, L("ID deste console"), 0, w);
        xui_text(c, XUI_F_ROW, x, KP_PY + KP_PH - 30, XUI_C_HAZE, k->console_id, 1, w);
    }
    /* ---- direita: teclado ---- */
    const char *al = k->alphabet ? k->alphabet : "";
    int nal = (int)strlen(al);
    for (int r = 0; r < XUI_KEY_ROWS; r++) {
        for (int col = 0; col < XUI_KEY_COLS; col++) {
            int i = r * XUI_KEY_COLS + col;
            if (i >= nal) break;
            char s1[2] = { al[i], 0 };
            key_btn(c, KB_X + col * (KEY_W + KEY_GAP), KB_Y + r * (KEY_H + KEY_GAP), KEY_W, KEY_H, s1,
                    !k->busy && k->row == r && k->col == col, 0, k->busy || k->len >= 10);
        }
    }
    /* ações: Apagar (cols 0-2) / Ler do pendrive (3-5) / Confirmar (6-7) */
    int ay = KB_Y + XUI_KEY_ROWS * (KEY_H + KEY_GAP) + 10;
    int w3 = 3 * KEY_W + 2 * KEY_GAP, w2 = 2 * KEY_W + KEY_GAP;
    int ax[3] = { KB_X, KB_X + 3 * (KEY_W + KEY_GAP), KB_X + 6 * (KEY_W + KEY_GAP) };
    int aw[3] = { w3, w3, w2 };
    const char *alab[3] = { L("Apagar"), L("Do pendrive"), L("Confirmar") };
    for (int a = 0; a < 3; a++)
        key_btn(c, ax[a], ay, aw[a], KEY_H, alab[a], !k->busy && k->row == XUI_KEY_ROWS && k->col == a,
                a == 2 && k->len >= 10 && !k->busy, k->busy || (a == 0 && k->len == 0) || (a == 2 && k->len < 10));
    if (k->hint && k->hint[0]) {
        int hw = draw_hints(c, 0, 0, k->hint, 1);
        draw_hints(c, KP_PX + KP_PW - 36 - hw, KP_PY + KP_PH + 40, k->hint, 0);
    }
}

xui_rect_t xui_key_frame(xui_view_t *v, xui_canvas_t *c, const xui_key_t *k, const xui_home_data_t *d,
                         unsigned now_ms, int *animating) {
    xui_rect_t none = { 0, 0, 0, 0 }, full = { 0, 0, v->w, v->h };
    xui_rect_t dirty = none;
    v->valid = 0;
    v->last_ms = now_ms;
    v->have_time = 1;
    char sig[600], num[48];
    int n = 0;
    snprintf(num, sizeof(num), "%d/%d/%d/%d", k->row, k->col, k->len, k->busy);
    sig_cat(sig, &n, (int)sizeof(sig), num);
    sig_cat(sig, &n, (int)sizeof(sig), k->title);
    sig_cat(sig, &n, (int)sizeof(sig), k->text);
    sig_cat(sig, &n, (int)sizeof(sig), k->typed);
    sig_cat(sig, &n, (int)sizeof(sig), k->console_id);
    sig_cat(sig, &n, (int)sizeof(sig), k->error);
    sig_cat(sig, &n, (int)sizeof(sig), k->status);
    sig_cat(sig, &n, (int)sizeof(sig), k->hint);
    if (!v->hero_valid) { build_hero(v, d, NULL); v->st_valid = 0; }
    int phase = (int)(now_ms / 300);
    if (!v->st_valid || v->st_kind != ST_KIND_KEY || strcmp(sig, v->st_sig) != 0) dirty = full;
    else if (k->busy && phase != v->st_phase) dirty = key_dots_rect();
    if (!xui_rect_empty(dirty)) {
        xui_set_clip(c, dirty);
        render_key(v, c, k, d ? d->version : NULL, phase);
        xui_clip_all(c);
        v->stats.frames++;
        if (dirty.w == full.w && dirty.h == full.h) v->stats.full++; else v->stats.partial++;
    }
    v->st_valid = 1;
    v->st_kind = ST_KIND_KEY;
    v->st_prog = -1;
    v->st_phase = phase;
    scopy(v->st_sig, sizeof(v->st_sig), sig);
    if (animating) *animating = k->busy;
    return dirty;
}

/* ================================================================
 * v0.21.33: Configurações em menu lateral (MENU-SETTINGS-PLAN.md, mockups
 * 60-68). Coluna "Configurações" (312 px) com as 4 seções e ENERGIA embaixo;
 * painel da seção (820 px). Foco = pílula branca; seção aberta com o foco
 * no painel = pílula cinza com barrinha Sky.
 * ================================================================ */
#define CF_LX   64
#define CF_LY   88
#define CF_LW   312
#define CF_LH   548
#define CF_RX   (CF_LX + CF_LW + 20)
#define CF_RY   CF_LY
#define CF_RW   (1280 - 64 - CF_RX)
#define CF_RH   CF_LH
#define CF_RIN  40
#define CF_IT_H 50
#define CF_IT_P 56
#define CF_ROW_H 58
#define CF_ROW_P 66
/* calibração: círculo de 320 px = curso inteiro do analógico */
#define CF_CAL_CX (CF_RX + CF_RIN + 175)
#define CF_CAL_CY (CF_RY + 290)
#define CF_CAL_R  160
#define ST_KIND_CFG ST_KIND_SETTINGS

xui_rect_t xui_cfg_calib_rect(void) {
    /* círculo + rastro + bolinha (pode passar um pouco da borda) + a leitura embaixo;
     * pára antes dos sliders (x = CF_RX + CF_RIN - 12 + 400) */
    xui_rect_t r = { CF_CAL_CX - CF_CAL_R - 16, CF_CAL_CY - CF_CAL_R - 16, 0, 0 };
    r.w = (CF_RX + CF_RIN - 12 + 400 - 2) - r.x;
    r.h = (CF_CAL_CY + CF_CAL_R + 46) - r.y;
    return r;
}

static void cf_chevron(xui_canvas_t *c, int x, int cy, uint32_t col) {
    for (int i = 0; i < 6; i++) {
        xui_fill(c, x + i, cy - 6 + i, 3, 2, col);
        xui_fill(c, x + i, cy + 5 - i, 3, 2, col);
    }
}

static void cf_bars(xui_canvas_t *c, int x, int base, int n, uint32_t on, uint32_t off) {
    for (int i = 0; i < 4; i++) {
        int h = 5 + i * 4;
        xui_round_rect(c, x + i * 6, base - h, 4, h, 1, i < n ? on : off, 255);
    }
}

/* ícones da coluna: 0 conta, 1 rede, 2 tela, 3 controle, 4 reiniciar, 5 desligar */
static void cf_icon(xui_canvas_t *c, int kind, int cx, int cy, uint32_t col) {
    switch (kind) {
    case 0:
        xui_circle(c, cx, cy - 5, 5, col, 255);
        xui_round_rect(c, cx - 9, cy + 2, 18, 9, 5, col, 255);
        break;
    case 1: cf_bars(c, cx - 11, cy + 9, 4, col, col); break;
    case 2:
        xui_round_frame(c, cx - 9, cy - 8, 18, 12, 2, 2, col);
        xui_fill(c, cx - 1, cy + 6, 2, 3, col);
        xui_fill(c, cx - 6, cy + 9, 12, 2, col);
        break;
    case 3: xui_mask(c, &xui_icon_pad, cx - 9, cy - 6, col, 255); break;
    case 4: xui_mask(c, &xui_icon_restart, cx - 10, cy - 10, col, 255); break;
    case 7:   /* v0.21.43: Desempenho (velocímetro) */
        xui_round_frame(c, cx - 10, cy - 9, 20, 20, 10, 2, col);
        xui_fill(c, cx - 1, cy - 1, 3, 3, col);
        for (int i = 0; i < 6; i++) xui_fill(c, cx + 1 + i, cy - 2 - i, 2, 2, col);
        break;
    case 6:   /* v0.21.38: Idioma (globo) */
        xui_round_frame(c, cx - 9, cy - 9, 18, 18, 9, 2, col);
        xui_round_frame(c, cx - 4, cy - 9, 8, 18, 4, 2, col);
        xui_fill(c, cx - 9, cy - 1, 18, 2, col);
        break;
    default: xui_mask(c, &xui_icon_power, cx - 10, cy - 10, col, 255); break;
    }
}

static void cf_column(xui_canvas_t *c, const xui_cfg_t *s) {
    xui_round_rect(c, CF_LX, CF_LY, CF_LW, CF_LH, 14, XUI_C_PANEL, 245);
    xui_mask(c, &xui_icon_gear, CF_LX + 24, CF_LY + 30, XUI_C_TEXT, 255);
    xui_text(c, XUI_F_H2, CF_LX + 54, CF_LY + 48, XUI_C_TEXT, L("Configurações"), 0, CF_LW - 70);
    int x = CF_LX + 14, w = CF_LW - 28;
    /* v0.21.43: as seções rolam numa janela que acaba antes da linha do
     * ENERGIA (Reiniciar/Desligar ficam fixos); barrinha à direita */
    int sy = CF_LY + CF_LH - 14 - 2 * CF_IT_P - 18;
    int vt = CF_LY + 74, vb = sy - 34, vh = vb - vt;
    int total = XUI_CFG_NSEC * CF_IT_P;
    int off = 0;
    if (total > vh) {
        int sel = s->item < XUI_CFG_NSEC ? s->item : XUI_CFG_NSEC - 1;
        off = (sel + 1) * CF_IT_P + 8 - vh;
        if (off < 0) off = 0;
        if (off > total - vh + 8) off = total - vh + 8;
        int th = vh * vh / (total + 8), ty = vt + (vh - th) * off / (total - vh + 8);
        xui_round_rect(c, CF_LX + CF_LW - 8, vt, 3, vh, 1, XUI_C_BUTTON, 255);
        xui_round_rect(c, CF_LX + CF_LW - 8, ty, 3, th, 1, XUI_C_DIM, 255);
    }
    xui_rect_t old = c->clip, vp = { CF_LX, vt - 4, CF_LW - 10, vh + 8 };
    for (int i = 0; i < XUI_CFG_NSEC + 2; i++) {
        if (i == XUI_CFG_NSEC) c->clip = old;
        else if (i == 0) xui_set_clip(c, xui_rect_inter(old, vp));
        int y = i < XUI_CFG_NSEC ? CF_LY + 82 + i * CF_IT_P - off
                                 : CF_LY + CF_LH - 14 - (XUI_CFG_NSEC + 2 - i) * CF_IT_P + (CF_IT_P - CF_IT_H);
        if (i < XUI_CFG_NSEC && (y + CF_IT_H < vt - 4 || y > vb + 4)) continue;
        const char *lab = i < XUI_CFG_NSEC ? (s->sections[i] ? s->sections[i] : "") : i == XUI_CFG_NSEC ? L("Reiniciar") : L("Desligar");
        int focus = i == s->item && !s->right;
        int open = i == s->item && s->right;
        uint32_t fg = XUI_C_TEXT;
        if (focus) {
            xui_round_rect(c, x - 3, y - 3, w + 6, CF_IT_H + 6, 10, XUI_C_TEXT, 255);
            fg = XUI_C_BG;
        } else if (open) {
            xui_round_rect(c, x, y, w, CF_IT_H, 8, XUI_C_BUTTON, 255);
            xui_round_rect(c, x, y + 10, 4, CF_IT_H - 20, 2, XUI_C_ACCENT, 255);
        } else if (i >= XUI_CFG_NSEC) {
            fg = XUI_C_DIM;
        }
        cf_icon(c, i < XUI_CFG_NSEC ? (i == 4 ? 6 : i == 5 ? 7 : i) : i == XUI_CFG_NSEC ? 4 : 5, x + 30, y + CF_IT_H / 2, fg);
        xui_text(c, XUI_F_ROW, x + 58, y + CF_IT_H / 2 + 8, fg, lab, 0, w - 70);
    }
    c->clip = old;
    xui_text(c, XUI_F_LABEL, x + 4, sy, XUI_C_FAINT, L("ENERGIA"), 1, 200);
    xui_fill(c, x + 4, sy - 26, w - 8, 1, XUI_C_BUTTON);
}

/* uma linha de opção no painel (y = topo) */
static void cf_row(xui_canvas_t *c, int y, const xui_cfg_row_t *r, int focus) {
    int x = CF_RX + CF_RIN - 12, w = CF_RW - 2 * CF_RIN + 24;
    int dim = (r->flags & XUI_CR_DIM) != 0;
    uint32_t fg = focus ? XUI_C_BG : dim ? XUI_C_FAINT : XUI_C_TEXT;
    if (focus) xui_round_rect(c, x - 3, y - 3, w + 6, CF_ROW_H + 6, 10, XUI_C_TEXT, 255);
    else xui_round_rect(c, x, y, w, CF_ROW_H, 8, XUI_C_BUTTON, dim ? 110 : 170);
    int base = y + CF_ROW_H / 2 + 8;
    int right = x + w - 22;
    int lx = x + 22;
    if ((r->flags & XUI_CR_WARN) && !focus) xui_circle(c, x + 8, y + 10, 5, XUI_C_FLARE, 255);
    if (r->flags & XUI_CR_CENTER) {
        xui_text_center(c, XUI_F_ROW, x, w, base, fg, r->label, 0);
        return;
    }
    if (r->flags & XUI_CR_RADIO) {
        if (r->flags & XUI_CR_ON) {
            xui_circle(c, x + 30, y + CF_ROW_H / 2, 11, XUI_C_ACCENT, 255);
            xui_circle(c, x + 30, y + CF_ROW_H / 2, 4, XUI_C_TEXT, 255);
        } else {
            xui_round_frame(c, x + 21, y + CF_ROW_H / 2 - 9, 18, 18, 9, 2, focus ? XUI_C_BG : XUI_C_FAINT);
        }
        lx = x + 56;
    }
    if (r->flags & XUI_CR_ARROW) { cf_chevron(c, right - 8, y + CF_ROW_H / 2, fg); right -= 26; }
    if ((r->flags & XUI_CR_CHOICES) && r->nchoices > 0) {
        int pw = 92, gap = 10, px0 = right - r->nchoices * pw - (r->nchoices - 1) * gap;
        for (int i = 0; i < r->nchoices; i++) {
            int bx = px0 + i * (pw + gap), by = y + CF_ROW_H / 2 - 17;
            if (i == r->choice) {
                xui_round_rect(c, bx, by, pw, 34, 17, XUI_C_ACCENT, 255);
                xui_text_center(c, XUI_F_BODY, bx, pw, by + 23, XUI_C_TEXT, r->choices[i], 0);
            } else {
                xui_round_rect(c, bx, by, pw, 34, 17, focus ? XUI_C_DIM : XUI_C_BG, focus ? 90 : 150);
                xui_text_center(c, XUI_F_BODY, bx, pw, by + 23, focus ? XUI_C_BG : XUI_C_DIM, r->choices[i], 0);
            }
        }
        right = px0 - 16;
    }
    if (r->tag && r->tag[0]) {
        int tw = xui_text_width(XUI_F_LABEL, r->tag, 1);
        xui_text(c, XUI_F_LABEL, right - tw - 8, base - 2, focus ? XUI_C_BG : XUI_C_HAZE, r->tag, 1, 0);
        right -= tw + 28;
    }
    if (r->value && r->value[0]) {
        int vf = (r->flags & XUI_CR_CHOICES) ? XUI_F_LABEL : XUI_F_BODY;
        int vw = xui_text_width(vf, r->value, 0);
        int maxw = right - lx - 160;
        if (maxw > 420) maxw = 420;
        if (vw > maxw) vw = maxw;
        uint32_t vc = focus ? XUI_C_BG : (r->flags & XUI_CR_WARN) ? XUI_C_FLARE : dim ? XUI_C_FAINT : XUI_C_DIM;
        xui_text(c, vf, right - vw, base - 1, vc, r->value, 0, maxw);
        right -= vw + 20;
    }
    int lx2 = xui_text(c, XUI_F_ROW, lx, base, fg, r->label, 0, right - lx);
    if (r->detail && r->detail[0] && lx2 + 12 < right)
        xui_text(c, XUI_F_BODY, lx2 + 12, base - 1, focus ? XUI_C_BG : dim ? XUI_C_FAINT : XUI_C_DIM, r->detail, 0,
                 right - lx2 - 12);
}

static void cf_slider(xui_canvas_t *c, int y, const char *lab, int pct, int focus, uint32_t col) {
    int x = CF_RX + CF_RIN - 12 + 400, w = CF_RW - 2 * CF_RIN + 24 - 400;
    uint32_t fg = focus ? XUI_C_BG : XUI_C_TEXT;
    if (focus) xui_round_rect(c, x - 3, y - 3, w + 6, 82 + 6, 10, XUI_C_TEXT, 255);
    else xui_round_rect(c, x, y, w, 82, 8, XUI_C_BUTTON, 170);
    xui_text(c, XUI_F_ROW, x + 20, y + 32, fg, lab, 0, w - 120);
    char v[16];
    snprintf(v, sizeof(v), "%d%%", pct);
    int vw = xui_text_width(XUI_F_ROW, v, 0);
    xui_text(c, XUI_F_ROW, x + w - 20 - vw, y + 32, fg, v, 0, 0);
    int tx = x + 20, tww = w - 40, ty = y + 56;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    xui_round_rect(c, tx, ty, tww, 6, 3, focus ? XUI_C_DIM : XUI_C_PH, 255);
    xui_round_rect(c, tx, ty, tww * pct / 100, 6, 3, col, 255);
    xui_circle(c, tx + tww * pct / 100, ty + 3, 9, focus ? XUI_C_BG : XUI_C_TEXT, 255);
    if (focus) {   /* setinhas: o D-pad ajusta */
        for (int i = 0; i < 5; i++) {
            xui_fill(c, tx - 14 + i, ty + 3 - i, 1, 2 * i + 1, XUI_C_BG);
            xui_fill(c, tx + tww + 14 - i, ty + 3 - i, 1, 2 * i + 1, XUI_C_BG);
        }
    }
}

/* posição do analógico -> ponto no círculo (preso um pouco além da borda) */
static void cf_cal_pt(int x, int y, int *ox, int *oy) {
    float fx = x / 32767.0f, fy = y / 32767.0f;
    float m = sqrtf(fx * fx + fy * fy), lim = 1.04f;
    if (m > lim) { fx *= lim / m; fy *= lim / m; }
    *ox = CF_CAL_CX + (int)(fx * CF_CAL_R);
    *oy = CF_CAL_CY - (int)(fy * CF_CAL_R);
}

static void cf_calib(xui_canvas_t *c, const xui_calib_t *k) {
    int cx = CF_CAL_CX, cy = CF_CAL_CY, R = CF_CAL_R;
    xui_circle(c, cx, cy, R + 2, XUI_C_BG, 200);
    xui_round_frame(c, cx - R, cy - R, 2 * R, 2 * R, R, 2, XUI_C_FAINT);        /* curso todo */
    int rm = R * k->out_pct / 100;
    xui_circle(c, cx, cy, rm, XUI_C_ACCENT, 40);                                 /* vai pro jogo */
    xui_round_frame(c, cx - rm, cy - rm, 2 * rm, 2 * rm, rm, 2, XUI_C_ACCENT);   /* limite máximo */
    int rd = R * k->dz_pct / 100;
    if (rd > 0) {
        xui_circle(c, cx, cy, rd, XUI_C_FLARE, 70);                              /* zona morta */
        xui_round_frame(c, cx - rd, cy - rd, 2 * rd, 2 * rd, rd, 2, XUI_C_FLARE);
    }
    xui_fill_a(c, cx - R, cy, 2 * R, 1, XUI_C_TEXT, 50);
    xui_fill_a(c, cx, cy - R, 1, 2 * R, XUI_C_TEXT, 50);
    for (int i = k->ntrail - 1; i >= 0; i--) {   /* rastro curto, o mais velho mais apagado */
        int px, py;
        cf_cal_pt(k->tx[i], k->ty[i], &px, &py);
        xui_circle(c, px, py, 4, XUI_C_TEXT, (unsigned)(110 - i * 12));
    }
    int dx, dy;
    cf_cal_pt(k->x, k->y, &dx, &dy);
    xui_circle(c, dx, dy, 11, XUI_C_BG, 255);
    xui_circle(c, dx, dy, 9, XUI_C_TEXT, 255);
    char l1[48], l2[48];
    snprintf(l1, sizeof(l1), "Posição: %d%%", k->pos_pct);
    snprintf(l2, sizeof(l2), "Jogo recebe: %d%%", k->game_pct);
    xui_text(c, XUI_F_BODY, cx - R, cy + R + 36, XUI_C_DIM, l1, 0, 160);
    xui_text(c, XUI_F_BODY, cx + 10, cy + R + 36, k->game_pct == 0 ? XUI_C_FLARE : XUI_C_ACCENT, l2, 0, 190);
}

static void cf_calib_page(xui_canvas_t *c, const xui_calib_t *k) {
    int cx = CF_CAL_CX, cy = CF_CAL_CY, R = CF_CAL_R;
    const char *stp = k->step ? k->step : "";
    if (stp[0]) {
        int sw = xui_text_width(XUI_F_LABEL, stp, 0) + 20;
        xui_round_rect(c, CF_RX + CF_RW - CF_RIN - sw, CF_RY + 44, sw, 26, 13, XUI_C_ACCENT, 255);
        xui_text_center(c, XUI_F_LABEL, CF_RX + CF_RW - CF_RIN - sw, sw, CF_RY + 62, XUI_C_BG, stp, 0);
    }
    cf_calib(c, k);
    /* legenda (+ qual controle, com mais de um) */
    xui_circle(c, cx - R + 6, cy + R + 60, 5, XUI_C_FLARE, 255);
    xui_text(c, XUI_F_LABEL, cx - R + 18, cy + R + 65, XUI_C_FAINT, L("ignorado"), 0, 100);
    xui_circle(c, cx - R + 100, cy + R + 60, 5, XUI_C_ACCENT, 255);
    xui_text(c, XUI_F_LABEL, cx - R + 112, cy + R + 65, XUI_C_FAINT, L("vai pro jogo"), 0, 120);
    if (k->pad) xui_text(c, XUI_F_LABEL, cx - R + 230, cy + R + 65, XUI_C_HAZE, k->pad, 0, 120);
    int y = CF_RY + 104;
    cf_slider(c, y, L("Zona morta mínima"), k->dz_pct, k->focus == 0, XUI_C_FLARE);
    y += 98;
    cf_slider(c, y, L("Limite máximo"), k->out_pct, k->focus == 1, XUI_C_ACCENT);
    y += 98;
    int x = CF_RX + CF_RIN - 12 + 400, w = CF_RW - 2 * CF_RIN + 24 - 400;
    for (int i = 0; i < 2; i++) {
        int f = k->focus == 2 + i, by = y + i * 62;
        const char *b = i ? (k->next ? k->next : L("Concluir")) : (k->measuring ? L("Medindo…") : L("Medir sozinho"));
        if (f) xui_round_rect(c, x - 3, by - 3, w + 6, 52, 10, XUI_C_TEXT, 255);
        else xui_round_rect(c, x, by, w, 46, 8, XUI_C_BUTTON, 255);
        xui_text_center(c, XUI_F_ROW, x, w, by + 30, f ? XUI_C_BG : XUI_C_TEXT, b, 0);
    }
    xui_text_wrap(c, XUI_F_LABEL, x, y + 146, w, 0, 3, 0, k->status ? XUI_C_ACCENT : XUI_C_FAINT,
                  k->status ? k->status : L("Medir sozinho: solte o analógico por 2 s, depois gire-o encostando na borda."));
}

/* Novidades: parágrafos (\n) com bolinha; "- " / "* " no começo some */
static void cf_notes_page(xui_canvas_t *c, const xui_cfg_t *s) {
    int x = CF_RX + CF_RIN, w = CF_RW - 2 * CF_RIN;
    int y = CF_RY + 150, ymax = CF_RY + CF_RH - 56;
    const char *p = s->notes;
    if (!p || !p[0]) {
        xui_text_wrap(c, XUI_F_BODY, x, y, w, 0, 3, 0, XUI_C_DIM, s->notes_empty ? s->notes_empty : "");
        return;
    }
    int paras = 0;
    for (const char *q = p; *q; q++) if (*q == '\n') paras++;
    paras++;
    int skip = s->scroll, multi = paras > 1;
    char para[XUI_MAX_W / 2];
    while (*p && y < ymax) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof(para)) n = sizeof(para) - 1;
        memcpy(para, p, n);
        para[n] = 0;
        p = e ? e + 1 : p + strlen(p);
        char *t = para;
        while (*t == ' ' || *t == '\t' || *t == '\r') t++;
        if ((t[0] == '-' || t[0] == '*') && t[1] == ' ') t += 2;
        size_t tl = strlen(t);
        while (tl && (t[tl - 1] == '\r' || t[tl - 1] == ' ')) t[--tl] = 0;
        if (!t[0]) continue;
        if (skip > 0) { skip--; continue; }
        int lines = (ymax - y) / xui_line_h(XUI_F_BODY);
        if (lines < 1) break;
        if (lines > 6) lines = 6;
        int tx = multi ? x + 24 : x;
        if (multi) xui_circle(c, x + 6, y - 6, 4, XUI_C_ACCENT, 255);
        int k = xui_text_wrap(c, XUI_F_BODY, tx, y, w - (tx - x), 0, lines, 0, XUI_C_TEXT, t);
        y += k * xui_line_h(XUI_F_BODY) + 18;
    }
}

static void render_cfg(xui_view_t *v, xui_canvas_t *c, const xui_cfg_t *s, const char *version) {
    xui_copy_from(c, v->bg, v->w, c->clip);
    xui_fill_a(c, 0, 0, v->w, v->h, XUI_C_BG, 200);
    draw_logo(c, version);
    cf_column(c, s);
    xui_round_rect(c, CF_RX, CF_RY, CF_RW, CF_RH, 14, XUI_C_PANEL, s->right ? 245 : 225);
    int x = CF_RX + CF_RIN, w = CF_RW - 2 * CF_RIN;
    if (s->page != XUI_CFG_ROWS) {
        if (s->crumb) xui_text(c, XUI_F_LABEL, x, CF_RY + 30, XUI_C_FAINT, s->crumb, 1, 500);
        xui_text(c, XUI_F_H2, x, CF_RY + 66, XUI_C_TEXT, s->title ? s->title : "", 0, w - 120);
        if (s->page == XUI_CFG_NOTES) {
            if (s->subtitle) xui_text(c, XUI_F_BODY, x, CF_RY + 96, XUI_C_DIM, s->subtitle, 0, w);
            cf_notes_page(c, s);
        } else if (s->calib) {
            cf_calib_page(c, s->calib);
        }
    } else {
        xui_text(c, XUI_F_H2, x, CF_RY + 52, XUI_C_TEXT, s->title ? s->title : "", 0, w);
        if (s->subtitle) xui_text(c, XUI_F_BODY, x, CF_RY + 82, XUI_C_DIM, s->subtitle, 0, w);
        int y = s->ninfo ? CF_RY + 92 : s->rows && s->nrows && s->rows[0].head && !s->card ? CF_RY + 112 : CF_RY + 116;
        for (int i = 0; i < s->ninfo && i < 2; i++) {
            int ix = x + i * 430, big = s->info[i].big_title;
            xui_text(c, XUI_F_LABEL, ix, y, XUI_C_FAINT, s->info[i].label, 1, 400);
            xui_text(c, big ? XUI_F_TITLE : XUI_F_ROW, ix, y + (big ? 40 : 30), XUI_C_TEXT, s->info[i].big, big ? 2 : 0,
                     i ? w - 430 : 420);
            if (s->info[i].small)
                xui_text(c, XUI_F_LABEL, ix, y + (big ? 64 : 52), XUI_C_HAZE, s->info[i].small, 0, i ? w - 430 : 420);
        }
        if (s->ninfo) y += 96;
        int yend = y;   /* fim do último bloco desenhado */
        if (s->card) {
            int cx = x - 12, cw = w + 24, h = s->card_text2 ? 104 : 80;
            xui_round_rect(c, cx, y, cw, h, 10, XUI_C_BG, 150);
            if (s->card == 2) cf_bars(c, cx + 20, y + 42, s->card_bars, s->card_ok ? XUI_C_ACCENT : XUI_C_FAINT, XUI_C_PH);
            else xui_circle(c, cx + 28, y + 30, 7, s->card_ok ? XUI_C_ACCENT : XUI_C_FAINT, 255);
            xui_text(c, XUI_F_ROW, cx + 56, y + 38, XUI_C_TEXT, s->card_title ? s->card_title : "", 0, cw - 80);
            if (s->card_text) xui_text(c, XUI_F_BODY, cx + 56, y + 66, XUI_C_DIM, s->card_text, 0, cw - 80);
            if (s->card_text2) xui_text(c, XUI_F_BODY, cx + 56, y + 90, XUI_C_FAINT, s->card_text2, 0, cw - 80);
            yend = y + h;
            y = yend + 24;
        }
        for (int i = 0; i < s->nrows; i++) {
            const xui_cfg_row_t *r = &s->rows[i];
            if (r->head) {
                if (i > 0) {
                    int dy = yend + 22;
                    xui_fill(c, x, dy, w, 1, XUI_C_BUTTON);
                    y = dy + 30;
                }
                xui_text(c, XUI_F_LABEL, x, y, XUI_C_FAINT, r->head, 1, 300);
                y += 12;
            } else if (i > 0) {
                y = yend + (CF_ROW_P - CF_ROW_H);
            }
            cf_row(c, y, r, i == s->focus);
            yend = y + CF_ROW_H;
            if (r->after && r->after[0]) {   /* até 2 linhas, separadas por \n */
                const char *p = r->after;
                int ly = yend + 26, k = 0;
                while (*p && k < 2) {
                    const char *e = strchr(p, '\n');
                    size_t n = e ? (size_t)(e - p) : strlen(p);
                    char ln[200];
                    if (n >= sizeof(ln)) n = sizeof(ln) - 1;
                    memcpy(ln, p, n);
                    ln[n] = 0;
                    p = e ? e + 1 : p + strlen(p);
                    if (!ln[0]) continue;
                    xui_text(c, XUI_F_LABEL, x, ly, XUI_C_HAZE, ln, 0, w);
                    ly += 20;
                    k++;
                }
                if (k) yend = ly - 20;
            }
        }
        if (s->text && s->text[0]) {
            int ty = yend + 30;   /* v0.21.43: era +4 sem linhas (o texto subia por cima do cartão) */
            int n = xui_text_wrap(c, XUI_F_BODY, x, ty, w, 0, 4, 0, XUI_C_DIM, s->text);
            yend = ty + (n - 1) * xui_line_h(XUI_F_BODY) + 8;
        }
        if (s->note && s->note[0]) {
            int ny = yend + 22, nx = x - 12, nw = w + 24;
            xui_round_rect(c, nx, ny, nw, 44, 8, XUI_C_BG, 150);
            xui_text(c, XUI_F_BODY, nx + 18, ny + 28, XUI_C_DIM, s->note, 0, nw - 36);
        }
    }
    if (s->footer && s->footer[0])
        xui_text(c, XUI_F_LABEL, x, CF_RY + CF_RH - 28, XUI_C_FAINT, s->footer, 0, w);
    int hb = CF_LY + CF_LH + 36;
    int hw = (s->hint && s->hint[0]) ? draw_hints(c, 0, 0, s->hint, 1) : 0;
    if (hw) draw_hints(c, v->w - 64 - hw, hb, s->hint, 0);
    if (s->notice && s->notice[0]) xui_text(c, XUI_F_BODY, CF_LX, hb, XUI_C_ACCENT, s->notice, 0, v->w - 64 - hw - 40 - CF_LX);
    if (s->confirm) draw_confirm(v, c, s->confirm);
}

/* FNV-1a 64 do conteúdo (as notas passam dos 600 B do st_sig) */
static void cf_hs(uint64_t *h, const char *p) {
    if (!p) p = "\x01";
    for (; *p; p++) { *h ^= (unsigned char)*p; *h *= 1099511628211ULL; }
    *h ^= 0x1f; *h *= 1099511628211ULL;
}
static void cf_hi(uint64_t *h, int x) {
    for (int i = 0; i < 4; i++) { *h ^= (unsigned)(x >> (8 * i)) & 0xff; *h *= 1099511628211ULL; }
}

static uint64_t cf_hash(const xui_cfg_t *s, int dynamic) {
    uint64_t h = 1469598103934665603ULL;
    if (dynamic) {   /* só o que anima na calibração */
        const xui_calib_t *k = s->calib;
        if (!k) return h;
        cf_hi(&h, k->x); cf_hi(&h, k->y); cf_hi(&h, k->pos_pct); cf_hi(&h, k->game_pct); cf_hi(&h, k->ntrail);
        for (int i = 0; i < k->ntrail && i < XUI_CAL_TRAIL; i++) { cf_hi(&h, k->tx[i]); cf_hi(&h, k->ty[i]); }
        return h;
    }
    for (int i = 0; i < XUI_CFG_NSEC; i++) cf_hs(&h, s->sections[i]);
    cf_hi(&h, s->item); cf_hi(&h, s->right); cf_hi(&h, s->page); cf_hi(&h, s->ninfo); cf_hi(&h, s->card);
    cf_hi(&h, s->card_ok); cf_hi(&h, s->card_bars); cf_hi(&h, s->nrows); cf_hi(&h, s->focus); cf_hi(&h, s->scroll);
    cf_hs(&h, s->crumb); cf_hs(&h, s->title); cf_hs(&h, s->subtitle);
    for (int i = 0; i < s->ninfo && i < 2; i++) {
        cf_hs(&h, s->info[i].label); cf_hs(&h, s->info[i].big); cf_hs(&h, s->info[i].small); cf_hi(&h, s->info[i].big_title);
    }
    cf_hs(&h, s->card_title); cf_hs(&h, s->card_text); cf_hs(&h, s->card_text2);
    for (int i = 0; i < s->nrows; i++) {
        const xui_cfg_row_t *r = &s->rows[i];
        cf_hs(&h, r->head); cf_hs(&h, r->label); cf_hs(&h, r->detail); cf_hs(&h, r->value); cf_hs(&h, r->tag);
        cf_hs(&h, r->after); cf_hi(&h, (int)r->flags); cf_hi(&h, r->nchoices); cf_hi(&h, r->choice);
        for (int j = 0; j < r->nchoices; j++) cf_hs(&h, r->choices[j]);
    }
    cf_hs(&h, s->text); cf_hs(&h, s->note); cf_hs(&h, s->notes); cf_hs(&h, s->notes_empty); cf_hs(&h, s->footer);
    cf_hs(&h, s->notice); cf_hs(&h, s->hint);
    if (s->calib) {
        const xui_calib_t *k = s->calib;
        cf_hi(&h, k->stick); cf_hi(&h, k->dz_pct); cf_hi(&h, k->out_pct); cf_hi(&h, k->focus); cf_hi(&h, k->measuring);
        cf_hs(&h, k->step); cf_hs(&h, k->next); cf_hs(&h, k->status); cf_hs(&h, k->pad);
    } else {
        cf_hi(&h, -1);
    }
    if (s->confirm) {
        cf_hs(&h, s->confirm->title); cf_hs(&h, s->confirm->text); cf_hs(&h, s->confirm->yes); cf_hs(&h, s->confirm->no);
        cf_hi(&h, s->confirm->focus);
    } else {
        cf_hi(&h, -2);
    }
    return h;
}

xui_rect_t xui_cfg_frame(xui_view_t *v, xui_canvas_t *c, const xui_cfg_t *s, const xui_home_data_t *d,
                         unsigned now_ms, int *animating) {
    xui_rect_t none = { 0, 0, 0, 0 }, full = { 0, 0, v->w, v->h };
    xui_rect_t dirty = none;
    v->valid = 0;   /* voltar pra home redesenha tudo */
    v->last_ms = now_ms;
    v->have_time = 1;
    char sig[64];
    unsigned long long hs = cf_hash(s, 0), hd = cf_hash(s, 1);
    snprintf(sig, sizeof(sig), "cfg:%016llx:%016llx", hs, hd);
    if (!v->hero_valid) { build_hero(v, d, NULL); v->st_valid = 0; }
    int calib_live = s->page == XUI_CFG_CALIB && s->calib && !s->confirm;
    if (!v->st_valid || v->st_kind != ST_KIND_CFG || strncmp(sig, v->st_sig, 21) != 0) {
        dirty = full;
    } else if (strcmp(sig, v->st_sig) != 0) {
        dirty = calib_live ? xui_cfg_calib_rect() : full;   /* só a posição mexeu */
    }
    if (!xui_rect_empty(dirty)) {
        xui_set_clip(c, dirty);
        render_cfg(v, c, s, d ? d->version : NULL);
        xui_clip_all(c);
        v->stats.frames++;
        if (dirty.w == full.w && dirty.h == full.h) v->stats.full++; else v->stats.partial++;
    }
    v->st_valid = 1;
    v->st_kind = ST_KIND_CFG;
    v->st_prog = -1;
    v->st_phase = 0;
    scopy(v->st_sig, sizeof(v->st_sig), sig);
    if (animating) *animating = calib_live;
    return dirty;
}

/* ================================================================
 * v0.21.0: Wi-Fi (lista de redes) e teclado de texto (senha)
 * ================================================================ */
#define ST_KIND_WIFI     102
#define ST_KIND_TEXTKEY  103

#define WP_PX  64
#define WP_PY  130
#define WP_PW  1152
#define WP_PH  470
#define WL_X   (WP_PX + 44)
#define WL_W   380
#define WR_X   (WL_X + WL_W + 40)
#define WR_W   (WP_PX + WP_PW - 44 - WR_X)
#define WR_Y0  (WP_PY + 84)
#define WROW_H 52
#define WROW_P 58
#define WR_VIS 6
#define WBTN_Y (WP_PY + 330)

static xui_rect_t wifi_status_dots(void) { xui_rect_t r = { WL_X, WP_PY + 86, WL_W, 30 }; return r; }
static xui_rect_t wifi_list_dots(void) { xui_rect_t r = { WR_X + 220, WP_PY + 34, 220, 30 }; return r; }

static void draw_dots(xui_canvas_t *c, int x, int cy, int phase) {
    for (int i = 0; i < 3; i++) {
        int on = (phase % 3) == i;
        xui_circle(c, x + 7 + i * 22, cy, on ? 7 : 5, on ? XUI_C_ACCENT : XUI_C_FAINT, 255);
    }
}

/* 4 barrinhas de sinal com a base em y */
static void draw_bars(xui_canvas_t *c, int x, int y, int bars, uint32_t on, uint32_t off) {
    for (int i = 0; i < 4; i++) {
        int h = 5 + i * 4;
        xui_round_rect(c, x + i * 7, y - h, 5, h, 1, i < bars ? on : off, 255);
    }
}

/* cadeado (base em y) */
static void draw_lock(xui_canvas_t *c, int x, int y, uint32_t col) {
    xui_round_frame(c, x + 4, y - 15, 6, 8, 4, 2, col);
    xui_round_rect(c, x, y - 10, 14, 10, 2, col, 255);
}

static void render_wifi(xui_view_t *v, xui_canvas_t *c, const xui_wifi_t *w, const char *version, int phase) {
    xui_copy_from(c, v->bg, v->w, c->clip);
    xui_fill_a(c, 0, 0, v->w, v->h, XUI_C_BG, 170);
    draw_logo(c, version);
    xui_round_rect(c, WP_PX, WP_PY, WP_PW, WP_PH, 14, XUI_C_PANEL, 240);
    /* ---- esquerda: título, estado, rede salva, botões ---- */
    int x = WL_X, y = WP_PY + 64;
    xui_text(c, XUI_F_H2, x, y, XUI_C_TEXT, "Wi-Fi", 0, WL_W);
    y += 44;
    int tx = x;
    if (w->status_kind == XUI_WS_OK) {
        xui_circle(c, x + 7, y - 7, 7, XUI_C_ACCENT, 255);
        tx += 24;
    } else if (w->status_kind == XUI_WS_ERROR) {
        xui_circle(c, x + 11, y - 7, 11, XUI_C_RED, 255);
        xui_text_center(c, XUI_F_LABEL, x, 22, y - 2, XUI_C_TEXT, "!", 0);
        tx += 30;
    } else if (w->status_kind == XUI_WS_NEUTRAL) {
        xui_circle(c, x + 7, y - 7, 6, XUI_C_FAINT, 255);
        tx += 24;
    }
    int tw = xui_text(c, XUI_F_ROW, tx, y, XUI_C_TEXT, w->status_title ? w->status_title : "", 0, x + WL_W - tx -
                      (w->status_kind == XUI_WS_BUSY ? 80 : 0));
    if (w->status_kind == XUI_WS_BUSY) draw_dots(c, tw + 12, y - 7, phase);
    y += 30;
    if (w->status_text && w->status_text[0])
        xui_text_wrap(c, XUI_F_BODY, x, y, WL_W, 0, 3, 0, w->status_kind == XUI_WS_ERROR ? XUI_C_TEXT : XUI_C_DIM,
                      w->status_text);
    if (w->saved && w->saved[0]) {
        xui_text(c, XUI_F_LABEL, x, WP_PY + 262, XUI_C_FAINT, L("REDE SALVA"), 1, WL_W);
        xui_text(c, XUI_F_ROW, x, WP_PY + 292, XUI_C_TEXT, w->saved, 0, WL_W);
    }
    const char *blab[2] = { L("Procurar redes"), L("Esquecer rede") };
    for (int i = 0; i < 2; i++) {
        int by = WBTN_Y + i * 62, f = w->focus == i - 2, dim = i == 1 && !w->can_forget;
        if (f) {
            xui_round_rect(c, x - 3, by - 3, WL_W + 6, 52, 10, XUI_C_TEXT, 255);
            xui_text_center(c, XUI_F_ROW, x, WL_W, by + 30, XUI_C_BG, blab[i], 0);
        } else {
            xui_round_rect(c, x, by, WL_W, 46, 8, XUI_C_BUTTON, dim ? 110 : 255);
            xui_text_center(c, XUI_F_ROW, x, WL_W, by + 30, dim ? XUI_C_FAINT : XUI_C_TEXT, blab[i], 0);
        }
    }
    /* ---- direita: redes ---- */
    xui_text(c, XUI_F_LABEL, WR_X, WP_PY + 56, XUI_C_FAINT, L("REDES DISPONÍVEIS"), 1, 200);
    if (w->scanning) {
        xui_rect_t d = wifi_list_dots();
        int lx = xui_text(c, XUI_F_LABEL, d.x, WP_PY + 56, XUI_C_ACCENT, L("Procurando"), 0, 120);
        draw_dots(c, lx + 8, WP_PY + 50, phase);
    }
    if (w->nnets <= 0) {
        xui_text_wrap(c, XUI_F_BODY, WR_X, WR_Y0 + 40, WR_W, 0, 4, 0, XUI_C_DIM,
                      w->empty ? w->empty : (w->scanning ? L("Procurando redes…") : L("Nenhuma rede encontrada.")));
    } else {
        int first = 0;
        if (w->focus >= WR_VIS) first = w->focus - WR_VIS + 1;
        for (int i = 0; i < WR_VIS && first + i < w->nnets; i++) {
            const xui_wifi_net_t *n = &w->nets[first + i];
            int ry = WR_Y0 + i * WROW_P, f = w->focus == first + i;
            uint32_t fg = f ? XUI_C_BG : (n->dim ? XUI_C_FAINT : XUI_C_TEXT);
            if (f) xui_round_rect(c, WR_X - 3, ry - 3, WR_W + 6, WROW_H + 6, 10, XUI_C_TEXT, 255);
            else xui_round_rect(c, WR_X, ry, WR_W, WROW_H, 8, XUI_C_BUTTON, 150);
            int base = ry + WROW_H / 2 + 8;
            draw_bars(c, WR_X + 16, ry + WROW_H / 2 + 9, n->bars, fg, f ? XUI_C_DIM : XUI_C_PH);
            int right = WR_X + WR_W - 18;
            if (n->tag && n->tag[0]) {
                int gw = xui_text_width(XUI_F_LABEL, n->tag, 0);
                right -= gw;
                xui_text(c, XUI_F_LABEL, right, base - 1,
                         f ? XUI_C_BG : (n->dim ? XUI_C_FAINT : XUI_C_ACCENT), n->tag, 0, 0);
                right -= 16;
            }
            if (n->lock) {
                right -= 14;
                draw_lock(c, right, base, fg);
                right -= 14;
            }
            xui_text(c, XUI_F_ROW, WR_X + 60, base, fg, n->ssid ? n->ssid : "", 0, right - (WR_X + 60));
        }
        int hidden = w->nnets - first - WR_VIS;
        if (hidden > 0) {
            char m[32];
            snprintf(m, sizeof(m), "+%d %s abaixo", hidden, hidden == 1 ? "rede" : "redes");
            xui_text(c, XUI_F_LABEL, WR_X, WR_Y0 + WR_VIS * WROW_P + 14, XUI_C_FAINT, m, 0, WR_W);
        }
    }
    if (w->hint && w->hint[0]) {
        int hw = draw_hints(c, 0, 0, w->hint, 1);
        draw_hints(c, WP_PX + WP_PW - 36 - hw, WP_PY + WP_PH + 40, w->hint, 0);
    }
}

xui_rect_t xui_wifi_frame(xui_view_t *v, xui_canvas_t *c, const xui_wifi_t *w, const xui_home_data_t *d,
                          unsigned now_ms, int *animating) {
    xui_rect_t none = { 0, 0, 0, 0 }, full = { 0, 0, v->w, v->h };
    xui_rect_t dirty = none;
    v->valid = 0;
    v->last_ms = now_ms;
    v->have_time = 1;
    char sig[600], num[64];
    int n = 0;
    snprintf(num, sizeof(num), "%d/%d/%d/%d/%d", w->focus, w->nnets, w->status_kind, w->scanning, w->can_forget);
    sig_cat(sig, &n, (int)sizeof(sig), num);
    sig_cat(sig, &n, (int)sizeof(sig), w->status_title);
    sig_cat(sig, &n, (int)sizeof(sig), w->status_text);
    sig_cat(sig, &n, (int)sizeof(sig), w->saved);
    sig_cat(sig, &n, (int)sizeof(sig), w->empty);
    sig_cat(sig, &n, (int)sizeof(sig), w->hint);
    for (int i = 0; i < w->nnets && n < (int)sizeof(sig) - 40; i++) {
        snprintf(num, sizeof(num), "%d%d%d", w->nets[i].bars, w->nets[i].lock, w->nets[i].dim);
        sig_cat(sig, &n, (int)sizeof(sig), num);
        sig_cat(sig, &n, (int)sizeof(sig), w->nets[i].ssid);
        sig_cat(sig, &n, (int)sizeof(sig), w->nets[i].tag);
    }
    if (!v->hero_valid) { build_hero(v, d, NULL); v->st_valid = 0; }
    int phase = (int)(now_ms / 300);
    int busy = w->status_kind == XUI_WS_BUSY;
    if (!v->st_valid || v->st_kind != ST_KIND_WIFI || strcmp(sig, v->st_sig) != 0) {
        dirty = full;
    } else if ((busy || w->scanning) && phase != v->st_phase) {
        if (busy) dirty = wifi_status_dots();
        if (w->scanning) dirty = xui_rect_union(dirty, wifi_list_dots());
    }
    if (!xui_rect_empty(dirty)) {
        xui_set_clip(c, dirty);
        render_wifi(v, c, w, d ? d->version : NULL, phase);
        xui_clip_all(c);
        v->stats.frames++;
        if (dirty.w == full.w && dirty.h == full.h) v->stats.full++; else v->stats.partial++;
    }
    v->st_valid = 1;
    v->st_kind = ST_KIND_WIFI;
    v->st_prog = -1;
    v->st_phase = phase;
    scopy(v->st_sig, sizeof(v->st_sig), sig);
    if (animating) *animating = busy || w->scanning;
    return dirty;
}

/* ---- teclado de texto (senha) ---- */
#define TK_KW   50
#define TK_KH   50
#define TK_GAP  6
#define TK_W    (XUI_TK_COLS * TK_KW + (XUI_TK_COLS - 1) * TK_GAP)
#define TK_X    (KP_PX + KP_PW - 44 - TK_W)
#define TK_Y    (KP_PY + 40)
#define TKL_X   (KP_PX + 44)
#define TKL_W   (TK_X - 48 - TKL_X)

static xui_rect_t tk_dots_rect(void) { xui_rect_t r = { TKL_X, KIN_Y + KIN_H + 40, TKL_W, 40 }; return r; }

/* spans das ações (iguais a xtk_act_span): A1 = 2,2,3,3; A2 = 5,5 */
static const int tk_a1[4][2] = { { 0, 2 }, { 2, 2 }, { 4, 3 }, { 7, 3 } };
static const int tk_a2[2][2] = { { 0, 5 }, { 5, 5 } };

static void render_textkey(xui_view_t *v, xui_canvas_t *c, const xui_textkey_t *k, const char *version, int phase) {
    xui_copy_from(c, v->bg, v->w, c->clip);
    xui_fill_a(c, 0, 0, v->w, v->h, XUI_C_BG, 170);
    draw_logo(c, version);
    xui_round_rect(c, KP_PX, KP_PY, KP_PW, KP_PH, 14, XUI_C_PANEL, 240);
    int x = TKL_X, y = KP_PY + 64, w = TKL_W;
    xui_text(c, XUI_F_H2, x, y, XUI_C_TEXT, k->title ? k->title : L("Senha"), 0, w);
    y += 38;
    xui_text_wrap(c, XUI_F_BODY, x, y, w, 0, 3, 0, XUI_C_DIM, k->text);
    /* caixa da senha: bolinhas ou o texto; mostra o fim se não couber */
    xui_round_rect(c, x, KIN_Y, w, KIN_H, 10, XUI_C_BUTTON, 255);
    int base = KIN_Y + KIN_H / 2 + 10, ix = x + 20, iw = w - 40;
    const char *t = k->typed ? k->typed : "";
    int n = (int)strlen(t);
    int endx = ix;
    if (!k->show) {
        int cap = iw / 16;
        int s0 = n > cap ? n - cap : 0;
        for (int i = s0; i < n; i++) xui_circle(c, ix + 6 + (i - s0) * 16, base - 8, 5, XUI_C_TEXT, 255);
        endx = ix + (n - s0) * 16;
    } else {
        int s0 = 0;
        while (s0 < n && xui_text_width(XUI_F_ROW, t + s0, 0) > iw - 24) s0++;
        int px0 = ix;
        if (s0 > 0) px0 = xui_text(c, XUI_F_ROW, ix, base, XUI_C_FAINT, "…", 0, 0);
        endx = xui_text(c, XUI_F_ROW, px0, base, XUI_C_TEXT, t + s0, 0, 0);
    }
    if (!k->busy) xui_fill(c, endx + 2, base - 22, 2, 28, XUI_C_ACCENT);
    char cnt[64];
    snprintf(cnt, sizeof(cnt), "%d/%d caracteres%s", k->len, k->maxlen,
             k->len < k->minlen ? (k->minlen == 8 ? L(" (mínimo 8)") : "") : "");
    xui_text(c, XUI_F_LABEL, x, KIN_Y + KIN_H + 24, k->len < k->minlen ? XUI_C_FAINT : XUI_C_DIM, cnt, 0, w);
    xui_rect_t dr = tk_dots_rect();
    if (k->busy) {
        const char *st = k->status ? k->status : L("Conectando…");
        int sx = xui_text(c, XUI_F_ROW, x, dr.y + 26, XUI_C_TEXT, st, 0, w - 90);
        draw_dots(c, sx + 12, dr.y + 19, phase);
    } else if (k->error && k->error[0]) {
        xui_circle(c, x + 11, dr.y + 19, 11, XUI_C_RED, 255);
        xui_text_center(c, XUI_F_LABEL, x, 22, dr.y + 24, XUI_C_TEXT, "!", 0);
        xui_text_wrap(c, XUI_F_BODY, x + 32, dr.y + 25, w - 32, 0, 2, 0, XUI_C_TEXT, k->error);
    }
    /* teclas */
    for (int r = 0; r < XUI_TK_ROWS; r++) {
        const char *row = k->rows[r] ? k->rows[r] : "";
        int rl = (int)strlen(row);
        for (int col = 0; col < XUI_TK_COLS && col < rl; col++) {
            char s1[2] = { row[col], 0 };
            key_btn(c, TK_X + col * (TK_KW + TK_GAP), TK_Y + r * (TK_KH + TK_GAP), TK_KW, TK_KH, s1,
                    !k->busy && k->row == r && k->col == col, 0, k->busy || k->len >= k->maxlen);
        }
    }
    int ay = TK_Y + XUI_TK_ROWS * (TK_KH + TK_GAP) + 8;
    const char *l1[4] = { k->shift_label ? k->shift_label : "ABC", k->sym_label ? k->sym_label : "#+=", L("Espaço"),
                          L("Apagar") };
    for (int a = 0; a < 4; a++) {
        int ax = TK_X + tk_a1[a][0] * (TK_KW + TK_GAP), aw = tk_a1[a][1] * (TK_KW + TK_GAP) - TK_GAP;
        key_btn(c, ax, ay, aw, TK_KH, l1[a], !k->busy && k->row == XUI_TK_ROWS && k->col == a, 0,
                k->busy || (a == 3 && k->len == 0) || (a == 2 && k->len >= k->maxlen));
    }
    ay += TK_KH + TK_GAP;
    const char *l2[2] = { k->show ? L("Ocultar senha") : L("Mostrar senha"), L("Conectar") };
    int ready = k->len >= k->minlen && k->len <= k->maxlen;
    for (int a = 0; a < 2; a++) {
        int ax = TK_X + tk_a2[a][0] * (TK_KW + TK_GAP), aw = tk_a2[a][1] * (TK_KW + TK_GAP) - TK_GAP;
        key_btn(c, ax, ay, aw, TK_KH, l2[a], !k->busy && k->row == XUI_TK_ROWS + 1 && k->col == a,
                a == 1 && ready && !k->busy, k->busy || (a == 1 && !ready));
    }
    if (k->hint && k->hint[0]) {
        int hw = draw_hints(c, 0, 0, k->hint, 1);
        draw_hints(c, KP_PX + KP_PW - 36 - hw, KP_PY + KP_PH + 40, k->hint, 0);
    }
}

xui_rect_t xui_textkey_frame(xui_view_t *v, xui_canvas_t *c, const xui_textkey_t *k, const xui_home_data_t *d,
                             unsigned now_ms, int *animating) {
    xui_rect_t none = { 0, 0, 0, 0 }, full = { 0, 0, v->w, v->h };
    xui_rect_t dirty = none;
    v->valid = 0;
    v->last_ms = now_ms;
    v->have_time = 1;
    /* a assinatura guarda a senha só como hash curto (nunca o texto) */
    unsigned hsh = 2166136261u;
    for (const char *p = k->typed ? k->typed : ""; *p; p++) hsh = (hsh ^ (unsigned char)*p) * 16777619u;
    char sig[600], num[96];
    int n = 0;
    snprintf(num, sizeof(num), "%d/%d/%d/%d/%d/%d/%08x", k->row, k->col, k->len, k->busy, k->show, k->maxlen, hsh);
    sig_cat(sig, &n, (int)sizeof(sig), num);
    sig_cat(sig, &n, (int)sizeof(sig), k->title);
    sig_cat(sig, &n, (int)sizeof(sig), k->text);
    for (int r = 0; r < XUI_TK_ROWS; r++) sig_cat(sig, &n, (int)sizeof(sig), k->rows[r]);
    sig_cat(sig, &n, (int)sizeof(sig), k->shift_label);
    sig_cat(sig, &n, (int)sizeof(sig), k->sym_label);
    sig_cat(sig, &n, (int)sizeof(sig), k->error);
    sig_cat(sig, &n, (int)sizeof(sig), k->status);
    sig_cat(sig, &n, (int)sizeof(sig), k->hint);
    if (!v->hero_valid) { build_hero(v, d, NULL); v->st_valid = 0; }
    int phase = (int)(now_ms / 300);
    if (!v->st_valid || v->st_kind != ST_KIND_TEXTKEY || strcmp(sig, v->st_sig) != 0) dirty = full;
    else if (k->busy && phase != v->st_phase) dirty = tk_dots_rect();
    if (!xui_rect_empty(dirty)) {
        xui_set_clip(c, dirty);
        render_textkey(v, c, k, d ? d->version : NULL, phase);
        xui_clip_all(c);
        v->stats.frames++;
        if (dirty.w == full.w && dirty.h == full.h) v->stats.full++; else v->stats.partial++;
    }
    v->st_valid = 1;
    v->st_kind = ST_KIND_TEXTKEY;
    v->st_prog = -1;
    v->st_phase = phase;
    scopy(v->st_sig, sizeof(v->st_sig), sig);
    if (animating) *animating = k->busy;
    return dirty;
}
