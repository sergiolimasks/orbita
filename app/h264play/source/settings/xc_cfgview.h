/*
 * orbita — o que as Configurações mostram (v0.21.33, MENU-SETTINGS-PLAN.md).
 *
 * C puro: recebe o estado do app já "achatado" (xcs_in_t, o main preenche
 * a partir de xcb/xcu/xw/xcv/áudio/PAD.TXT) + a navegação (xc_cfgnav) e
 * monta o xui_cfg_t que o xui_cfg_frame desenha: textos, linhas, dicas,
 * opções focáveis de cada seção. O mockup do host (hosttest/ui/mockup.c)
 * usa o MESMO código, então os PNGs saem do texto real.
 *
 * Também a calibração de um analógico (xcs_cal_*): foco, sliders, Y
 * padrão, Medir sozinho e o rastro da prévia.
 */
#ifndef XC_CFGVIEW_H
#define XC_CFGVIEW_H
#include "../ui/xc_i18n.h"   /* v0.21.38 */
#include <stdint.h>

#include "../ui/xui.h"
#include "../input/xc_padcfg.h"
#include "xc_cfgnav.h"

#ifdef __cplusplus
extern "C" {
#endif

/* opções (o que o A / esquerda-direita fazem) */
enum {
    XCS_OPT_NONE = 0,
    XCS_OPT_KEY,        /* Chave beta -> Trocar chave */
    XCS_OPT_NOTES,      /* Novidades desta versão */
    XCS_OPT_UPDATE,     /* Procurar atualização / Atualizar para vX */
    XCS_OPT_WIFI,       /* abre a tela do Wi-Fi */
    XCS_OPT_720,        /* grava VIDEO.TXT = 720p */
    XCS_OPT_1080,       /* grava VIDEO.TXT = 1080p */
    XCS_OPT_VOLUME,     /* 100/150/200/250% (esquerda/direita) */
    XCS_OPT_PAD_L,      /* calibração (começa sempre pelo esquerdo) */
    XCS_OPT_PAD_R,
    XCS_OPT_PAD_RESET,
    XCS_OPT_OVERLAY,    /* v0.21.43: Desempenho > Mostrar no jogo (liga/desliga) */
    XCS_OPT_SPEED,      /* v0.21.43: Rede > Teste de velocidade */
    XCS_OPT_LANG,       /* v0.21.38: idioma dos jogos: LANG + índice (lista, A escolhe) */
    XCS_OPT_ULANG = XCS_OPT_LANG + 8   /* v0.21.38: idioma do app: ULANG + XI_* */
};

/* Wi-Fi como a seção Rede mostra */
enum { XCS_W_NONE = 0, XCS_W_STARTING, XCS_W_FAILED, XCS_W_UP, XCS_W_CONNECTING, XCS_W_DOWN };

#define XCS_NVOL 4
#define XCS_NLANG 4
extern const char *const XCS_LANG_CODES[XCS_NLANG];   /* pt-BR, pt-PT, en-US, es-MX */
int xcs_lang_index(const char *code);
extern const int XCS_VOLUMES[XCS_NVOL];   /* 100, 150, 200, 250 */

typedef struct {
    /* Conta e sobre */
    int         beta_build;                 /* XCLOUD360_BETA */
    int         key_row;                    /* mostra "Chave beta" (BETA e autorizado) */
    const char *console_short, *console_full;
    const char *version;                    /* "v0.21.33" */
    const char *channel;                    /* "beta" / "stable" / "" (não checou) */
    const char *key_masked;                 /* "*****-**P9H" */
    const char *key_when;                   /* "validada em 25/10 22:56" */
    int         upd_phase;                  /* XCU_* */
    int         upd_busy;                   /* procurando (pendente ou rodando) */
    int         upd_avail;                  /* versão nova conhecida */
    const char *upd_latest;
    /* Rede */
    int         cable;                      /* stream_net_cable(): DHCP do cabo (o PHY mente no Corona) */
    uint32_t    cable_ip;                   /* a.b.c.d como (a<<24)|..|d */
    int         wifi;                       /* XCS_W_* */
    const char *wifi_ssid;
    int         wifi_rssi;
    int         wifi_dhcp;
    uint32_t    wifi_ip;
    /* Vídeo */
    int         av_kind;                    /* XCV_AV_* (-1 = não sei) */
    int         mode;                       /* XCV_HDMI_720P... (o que está na TV agora) */
    int         ovr;                        /* XCV_OVR_* escolhido (VIDEO.TXT / tela) */
    int         have_1080p;                 /* tabela 1080p pra esta placa */
    int         forced_720p;                /* LB+RB no boot */
    /* Som */
    int         audio;                      /* build com áudio */
    int         volume;                     /* % (50..400) */
    const char *lang;                       /* v0.21.38: idioma dos jogos (código) */
    int         ui_lang;                    /* v0.21.38: XI_* */
    /* Controles */
    xpc_cfg_t   pad;
    /* Novidades */
    const char *notes;                      /* texto das notas desta versão (ou NULL) */
    const char *notes_when;                 /* "26/10/2026" (NULL = não sabe) */
    int         notes_from_file;            /* 1 = NOTES.TXT, 0 = manifesto */
    /* v0.21.43: Desempenho (-1 = sem dado) */
    int         perf_cpu_c10, perf_gpu_c10, perf_cpu_pct, perf_gpu_pct, perf_fps10;
    int         perf_audio_ms10, perf_pad_ms100, perf_net_pct, perf_dec_ms10, perf_live, perf_overlay;
    int         speed_state, speed_mbps10, speed_lat_ms;   /* XLU_SPD_* */
    int         perf_rtt_ms10;   /* latência do jogo (WebRTC), -1 = sem */
    const char *speed_msg;
    /* aviso curto (ex. "Chave trocada") */
    const char *notice;
} xcs_in_t;

/* ---- calibração ---- */
#define XCS_CAL_TRAIL XUI_CAL_TRAIL
typedef struct {
    int        stick;          /* XPC_LEFT / XPC_RIGHT */
    int        focus;          /* 0 zona morta, 1 limite, 2 Medir sozinho, 3 Próximo/Concluir */
    xpc_cfg_t  cfg;            /* em edição (publicada pelo main a cada mudança) */
    xpc_meas_t meas;
    int        x, y;           /* posição ao vivo (bruta, Y positivo = cima) */
    int        tx[XCS_CAL_TRAIL], ty[XCS_CAL_TRAIL], ntrail;
    uint32_t   trail_ms;
    int        port, multi;    /* controle lido; mais de um mexeu */
    int        last_result;    /* 1 = Medir sozinho terminou (mostra o resultado) */
    char       status[120];
} xcs_cal_t;

enum { XCS_CAL_NONE = 0, XCS_CAL_CHANGED, XCS_CAL_REDRAW, XCS_CAL_DONE };
void xcs_cal_begin(xcs_cal_t *k, const xpc_cfg_t *cur, int port);
/* tecla XCF_K_* (D-pad: UP/DOWN/LEFT/RIGHT; A; Y). DONE = "Concluir". */
int  xcs_cal_key(xcs_cal_t *k, int key, uint32_t now_ms);
/* posição do controle lido (x, y brutos; Y positivo = cima). CHANGED =
 * o Medir sozinho terminou e mudou a config. */
int  xcs_cal_feed(xcs_cal_t *k, uint32_t now_ms, int x, int y);

/* ---- a tela ---- */
typedef struct {
    xui_cfg_t     cfg;
    xui_cfg_row_t rows[8];
    int           opt_row[XCF_MAX_OPT];   /* opção -> linha */
    xui_calib_t   cal;
    xui_confirm_t confirm;
    char          b_title[96], b_info1[64], b_info2[64], b_key[96], b_notes[96], b_upd[64], b_upd_v[64];
    char          b_card_t[96], b_card_x[128], b_after[200], b_vol[64], b_lpad[48], b_rpad[48], b_cal_pad[24];
    char          b_cal_status[120], b_sub[120], b_conf[200];
    char          b_p[5][96];   /* v0.21.43: Desempenho */
    char          b_spd[96];
} xcs_view_t;

/* Opções focáveis da seção agora: ids em ids[] (XCS_OPT_*), bits de
 * "aceita esquerda/direita" em *adj. Retorna quantas. */
int  xcs_options(const xcs_in_t *in, int sec, int *ids, unsigned *adj);
/* Monta a tela. cal só na página da calibração; scroll = parágrafos pulados das notas. */
void xcs_build(xcs_view_t *o, const xcs_in_t *in, const xcf_nav_t *nav, const xcs_cal_t *cal, int scroll,
               uint32_t now_ms);
/* parágrafos das notas (pra prender a rolagem) */
int  xcs_notes_paras(const char *notes);
/* "HDMI  ·  720p  ·  60 Hz" */
const char *xcs_out_text(int mode);
/* volume % -> índice em XCS_VOLUMES (-1 = personalizado) e o próximo na direção dir */
int  xcs_vol_index(int pct);
int  xcs_vol_step(int pct, int dir);

#ifdef __cplusplus
}
#endif
#endif
