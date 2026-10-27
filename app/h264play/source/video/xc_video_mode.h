/*
 * orbita — modo de vídeo: escolha (cabo + EDID + escolha manual),
 * timing CEA, tabela 1080p (dados de dump) e o scaler 1280x720 -> TV.
 * Puro C (testado no host em hosttest/video/); o lado Xbox é
 * xc_video_xenon.c. Ver VIDEO-1080P.md.
 */
#ifndef XC_VIDEO_MODE_H
#define XC_VIDEO_MODE_H
#include <stddef.h>
#include <stdint.h>
#include "xc_edid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- cabo (AV pack do SMC, mesmos códigos do xenos_autoset_mode) ---- */
enum { XCV_AV_HDMI = 0, XCV_AV_COMPONENT, XCV_AV_VGA };
int xcv_av_kind(int avpack);
const char *xcv_av_name(int avpack);   /* "HDMI", "componente", "VGA", "AV comum" */

/* ---- escolha manual (XC360/VIDEO.TXT: "auto", "720p" ou "1080p") ---- */
enum { XCV_OVR_AUTO = 0, XCV_OVR_720P, XCV_OVR_1080P, XCV_OVR_COUNT };
int         xcv_ovr_parse(const char *s, size_t n);   /* desconhecido/vazio = AUTO */
/* v0.21.21: "1080" / "1080p" + letras de diagnóstico (VIDEO-1080P.md):
 *   n = sem scaler (1280x720 no canto da imagem 1080p)
 *   c = CRTC MVP/TRIG com as constantes do libxenon (não as do dump)
 *   x = sem os registradores D1MODE extras do dump (0x6520/24/78, 0x65e0)
 * "1080cx" = o caminho da v0.21.19. Letra desconhecida = AUTO. */
enum { XCV_F_NOSCALE = 1u, XCV_F_LXCRTC = 2u, XCV_F_NOEXTRA = 4u,
       /* v0.21.22: filtro do scaler (padrão = 2 taps bilinear fixo) */
       XCV_F_SCL_RHD = 8u,      /* r: controles do radeonhd (0x00030100) */
       XCV_F_SCL_NEAREST = 16u, /* e: ponto mais próximo (1 tap) */
       XCV_F_SCL_KERNEL = 32u   /* k: 6/5 taps do dump (RAM de coeficientes: imagem "contorno") */ };
int         xcv_ovr_parse_ex(const char *s, size_t n, unsigned *flags);
void        xcv_flags_str(unsigned flags, char *out, size_t n);   /* "nc" ou "" */
/* D1MODE extras do dump (pulados com a letra x) */
int         xcv_post_is_extra(uint16_t reg);

/* ---- v0.21.22: filtro do scaler D1SCL ----
 * O layout do D1SCL do Xenos é o do DCE da AMD (0x6590 = SCL_SCALER_ENABLE,
 * +4 por registrador): 0x6594 TAP_CONTROL (V taps-1 [2:0], H taps-1 [11:8]),
 * 0x65b0 HORZ_FILTER_CONTROL, 0x65b4 H ratio, 0x65b8 H init RGB, 0x65bc H init
 * chroma, 0x65c0 VERT_FILTER_CONTROL, 0x65c4 V ratio, 0x65c8 V init, 0x65cc
 * UPDATE. FILTER_CONTROL: bit 0 PICK_NEAREST, bit 8 2TAP_HARDCODE_COEF_EN
 * (coeficientes bilineares fixos, sem a RAM de coeficientes). Com 6/5 taps
 * (o que o kernel usa) o filtro lê a RAM de coeficientes, que o kernel
 * carregou e o libxenon não: a imagem sai como detecção de borda. */
enum { XCV_SCL_BILINEAR = 0, XCV_SCL_BILINEAR_RHD, XCV_SCL_NEAREST, XCV_SCL_KERNEL };
typedef struct {
    int      taps_h, taps_v;
    uint32_t tap;            /* 0x6594 */
    uint32_t hctl, vctl;     /* 0x65b0, 0x65c0 */
    uint32_t hinit, vinit;   /* 0x65b8, 0x65c8 */
} xcv_scl_t;
/* Fase inicial 16.16 como o kernel calcula: (razão + taps + 1) / 2,
 * arredondada pra múltiplo de 0x200 (6 taps, 2/3 -> 0x3d600; 5 -> 0x35600). */
uint32_t xcv_scl_init(uint32_t ratio24, int taps);
/* vctl_kernel = o 0x65c0 do dump (0x01000000) */
void     xcv_scl_filter(int kind, uint32_t hratio, uint32_t vratio, uint32_t vctl_kernel, xcv_scl_t *o);
int      xcv_scl_kind(unsigned flags);
const char *xcv_scl_name(int kind);
const char *xcv_ovr_name(int ovr);                     /* o texto gravado no arquivo */
int         xcv_ovr_next(int ovr);                     /* Auto -> 720p -> 1080p -> Auto */

/* ---- modo final ---- */
enum { XCV_HDMI_720P = 0, XCV_HDMI_1080P, XCV_YUV_720P, XCV_VGA_720P };
const char *xcv_mode_name(int mode);

typedef struct {
    int         mode;
    const char *why;    /* frase curta pro log [video] */
} xcv_choice_t;

/* edid = NULL se não leu; have_1080p = existe tabela 1080p validada pra
 * esta placa; force_720p = LB+RB segurados no boot (escape). */
xcv_choice_t xcv_choose(int avpack, const xc_edid_info_t *edid, int ovr, int have_1080p, int force_720p);

/* ---- timing ---- */
typedef struct {
    int      h_active, h_front, h_sync, h_back;
    int      v_active, v_front, v_sync, v_back;
    uint32_t pclk_khz;
} xcv_timing_t;
extern const xcv_timing_t XCV_CEA_720P60;    /* VIC 4:  74.25 MHz, 1650x750 */
extern const xcv_timing_t XCV_CEA_1080P60;   /* VIC 16: 148.5 MHz, 2200x1125 */

/* Os campos de struct mode_s (libxenon) que o xenos_set_mode_f1 usa. */
typedef struct {
    int total_w, total_h, active_w, active_h, hsync_offset, vsync_offset;
} xcv_crtc_t;
void     xcv_crtc_from_timing(const xcv_timing_t *t, xcv_crtc_t *c);
unsigned xcv_refresh_x100(const xcv_timing_t *t);   /* 6000 = 60.00 Hz */

/* ---- registradores ---- */
typedef struct { uint16_t reg; uint32_t val; } xcv_reg_t;

/* Razão do scaler AVIVO (fonte/destino em ponto fixo 8.24, arredondada).
 * 1280 -> 1920 e 720 -> 1080 = 2/3 = 0x00AAAAAB. */
uint32_t xcv_scale_ratio(int src, int dst);
/* Conjunto de reserva do scaler (viewport + enable + razões, offsets do
 * dump); só é usado se a tabela 1080p não trouxer o bloco D1MODE/D1SCL do
 * dump. Retorna quantos (<= cap). */
int xcv_scaler_regs(int src_w, int src_h, int dst_w, int dst_h, xcv_reg_t *out, int cap);

/* ---- tabela 1080p (xc_video_1080p_data.h, gerada por tools/video/dump2mode.py) ---- */
enum { XCV_BOARD_PRE_CORONA = 0, XCV_BOARD_CORONA = 1, XCV_BOARD_COUNT };
typedef struct {
    int             have;      /* 1 = veio de um dump real deste tipo de placa */
    const char     *source;    /* "dump Corona kernel 2.0.17559 (orbita-dump-1080p.txt)" */
    uint32_t        ana[256];  /* mode->ana do libxenon (0xffffffff = não lido) */
    xcv_crtc_t      crtc;
    const xcv_reg_t *post;     /* D1MODE/D1SCL depois do xenos_set_mode */
    int             n_post;
    const xcv_reg_t *crtc_post; /* v0.21.21: D1CRTC MVP/TRIG do kernel (0x6038-0x6068) */
    int             n_crtc_post;
} xcv_table_t;
const xcv_table_t *xcv_1080p_table(int board);   /* NULL se board inválido */
/* 1 = a tabela tem tudo o que o xenos_init_ana_new do libxenon escreve
 * (nada de 0xffffffff nesses índices). why: o índice que falta. */
int  xcv_ana_complete(const uint32_t ana[256], int corona, char *why, size_t n);
/* have && completa && timing 1080p coerente */
int  xcv_1080p_usable(int board);

#ifdef __cplusplus
}
#endif
#endif
