/*
 * XCloud360 — XUI: visual novo do menu (estilo do site xCloud), C puro.
 *
 * Tudo aqui desenha num canvas XRGB em RAM comum (uint32 = 0x00RRGGBB no
 * endianness nativo), sem libxenon: o mesmo código roda no host (hosttest/ui,
 * gera PNG) e no Xbox (xui_fb_xenon.c copia o retângulo sujo pro framebuffer
 * do console, já no layout de tiles e na ordem de bytes do Xenos).
 *
 * Camadas:
 *   xui_gfx.c   primitivas (retângulo, cantos arredondados AA, degradê,
 *               escala bilinear, máscaras de alfa), recorte por clip
 *   xui_text.c  texto com a Inter rasterizada (xui_assets.h), UTF-8 ->
 *               Latin-1, kerning, quebra de linha e reticências
 *   xui_home.c  tela de jogos (hero + fileiras + botões de energia), telas de
 *               estado e diálogo de confirmação; caches de capa/hero e
 *               animações; diz o que ficou sujo
 *   xui_tile.c  canvas -> framebuffer em tiles 32x32 do Xenos (puro, medido
 *               no host)
 * Ver app/h264play/MENU-UI.md.
 */
#ifndef XCLOUD360_XUI_H
#define XCLOUD360_XUI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XUI_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define XUI_R(p) (((p) >> 16) & 0xff)
#define XUI_G(p) (((p) >> 8) & 0xff)
#define XUI_B(p) ((p) & 0xff)

/* Paleta medida nas capturas do site (hosttest/ui: ~/scratch/xcloud-ref). */
#define XUI_C_BG      XUI_RGB(27, 28, 30)      /* fundo da página */
#define XUI_C_PANEL   XUI_RGB(45, 48, 53)      /* cartões/painéis */
#define XUI_C_BUTTON  XUI_RGB(59, 62, 68)
#define XUI_C_PH      XUI_RGB(52, 55, 61)      /* capa ainda não carregada */
#define XUI_C_TEXT    XUI_RGB(255, 255, 255)
#define XUI_C_DIM     XUI_RGB(182, 185, 191)
#define XUI_C_FAINT   XUI_RGB(120, 124, 132)
#define XUI_C_GREEN   XUI_RGB(16, 124, 16)     /* só o botão A (cor do controle) */
/* v0.20.1: marca orbita (brand/BRAND.md): destaque "Orbita Sky" no lugar
 * do verde Xbox (foco, selos, progresso), acento "Flare" pra avisos raros. */
#define XUI_C_ACCENT  XUI_RGB(56, 182, 255)    /* #38B6FF */
#define XUI_C_FLARE   XUI_RGB(255, 107, 61)    /* #FF6B3D */
#define XUI_C_HAZE    XUI_RGB(154, 160, 166)   /* #9AA0A6: versão, legendas */
#define XUI_C_RED     XUI_RGB(232, 72, 72)
#define XUI_C_BLUE    XUI_RGB(52, 120, 220)
#define XUI_C_YELLOW  XUI_RGB(240, 190, 30)

#define XUI_MAX_W 2048           /* largura máx. de destino do xui_scale_cover */

typedef struct { int x, y, w, h; } xui_rect_t;

typedef struct {
    uint32_t  *px;
    int        w, h, stride;   /* stride em pixels */
    xui_rect_t clip;           /* nada é desenhado fora disto */
} xui_canvas_t;

/* Imagem fonte (capa, hero). fmt: XUI_FMT_XRGB = 0x00RRGGBB (o canvas);
 * XUI_FMT_UIRGB = o UI_RGB/XCC_PIX do catálogo, (b<<24)|(g<<16)|(r<<8),
 * convertido na escala (sem cópia extra). */
enum { XUI_FMT_XRGB = 0, XUI_FMT_UIRGB = 1 };
typedef struct {
    const uint32_t *px;
    int             w, h, stride;
    int             fmt;
} xui_image_t;

/* v0.20.1: imagem com alfa próprio (logo colorido). px = 0x00RRGGBB. */
typedef struct {
    const uint32_t      *px;
    const unsigned char *a;
    int                  w, h;
} xui_sprite_t;

/* Máscara de alfa 8 bits (ícone, glifo). */
typedef struct {
    const unsigned char *a;
    int                  w, h;
} xui_mask_t;

/* ---- fonte (atlas gerado por tools/gen_xui_assets.py) ---- */
typedef struct {
    int  off;
    unsigned char w, h;
    signed char bx, by;        /* canto sup. esq. relativo à origem na linha de base */
    unsigned short adv64;      /* avanço em 1/64 px */
} xui_glyph_t;
typedef struct { unsigned short pair; short k64; } xui_kern_t;
typedef struct {
    const unsigned char *bits;
    const xui_glyph_t   *glyphs;
    const short         *map;          /* Latin-1 -> glifo (-1 = não tem) */
    const xui_kern_t    *kern;         /* ordenado por pair = (a<<8)|b */
    int                  nkern;
    int                  ascent, descent, px;
} xui_face_t;

enum { XUI_F_BODY = 0, XUI_F_LABEL, XUI_F_ROW, XUI_F_TITLE, XUI_F_CODE, XUI_F_H2, XUI_F_COUNT };

/* ---- canvas / primitivas (xui_gfx.c) ---- */
void xui_canvas_init(xui_canvas_t *c, uint32_t *px, int w, int h, int stride);
void xui_set_clip(xui_canvas_t *c, xui_rect_t r);   /* recortado ao canvas */
void xui_clip_all(xui_canvas_t *c);
xui_rect_t xui_rect_union(xui_rect_t a, xui_rect_t b);   /* w<=0 = vazio */
xui_rect_t xui_rect_inter(xui_rect_t a, xui_rect_t b);
static inline int xui_rect_empty(xui_rect_t r) { return r.w <= 0 || r.h <= 0; }

static inline uint32_t xui_mix(uint32_t d, uint32_t s, unsigned a /*0..255*/) {
    a += a >> 7;                        /* 255 -> 256: opaco de verdade */
    unsigned na = 256 - a;
    uint32_t rb = (((s & 0xff00ff) * a + (d & 0xff00ff) * na) >> 8) & 0xff00ff;
    uint32_t g  = (((s & 0x00ff00) * a + (d & 0x00ff00) * na) >> 8) & 0x00ff00;
    return rb | g;
}

void xui_fill(xui_canvas_t *c, int x, int y, int w, int h, uint32_t col);
/* alpha 0..255 sobre o que já está no canvas */
void xui_fill_a(xui_canvas_t *c, int x, int y, int w, int h, uint32_t col, unsigned alpha);
/* retângulo de cantos arredondados anti-aliased (raio r px) */
void xui_round_rect(xui_canvas_t *c, int x, int y, int w, int h, int r, uint32_t col, unsigned alpha);
/* só a borda (espessura t), por fora de (x,y,w,h) com raio externo r */
void xui_round_frame(xui_canvas_t *c, int x, int y, int w, int h, int r, int t, uint32_t col);
void xui_circle(xui_canvas_t *c, int cx, int cy, int rad, uint32_t col, unsigned alpha);
/* degradê vertical: escurece de a0 (topo) até a1 (base) em direção a col */
void xui_vgrad(xui_canvas_t *c, int x, int y, int w, int h, uint32_t col, unsigned a0, unsigned a1);
void xui_hgrad(xui_canvas_t *c, int x, int y, int w, int h, uint32_t col, unsigned a0, unsigned a1);
/* cópia 1:1 de img (sem escala) em (x,y); r>0 = cantos arredondados */
void xui_blit(xui_canvas_t *c, const xui_image_t *img, int x, int y, int r);
/* v0.20.1: sprite RGBA por cima do canvas */
void xui_sprite(xui_canvas_t *c, const xui_sprite_t *s, int x, int y);
/* máscara de alfa pintada com col (alpha extra 0..255) */
void xui_mask(xui_canvas_t *c, const xui_mask_t *m, int x, int y, uint32_t col, unsigned alpha);
/* Escala bilinear de src (recorte cover: preenche dw x dh cortando o
 * excesso, centralizado) pra dst (buffer próprio, stride = dw). */
void xui_scale_cover(uint32_t *dst, int dw, int dh, const xui_image_t *src);
/* idem, mas só calcula as primeiras `rows` linhas do destino */
void xui_scale_cover_part(uint32_t *dst, int dw, int dh, const xui_image_t *src, int rows);
/* Cópia de região de outro buffer do mesmo tamanho do canvas (camada de fundo). */
void xui_copy_from(xui_canvas_t *c, const uint32_t *src, int src_stride, xui_rect_t r);

/* ---- texto (xui_text.c). Strings em UTF-8; o que não existe em Latin-1
 * vira um equivalente (aspas, travessão, reticências) ou some. ---- */
const xui_face_t *xui_face(int f);
/* largura em px; letter = espaçamento extra entre letras (px) */
int  xui_text_width(int f, const char *utf8, int letter);
/* desenha com a linha de base em y; retorna x final. max_w > 0 corta com "…" */
int  xui_text(xui_canvas_t *c, int f, int x, int y, uint32_t col, const char *utf8, int letter, int max_w);
/* centrado em [x, x+w) */
void xui_text_center(xui_canvas_t *c, int f, int x, int w, int y, uint32_t col, const char *utf8, int letter);
/* Quebra em até max_lines linhas de largura w (a última com "…"); align:
 * 0 = esquerda, 1 = centro. Retorna nº de linhas. line_h = 0 -> padrão. */
int  xui_text_wrap(xui_canvas_t *c, int f, int x, int y, int w, int line_h, int max_lines,
                   int align, uint32_t col, const char *utf8);
int  xui_line_h(int f);
/* Próximo caractere Latin-1 de uma string UTF-8 (0 = fim; avança *s). */
unsigned xui_utf8_next(const char **s);
/* UTF-8 -> Latin-1 (pra quem precisar de bytes, ex. testes); retorna len. */
int  xui_utf8_to_latin1(char *dst, int cap, const char *utf8);

/* ---- tiles do Xenos (xui_tile.c) ----
 * Copia o retângulo r do canvas pro fb no layout do console do libxenon
 * (blocos 32x32, ver menu_ui.c pset), já com os bytes trocados pro Xenos
 * (0x00RRGGBB nativo -> BGRX na memória). fb_stride = largura alinhada a 32.
 * r é alargado pra múltiplos de 4 em x. */
void xui_tile_copy(uint32_t *fb, int fb_stride, int fb_w, int fb_h,
                   const xui_canvas_t *c, xui_rect_t r);
/* Referência lenta (1 pixel por vez, mesma fórmula do menu_ui.c), pros testes. */
uint32_t xui_tile_offset(int x, int y, int fb_stride);

/* ================================================================
 * Dados que o desenho consome (preenchidos pelo main / catálogo)
 * ================================================================ */
enum {
    XUI_IT_GAMEPASS = 1 << 0,   /* jogo da assinatura: etiqueta "ASSINATURA" (v0.20.1) */
    XUI_IT_SESSION  = 1 << 1,   /* (calculado) é o jogo da sessão aberta */
};
typedef struct {
    const char *id;             /* chave das imagens (productId/titleId) */
    const char *name;           /* UTF-8 */
    unsigned    flags;          /* XUI_IT_* */
    int         game;           /* índice livre do chamador (ex. g_mm.games) */
} xui_item_t;
typedef struct {
    const char       *title;    /* UTF-8, ex. "Voltar a jogar" */
    const xui_item_t *items;
    int               n;
} xui_row_t;

/* Botões do topo à direita (foco ao subir além da 1ª fileira). */
/* v0.20.1: + Atualizar; v0.20.3: + Configurações (só com settings_avail) */
enum { XUI_HB_SETTINGS = 0, XUI_HB_UPDATE, XUI_HB_REBOOT, XUI_HB_SHUTDOWN, XUI_HB_COUNT };

typedef struct {
    const xui_row_t *rows;
    int              nrows;
    unsigned         rows_gen;   /* ++ quando o conteúdo das fileiras muda */
    /* foco: row = -1 -> botões do topo (col = XUI_HB_*) */
    int              row, col;
    /* Imagens: devolvem NULL se ainda não tem (placeholder/fallback). O
     * ponteiro precisa continuar válido até img_gen mudar. Capa: retrato
     * (qualquer tamanho, idealmente 2:3); hero: paisagem (idealmente 16:9). */
    const xui_image_t *(*cover)(void *u, const char *id);
    const xui_image_t *(*hero)(void *u, const char *id);
    void            *u;
    unsigned         img_gen;    /* ++ quando chega/sai imagem: redesenha */
    /* Sessão do xCloud ainda aberta (o jogo continua no servidor ao sair pro
     * menu): id do jogo e nome, ou NULL. A capa ganha o selo "EM JOGO" e o
     * hero diz "A: voltar ao jogo". */
    const char      *session_id;
    const char      *session_name;
    /* rodapé/topo: dica de botões ("A Jogar  Y Atualizar"), opcional */
    const char      *hint;
    const char      *notice;     /* aviso curto sob o hero (ex. erro do PLAY) */
    const char      *version;    /* v0.20.0: "v0.20.0" ao lado do logo (home e estados), opcional */
    /* v0.20.1: há versão nova: selo no botão Atualizar; update_label = rótulo
     * do botão em foco (ex. "Atualizar: v0.21.0"), NULL = "Atualizar" */
    int              update_avail;
    const char      *update_label;
    /* v0.20.3: botão Configurações (engrenagem) no topo: só pra quem tem
     * chave beta válida */
    int              settings_avail;
    /* v0.21.29: conexão ao lado do logo: 0 = sem conexão, 1 = cabo, 2 = Wi-Fi */
    int              net_kind;
} xui_home_data_t;

/* Telas de estado (mesmo estilo). */
enum {
    XUI_ST_LOADING = 0,   /* carregando catálogo / login em andamento */
    XUI_ST_LOGIN,         /* código do device login */
    XUI_ST_STARTING,      /* iniciando jogo */
    XUI_ST_QUEUE,         /* na fila */
    XUI_ST_ERROR,
    XUI_ST_BUSY,          /* "Encerrando sessão...", "Desligando..." */
};
typedef struct {
    int         kind;
    const char *title;       /* UTF-8 */
    const char *text;        /* parágrafo (quebra sozinho) */
    const char *url;         /* LOGIN */
    const char *code;        /* LOGIN */
    const char *detail;      /* linha pequena (status do login, posição...) */
    const char *hint;        /* "B Cancelar" etc.: "A ...", "B ...", "X ...", "Y ..." */
    const char *game_id;     /* fundo: hero/capa desse jogo, se houver */
    int         progress;    /* 0..100, ou -1 = indeterminado (pontinhos) */
} xui_status_t;

/* Diálogo de confirmação (Reiniciar/Desligar). */
typedef struct {
    const char *title;       /* "Desligar o Xbox?" */
    const char *text;
    const char *yes;         /* "Desligar" */
    const char *no;          /* "Cancelar" */
    int         focus;       /* 0 = no (padrão), 1 = yes */
} xui_confirm_t;

/* v0.20.3: tela da chave beta com teclado na tela (modelo em
 * beta/xc_keypad.h: 4 fileiras de 8 com o alfabeto das chaves + fileira de
 * ações Apagar / Ler do pendrive / Confirmar). */
#define XUI_KEY_COLS 8
#define XUI_KEY_ROWS 4
typedef struct {
    const char *title;        /* "Chave beta" */
    const char *text;         /* explicação (quebra sozinho) */
    const char *alphabet;     /* XCB_ALPHABET (31 teclas) */
    const char *typed;        /* "ABCDE-FG" (hífen automático) */
    int         len;          /* letras digitadas (0..10) */
    int         row, col;     /* foco; row = XUI_KEY_ROWS -> ações (0 Apagar, 1 Pendrive, 2 Confirmar) */
    const char *console_id;   /* curto, "A1B2-C3D4-E5F6" */
    const char *error;        /* linha vermelha (NULL = nenhuma) */
    const char *status;       /* com busy: "Validando…" + pontinhos */
    int         busy;
    const char *hint;
} xui_key_t;

/* v0.20.3: Configurações (topo, engrenagem). */
typedef struct {
    const char *console_short;  /* "A1B2-C3D4-E5F6" */
    const char *console_full;   /* 32 hex */
    const char *key_masked;     /* "*****-**HJK" */
    const char *key_status;     /* "Validada em 25/10 22:56" */
    const char *version;        /* "orbita v0.20.3 (beta)" */
    const char *note;           /* "Para reportar um bug, envie um print desta tela junto" */
    const char *notice;         /* aviso curto (ex. "Chave trocada") */
    const char *button;         /* "Trocar chave" */
    const char *hint;
    /* v0.21.0: segundo botão ("Wi-Fi") e qual está em foco (0 = button, 1 = button2) */
    const char *button2;
    const char *button2_note;   /* linha curta ao lado (ex. "Cabo de rede", "Casa · conectado") */
    int         focus;
} xui_settings_t;

/* ---- v0.21.0: Wi-Fi (WIFI.md) ---- */
typedef struct {
    const char *ssid;          /* UTF-8 */
    int         bars;          /* 0..4 */
    int         lock;          /* rede com senha */
    const char *tag;           /* "Conectada", "Salva", "Não suportada" ou NULL */
    int         dim;           /* não dá pra conectar (segurança não suportada) */
} xui_wifi_net_t;

enum { XUI_WS_NEUTRAL = 0, XUI_WS_OK, XUI_WS_BUSY, XUI_WS_ERROR };

typedef struct {
    const char *status_title;  /* "Wi-Fi: Casa", "Cabo de rede", "Desconectado" */
    const char *status_text;   /* IP, sinal, erro... */
    int         status_kind;   /* XUI_WS_* */
    const char *saved;         /* "Rede salva: Casa" ou NULL */
    const xui_wifi_net_t *nets;
    int         nnets;
    /* foco: -2 = "Procurar redes", -1 = "Esquecer rede", 0.. = rede da lista */
    int         focus;
    int         can_forget;    /* sem rede salva o botão fica apagado */
    int         scanning;      /* "Procurando…" + pontinhos */
    const char *empty;         /* lista vazia: texto no lugar */
    const char *hint;
} xui_wifi_t;

/* Teclado de texto (senha). Modelo em beta/xc_keypad.h (xtk_*): 10 colunas,
 * 4 fileiras de caracteres da página atual + 2 fileiras de ações. */
#define XUI_TK_COLS 10
#define XUI_TK_ROWS 4
typedef struct {
    const char *title;         /* "Senha do Wi-Fi" */
    const char *text;          /* "Rede: Casa" (quebra sozinho) */
    const char *rows[XUI_TK_ROWS];   /* as 4 fileiras da página (10 caracteres cada) */
    const char *shift_label;   /* "ABC" / "abc" */
    const char *sym_label;     /* "#+=" / "abc" */
    const char *typed;         /* o que foi digitado (ASCII) */
    int         len, minlen, maxlen;
    int         show;          /* 0 = bolinhas */
    int         row, col;      /* foco; row 4/5 = ações (col = ação) */
    const char *error;
    const char *status;        /* com busy */
    int         busy;
    const char *hint;
} xui_textkey_t;

/* ================================================================
 * Estado de tela (caches + animação). Um por app; só o main usa.
 * ================================================================ */
typedef struct xui_view xui_view_t;

/* W x H do canvas (1280x720). Aloca caches (~10 MB). NULL = sem memória. */
xui_view_t *xui_view_new(int w, int h);
void        xui_view_free(xui_view_t *v);
/* Esquece tudo (próximo quadro redesenha a tela inteira). */
void        xui_view_invalidate(xui_view_t *v);
/* Próximo quadro redesenha a tela toda SEM mexer em rolagem/animação (o
 * canvas foi sobrescrito por outra coisa; testes de sujeira). */
void        xui_view_repaint(xui_view_t *v);

/* Tela de jogos. Avança animações até now_ms, desenha só o que mudou no
 * canvas e devolve o retângulo sujo (w=0: nada mudou). Chamar a cada volta
 * do loop do menu; quando *animating = 0 dá pra dormir até o próximo input.
 * confirm != NULL: escurece e põe o diálogo por cima. */
xui_rect_t  xui_home_frame(xui_view_t *v, xui_canvas_t *c, const xui_home_data_t *d,
                           const xui_confirm_t *confirm, unsigned now_ms, int *animating);
/* Telas de estado (mesma lógica de sujeira; os pontinhos animam). */
xui_rect_t  xui_status_frame(xui_view_t *v, xui_canvas_t *c, const xui_status_t *s,
                             const xui_home_data_t *d, unsigned now_ms, int *animating);

/* v0.20.3: chave beta (teclado) e configurações. Mesma lógica de sujeira
 * das telas de estado: conteúdo mudou = tela inteira; só os pontinhos do
 * "Validando…" = retângulo pequeno. */
xui_rect_t  xui_key_frame(xui_view_t *v, xui_canvas_t *c, const xui_key_t *k,
                          const xui_home_data_t *d, unsigned now_ms, int *animating);
/* v0.21.0: Wi-Fi (lista de redes) e teclado de texto (senha). Mesma
 * lógica de sujeira: conteúdo mudou = tela inteira; só os pontinhos do
 * "Procurando…"/"Conectando…" = retângulo pequeno. */
xui_rect_t  xui_wifi_frame(xui_view_t *v, xui_canvas_t *c, const xui_wifi_t *w,
                           const xui_home_data_t *d, unsigned now_ms, int *animating);
xui_rect_t  xui_textkey_frame(xui_view_t *v, xui_canvas_t *c, const xui_textkey_t *k,
                              const xui_home_data_t *d, unsigned now_ms, int *animating);
xui_rect_t  xui_settings_frame(xui_view_t *v, xui_canvas_t *c, const xui_settings_t *s,
                               const xui_home_data_t *d, unsigned now_ms, int *animating);

/* Medidas pra testes/bench. */
typedef struct {
    unsigned frames, full, partial;
    unsigned hero_builds, card_scales;
} xui_stats_t;
void xui_view_stats(const xui_view_t *v, xui_stats_t *out);

/* O que a tela mostra / vai mostrar logo, em ordem de importância (foco,
 * resto da fileira em foco, fileira de baixo, de cima, a seguinte), pra
 * pedir as capas ao catálogo (xcc_want). Devolve quantos itens pôs em out. */
int  xui_home_visible(const xui_view_t *v, const xui_home_data_t *d, const xui_item_t **out, int max);

#ifdef __cplusplus
}
#endif

#endif
