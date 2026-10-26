/*
 * XCloud360 — camada de dados do menu de capas (fileiras tipo xbox.com/play).
 * Ver app/h264play/CATALOG-COVERS.md.
 *
 * Fileiras: "Voltar a jogar" (GSSV /v2/titles/mru, pego no login), "Mais
 * populares na nuvem" e "Adicionados recentemente" (listas SIGL públicas do
 * catalog.gamepass.com, filtradas pelo catálogo da conta) e "Todos os jogos"
 * (a lista do login, alfabética). Capas/hero: URLs do displaycatalog
 * (Images), baixadas já redimensionadas do store-images, guardadas no
 * pendrive (XC360/COVERS, XC360/HERO) e decodificadas (stb_image) em pixels
 * UI_RGB (o XUI converte na escala, source/ui/xui_cat.c).
 *
 * THREADS
 *   main (hw 0): xcc_init, xcc_poll (1x por quadro), xcc_rows_copy, xcc_want,
 *     xcc_cover_get/xcc_hero_get, xcc_mru_touch, xcc_bg_start/stop/running.
 *     Nada disso bloqueia nem toca rede/USB.
 *   worker (hw 3, XCC_HW_THREAD): rede (HTTPS sobre o net_pipe) + pendrive
 *     (xc_store_*, que já faz o xc_usb_hold) + decode JPEG. Só roda quando o
 *     main chama xcc_bg_start — com a hw 3 livre (sem login, sem sessão, fora
 *     do jogo). xcc_bg_stop interrompe o pedido em voo (xcn_cancelled).
 *   login (hw 3, login_task): ganchos lf_hooks_t (login_flow.h) — carrega o
 *     cache no começo, busca o MRU com o gsToken, recebe a lista final.
 * Fila: o main publica o que está visível (xcc_want); o worker atende isso
 * primeiro e, sem pedido, baixa o resto pro pendrive aos poucos.
 * Os pixels são do main: um ponteiro de xcc_cover_get vale até o próximo
 * xcc_poll() (é ali que o LRU entra/sai).
 */
#ifndef XC360_CATALOG_H
#define XC360_CATALOG_H
#include <stddef.h>
#include <stdint.h>
#include "../login/xbox_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XCC_HW_THREAD
#define XCC_HW_THREAD 3
#endif

enum { XCC_ROW_RECENT = 0, XCC_ROW_POPULAR, XCC_ROW_NEW, XCC_ROW_ALL, XCC_NROWS };
enum { XCC_COVER = 0, XCC_HERO = 1 };

#define XCC_ROW_MAX      40     /* recentes / populares / novos */
#define XCC_ALL_MAX      1024   /* "Todos os jogos" */
#define XCC_COVER_W      176
#define XCC_COVER_H      264
/* v0.20.0: hero em 1280x720 (era 640x360 ampliado 2x no menu, macio): o
 * XUI só copia, nítido. ~3,7 MB decodificado cada (ver max_heroes). */
#define XCC_HERO_W       1280
#define XCC_HERO_H       720

/* Pixel UI_RGB (XUI_FMT_UIRGB do xui.h): (b<<24)|(g<<16)|(r<<8). */
#define XCC_PIX(r, g, b) (((uint32_t)(b) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(r) << 8))

typedef struct {
    char     id[48];        /* titleId (PLAY usa este) */
    char     product[24];   /* bigId da Store (capas usam este) */
    char     name[64];      /* UTF-8 (v0.20.0; antes ASCII dobrado) */
    unsigned flags;         /* XA_F_* */
} xcc_item_t;

typedef struct {
    char        title[32];  /* "Voltar a jogar"... (ASCII, cabe na fonte 8x16) */
    int         n;
    xcc_item_t *items;      /* aponta pra dentro do xcc_rows_t */
} xcc_row_t;

/* Cópia do main (grande: ~150 KB; deixe estática). */
typedef struct {
    unsigned   seq;                         /* 0 = nunca copiado */
    xcc_row_t  row[XCC_NROWS];
    xcc_item_t recent[XCC_ROW_MAX], popular[XCC_ROW_MAX], fresh[XCC_ROW_MAX];
    xcc_item_t all[XCC_ALL_MAX];
} xcc_rows_t;

typedef struct {
    int             w, h;
    const uint32_t *px;     /* w*h pixels XCC_PIX, linha a linha */
} xcc_image_t;

/* Resposta de um GET (corpo malloc, terminado em NUL). */
typedef int (*xcc_fetch_fn)(const char *host, const char *path, size_t max_body,
                            int *status, char **body, size_t *len);

typedef struct {
    const char  *market;          /* "BR" */
    const char  *language;        /* "pt-BR" */
    int          max_covers;      /* LRU de capas decodificadas (RAM) */
    int          max_heroes;
    int          bg_pace_ms;      /* pausa entre downloads de fundo */
    int          mru_max;         /* mr= do /v2/titles/mru */
    xcc_fetch_fn fetch;           /* NULL = HTTPS (keep-alive por host) */
} xcc_config_t;

void xcc_config_defaults(xcc_config_t *c);

/* ---------------------------------------------------------------- main */
/* Uma vez, no boot, antes do lt_start (registra os ganchos do login). */
int  xcc_init(const xcc_config_t *cfg);   /* cfg NULL = padrão */
/* 1x por quadro no menu: entrega capas decodificadas ao LRU, despeja velhas. */
void xcc_poll(void);
/* Muda quando fileiras ou capas mudam (redesenhar). */
unsigned xcc_seq(void);
/* Copia as fileiras se mudaram desde out->seq. 1 = copiou, 0 = igual. */
int  xcc_rows_copy(xcc_rows_t *out);
/* O que está na tela agora (bigIds), na ordem de importância. kind =
 * XCC_COVER ou XCC_HERO. Substitui o pedido anterior daquele tipo. */
void xcc_want(int kind, const char *const *products, int n);
/* NULL = ainda não (já entra na fila se não estava). */
const xcc_image_t *xcc_cover_get(const char *product);
const xcc_image_t *xcc_hero_get(const char *product);
/* Depois de um PLAY: põe o título no começo de "Voltar a jogar" (como o
 * PREPEND_MRU_TITLES do site) e grava no pendrive na próxima vez do worker. */
void xcc_mru_touch(const char *title_id);
/* Worker na hw 3. Só chame com !lt_running() e sem sessão (xn_can_play()).
 * 0 = subiu (ou já rodava), -1 = falhou. */
int  xcc_bg_start(void);
/* Pede pra parar (não bloqueia); antes do PLAY espere !xcc_bg_running(). */
void xcc_bg_stop(void);
int  xcc_bg_running(void);
/* v0.20.1: trabalho extra que roda na MESMA thread do worker (hw 3), a cada
 * volta, depois do xcc_worker_step: o upload do log beta (xlu_step /
 * xlu_finish). step: 1 = trabalhou. Chamar antes do 1o xcc_bg_start. */
void xcc_set_worker_extra(int (*step)(void), void (*finish)(void));

typedef struct {
    int      loaded_cache, rows_net, mru_net;     /* 1 = feito nesta sessão */
    int      n_index, n_with_urls, n_covers_disk, n_heroes_disk;
    int      ram_covers, ram_heroes;
    unsigned downloads, decodes, disk_hits, net_errors, decode_errors;
    char     last_err[96];
} xcc_status_t;
void xcc_status(xcc_status_t *s);

/* ---------------------------------------------- ganchos do login (hw 3) */
void xcc_hook_start(void);
void xcc_hook_cloud(const xa_endpoint_t *ep);
void xcc_hook_titles(const xa_title_t *t, int n);
/* Lista do MRU já baixada (o hook_cloud chama; testes também). */
void xcc_set_mru(const xa_title_t *t, int n);

/* ------------------------------------------- worker (host testa direto) */
/* Um passo de trabalho. 1 = fez algo, 0 = nada a fazer agora. */
int  xcc_worker_step(void);
/* Fim do worker: grava o que falta e fecha as conexões. */
void xcc_worker_finish(void);
/* Libera tudo (testes). */
void xcc_shutdown(void);
/* Testes: zera backoffs/espera de rede (o host não tem relógio falso). */
void xcc_debug_reset_backoff(void);

/* --------------------------------------------- partes puras (hosttest) */
/* SIGL v2/v3: [{"siglId":..,"title":..},{"id":"9N.."},...] -> ids (malloc). */
int  xcc_parse_sigl(const char *json, char (**ids)[24], int *n, char *title, size_t tcap);
typedef struct {
    char product[24];
    char cover[160];        /* caminho em store-images.s-microsoft.com, "" = nenhum */
    char hero[160];
} xcc_imgref_t;
/* displaycatalog /v7.0/products -> refs (malloc), 1 por produto. */
int  xcc_parse_images(const char *json, xcc_imgref_t **out, int *n);
/* URL completa do caminho + tamanho (?w=&h=&q=&mode=crop). */
void xcc_image_path(char *dst, size_t cap, const char *path, int kind);
/* Nome 8.3 do arquivo no pendrive: "COVERS/1A2B3C4D.JPG" / "HERO/1A2B3C4D.JPG". */
void xcc_file_name(char *dst, size_t cap, const char *product, int kind);
/* JPEG -> pixels XCC_PIX exatamente w x h (escala se precisar). malloc. */
uint32_t *xcc_decode_jpeg(const uint8_t *jpg, size_t len, int w, int h);

#ifdef __cplusplus
}
#endif
#endif
