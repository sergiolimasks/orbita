/*
 * orbita — escolha do modo de vídeo + timing + scaler (puro C).
 * Ver xc_video_mode.h e VIDEO-1080P.md.
 */
#include "xc_video_mode.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* A tabela 1080p (dados). É o ÚNICO arquivo que muda quando chega um dump:
 * python3 tools/video/dump2mode.py gen orbita-dump-1080p.txt ... */
#ifdef XCV_1080P_DATA_FILE      /* hosttest: tabela gerada de um dump sintético */
#include XCV_1080P_DATA_FILE
#else
#include "xc_video_1080p_data.h"
#endif

int xcv_av_kind(int avpack) {
    switch (avpack) {
    case 0x0C: case 0x0F: case 0x4F: return XCV_AV_COMPONENT;
    case 0x5B: case 0x59: case 0x1B: return XCV_AV_VGA;
    default: return XCV_AV_HDMI;   /* HDMI e o resto (como o main.c fazia) */
    }
}

const char *xcv_av_name(int avpack) {
    switch (avpack) {
    case 0x13: case 0x14: case 0x1C: case 0x1E: case 0x1F: return "HDMI";
    case 0x0C: case 0x0F: return "componente";
    case 0x4F: return "AV (modo HD)";
    case 0x5B: case 0x59: case 0x1B: return "VGA";
    case 0x43: case 0x47: case 0x54: case 0x57: return "AV comum/SCART";
    default: return "desconhecido";
    }
}

int xcv_ovr_parse(const char *s, size_t n) {
    unsigned f;
    return xcv_ovr_parse_ex(s, n, &f);
}

int xcv_post_is_extra(uint16_t reg) {
    return reg == 0x6520 || reg == 0x6524 || reg == 0x6578 || reg == 0x65e0;
}

void xcv_flags_str(unsigned f, char *o, size_t n) {
    if (!n) return;
    snprintf(o, n, "%s%s%s%s%s%s", (f & XCV_F_NOSCALE) ? "n" : "", (f & XCV_F_LXCRTC) ? "c" : "",
             (f & XCV_F_NOEXTRA) ? "x" : "", (f & XCV_F_SCL_RHD) ? "r" : "", (f & XCV_F_SCL_NEAREST) ? "e" : "",
             (f & XCV_F_SCL_KERNEL) ? "k" : "");
}

uint32_t xcv_scl_init(uint32_t ratio24, int taps) {
    unsigned long long v = ((unsigned long long)ratio24 + ((unsigned long long)(taps + 1) << 24)) / 2;  /* 8.24 */
    uint32_t x = (uint32_t)(v >> 8);                                                                      /* 16.16 */
    return (x + 0x100) & ~0x1FFu;
}

int xcv_scl_kind(unsigned f) {
    if (f & XCV_F_SCL_KERNEL) return XCV_SCL_KERNEL;
    if (f & XCV_F_SCL_NEAREST) return XCV_SCL_NEAREST;
    if (f & XCV_F_SCL_RHD) return XCV_SCL_BILINEAR_RHD;
    return XCV_SCL_BILINEAR;
}

const char *xcv_scl_name(int k) {
    switch (k) {
    case XCV_SCL_BILINEAR_RHD: return "2 taps bilinear fixo (controles radeonhd)";
    case XCV_SCL_NEAREST: return "ponto mais proximo";
    case XCV_SCL_KERNEL: return "6/5 taps do kernel (RAM de coeficientes)";
    default: return "2 taps bilinear fixo";
    }
}

void xcv_scl_filter(int kind, uint32_t hr, uint32_t vr, uint32_t vctl_kernel, xcv_scl_t *o) {
    memset(o, 0, sizeof(*o));
    switch (kind) {
    case XCV_SCL_KERNEL:
        o->taps_h = 6; o->taps_v = 5;
        o->hctl = 0; o->vctl = vctl_kernel;
        break;
    case XCV_SCL_NEAREST:
        o->taps_h = 1; o->taps_v = 1;
        o->hctl = 0x1; o->vctl = vctl_kernel | 0x1;
        break;
    case XCV_SCL_BILINEAR_RHD:   /* radeonhd DxScaleSet (R5xx): TAP 0x101, FILTER 0x00030100 */
        o->taps_h = 2; o->taps_v = 2;
        o->hctl = 0x00030100; o->vctl = 0x00030100;
        break;
    default:
        o->taps_h = 2; o->taps_v = 2;
        o->hctl = 0x100; o->vctl = vctl_kernel | 0x100;
        break;
    }
    o->tap = ((uint32_t)(o->taps_h - 1) << 8) | (uint32_t)(o->taps_v - 1);
    o->hinit = xcv_scl_init(hr, o->taps_h);
    o->vinit = xcv_scl_init(vr, o->taps_v);
}

int xcv_ovr_parse_ex(const char *s, size_t n, unsigned *flags) {
    char w[16];
    *flags = 0;
    size_t k = 0;
    for (size_t i = 0; s && i < n && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) { if (k) break; continue; }
        if (k < sizeof(w) - 1) w[k++] = (char)tolower(c);
    }
    w[k] = 0;
    if (!strcmp(w, "720p") || !strcmp(w, "720")) return XCV_OVR_720P;
    if (!strncmp(w, "1080", 4)) {
        const char *p = w + 4;
        if (*p == 'p') p++;
        unsigned f = 0;
        for (; *p; p++) {
            if (*p == 'n') f |= XCV_F_NOSCALE;
            else if (*p == 'c') f |= XCV_F_LXCRTC;
            else if (*p == 'x') f |= XCV_F_NOEXTRA;
            else if (*p == 'r') f |= XCV_F_SCL_RHD;
            else if (*p == 'e') f |= XCV_F_SCL_NEAREST;
            else if (*p == 'k') f |= XCV_F_SCL_KERNEL;
            else return XCV_OVR_AUTO;
        }
        *flags = f;
        return XCV_OVR_1080P;
    }
    return XCV_OVR_AUTO;
}

const char *xcv_ovr_name(int ovr) {
    switch (ovr) {
    case XCV_OVR_720P: return "720p";
    case XCV_OVR_1080P: return "1080p";
    default: return "auto";
    }
}

int xcv_ovr_next(int ovr) { return (ovr + 1) % XCV_OVR_COUNT; }

const char *xcv_mode_name(int mode) {
    switch (mode) {
    case XCV_HDMI_1080P: return "HDMI 1080p (1280x720 escalado)";
    case XCV_YUV_720P: return "componente 720p";
    case XCV_VGA_720P: return "VGA 1280x720";
    default: return "HDMI 720p";
    }
}

xcv_choice_t xcv_choose(int avpack, const xc_edid_info_t *e, int ovr, int have_1080p, int force_720p) {
    xcv_choice_t c = { XCV_HDMI_720P, "" };
    int av = xcv_av_kind(avpack);
    if (av == XCV_AV_COMPONENT) { c.mode = XCV_YUV_720P; c.why = "cabo componente"; return c; }
    if (av == XCV_AV_VGA) { c.mode = XCV_VGA_720P; c.why = "cabo VGA"; return c; }
    if (force_720p) { c.why = "LB+RB no boot: 720p forcado"; return c; }
    if (ovr == XCV_OVR_720P) { c.why = "manual 720p"; return c; }
    if (ovr == XCV_OVR_1080P) {
        if (have_1080p) { c.mode = XCV_HDMI_1080P; c.why = "manual 1080p"; }
        else c.why = "manual 1080p, mas sem tabela 1080p pra esta placa: 720p";
        return c;
    }
    if (!e || !e->valid) { c.why = "auto: sem EDID, 720p"; return c; }
    if (e->has_720p60) { c.why = "auto: TV aceita 720p60"; return c; }
    if (e->has_1080p60) {
        if (have_1080p) { c.mode = XCV_HDMI_1080P; c.why = "auto: EDID sem 720p60 e com 1080p60"; }
        else c.why = "auto: EDID pede 1080p60, mas sem tabela 1080p pra esta placa: 720p";
        return c;
    }
    c.why = "auto: EDID sem 720p60 nem 1080p60, 720p";
    return c;
}

/* CEA-861: 720p60 = 1280 + 110/40/220, 720 + 5/5/20 @ 74.25 MHz;
 *          1080p60 = 1920 + 88/44/148, 1080 + 4/5/36 @ 148.5 MHz. */
const xcv_timing_t XCV_CEA_720P60  = { 1280, 110, 40, 220, 720, 5, 5, 20, 74250 };
const xcv_timing_t XCV_CEA_1080P60 = { 1920, 88, 44, 148, 1080, 4, 5, 36, 148500 };

void xcv_crtc_from_timing(const xcv_timing_t *t, xcv_crtc_t *c) {
    c->active_w = t->h_active;
    c->active_h = t->v_active;
    c->total_w = t->h_active + t->h_front + t->h_sync + t->h_back;
    c->total_h = t->v_active + t->v_front + t->v_sync + t->v_back;
    /* o libxenon usa sync+back-1 (720p: 259) e sync+back (720p: 25) */
    c->hsync_offset = t->h_sync + t->h_back - 1;
    c->vsync_offset = t->v_sync + t->v_back;
}

unsigned xcv_refresh_x100(const xcv_timing_t *t) {
    unsigned long long tot = (unsigned long long)(t->h_active + t->h_front + t->h_sync + t->h_back) *
                             (unsigned long long)(t->v_active + t->v_front + t->v_sync + t->v_back);
    if (!tot) return 0;
    return (unsigned)(((unsigned long long)t->pclk_khz * 100000ull + tot / 2) / tot);
}

uint32_t xcv_scale_ratio(int src, int dst) {
    if (src <= 0 || dst <= 0) return 0x01000000u;
    return (uint32_t)((((unsigned long long)src << 24) + (unsigned)dst / 2) / (unsigned)dst);
}

/* Reserva (tabela sem bloco POST). Offsets confirmados pelo dump Corona
 * 1080p (fase B, back buffer 1280x720): razões em 0x65b4 e 0x65c4
 * (0x00aaaaa0 = 2/3 com os 5 bits de baixo zerados), TAP_CONTROL 0x504,
 * fases iniciais 0x65b8/0x65c8 (0x3d600/0x35600). Só roda se uma tabela
 * tiver have=1 e n_post=0. */
int xcv_scaler_regs(int sw, int sh, int dw, int dh, xcv_reg_t *o, int cap) {
    xcv_reg_t r[] = {
        { 0x6580, 0 },                                        /* D1MODE_VIEWPORT_START */
        { 0x6584, ((uint32_t)sw << 16) | (uint32_t)sh },     /* D1MODE_VIEWPORT_SIZE (fonte) */
        { 0x6590, (sw != dw || sh != dh) ? 1u : 0u },         /* D1SCL_SCALER_ENABLE */
        { 0x65b4, xcv_scale_ratio(sw, dw) & ~0x1Fu },         /* razão (horizontal) */
        { 0x65c4, xcv_scale_ratio(sh, dh) & ~0x1Fu },         /* razão (vertical) */
    };
    int n = (int)(sizeof(r) / sizeof(r[0]));
    if (n > cap) n = cap;
    memcpy(o, r, (size_t)n * sizeof(r[0]));
    return n;
}

const xcv_table_t *xcv_1080p_table(int board) {
    if (board < 0 || board >= XCV_BOARD_COUNT) return NULL;
    return &XCV_1080P_TABLES[board];
}

int xcv_ana_complete(const uint32_t ana[256], int corona, char *why, size_t n) {
    /* índices que o xenos_init_ana_new lê de mode->ana */
    static const uint8_t used[] = { 0x00, 0x03, 0x06, 0x07, 0x08, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
                                    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0xd0, 0xd1, 0xd6, 0xd8 };
    for (size_t i = 0; i < sizeof(used); i++)
        if (ana[used[i]] == 0xffffffffu) { if (why && n) snprintf(why, n, "ana[0x%02x] nao lido", used[i]); return 0; }
    for (int i = 0x26; i <= 0x57; i++)
        if (ana[i] == 0xffffffffu) { if (why && n) snprintf(why, n, "ana[0x%02x] nao lido", i); return 0; }
    if (corona && ana[0xd2] == 0xffffffffu) { if (why && n) snprintf(why, n, "ana[0xd2] nao lido"); return 0; }
    if (why && n) why[0] = 0;
    return 1;
}

int xcv_1080p_usable(int board) {
    const xcv_table_t *t = xcv_1080p_table(board);
    if (!t || !t->have) return 0;
    if (!xcv_ana_complete(t->ana, board == XCV_BOARD_CORONA, NULL, 0)) return 0;
    if (t->crtc.active_w != 1920 || t->crtc.active_h != 1080) return 0;
    if (t->crtc.total_w <= t->crtc.active_w || t->crtc.total_h <= t->crtc.active_h) return 0;
    return 1;
}
