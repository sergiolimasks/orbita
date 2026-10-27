/*
 * orbita — EDID (bloco base + extensões CEA-861) -> resumo de modos.
 * Puro C, sem libxenon: o Xbox lê os bytes com xenos_get_edid() (DDC pelo
 * SMC) e os testes do host usam EDIDs reais (hosttest/video/).
 * Ver VIDEO-1080P.md.
 */
#ifndef XC_EDID_H
#define XC_EDID_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XC_EDID_BLOCK    128
#define XC_EDID_MAX_EXT  4      /* o mesmo teto do libxenon (MAX_EDID_EXT_NUM) */
#define XC_EDID_MAX_VIC  64
#define XC_EDID_MAX_DTD  12
#define XC_EDID_MAX_STD  8

typedef struct {
    uint16_t w, h;          /* ativo (h = linhas do quadro; entrelaçado: as duas metades) */
    uint16_t hz_x100;       /* 6000 = 60.00 Hz */
    uint8_t  interlaced;
    uint32_t pclk_khz;
} xc_edid_mode_t;

typedef struct {
    int      valid;           /* cabeçalho + checksum do bloco base ok */
    int      version, revision;
    int      digital;         /* entrada digital (HDMI/DVI) */
    int      hdmi;            /* VSDB HDMI (OUI 00-0C-03) numa extensão CEA */
    int      n_ext;           /* extensões declaradas */
    int      n_cea;           /* extensões CEA com checksum ok e lidas */
    int      cea_bad;         /* extensões CEA com checksum ruim (ignoradas) */
    char     mfg[4];          /* "SAM" */
    uint16_t product;
    char     name[14];        /* nome do monitor (descritor 0xFC), pode ser "" */
    int      n_vic;
    uint8_t  vic[XC_EDID_MAX_VIC];
    uint8_t  vic_native[XC_EDID_MAX_VIC];
    int      n_dtd;
    xc_edid_mode_t dtd[XC_EDID_MAX_DTD];   /* dtd[0] = preferido (bloco base) */
    int      n_std;
    xc_edid_mode_t std[XC_EDID_MAX_STD];
    /* o que a escolha do modo usa */
    int      has_720p60;      /* VIC 4/69, ou DTD/STD 1280x720 progressivo ~60 Hz */
    int      has_1080p60;     /* VIC 16/76, ou DTD/STD 1920x1080 progressivo ~60 Hz */
    int      has_1080i60;     /* VIC 5 ou DTD 1920x1080i */
} xc_edid_info_t;

/* Lê `len` bytes (128 * (1 + extensões lidas)). 0 = bloco base válido;
 * -1 = inválido (out->valid = 0, o resto zerado). Extensões que não cabem
 * em `len` ou com checksum ruim são ignoradas (contadas em cea_bad). */
int  xc_edid_parse(const uint8_t *buf, size_t len, xc_edid_info_t *out);

/* Linha curta pro log: "SAM 'SAMSUNG' hdmi pref 1920x1080p60.00 720p60=1
 * 1080p60=1 VIC 16*,4,31,..." (sem número de série). */
void xc_edid_summary(const xc_edid_info_t *e, char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
