/*
 * orbita — modo de vídeo no Xbox: EDID, escolha e HDMI 1080p.
 * Lógica pura em xc_video_mode.c / xc_edid.c (testadas no host); aqui só
 * libxenon. Ver xc_video.h e VIDEO-1080P.md.
 *
 * 1080p = o mesmo caminho do libxenon (xenos_set_mode: ANA pelo SMC +
 * CRTC + D1GRPH), com uma struct mode_s nossa: CRTC 1920x1080 (2200x1125),
 * superfície/viewport 1280x720, e depois o bloco D1MODE/D1SCL (scaler) da
 * tabela. Sem fork do libxenon: xenos_set_mode/xenos_write32 já são
 * públicos (só não estão no header).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <xetypes.h>
#include <xenon_smc/xenon_smc.h>
#include <input/input.h>
#include <usb/usbmain.h>

#include "xc_video.h"
#include "xc_video_mode.h"
#include "../polite_wait.h"
#include "../login/xc_port.h"   /* xc_log, xc_store_* */

/* ---- libxenon (drivers/xenos/xenos.c), sem protótipo nos headers ---- */
/* Cópia EXATA da struct mode_s de xenos_videomodesdata.h (o header define
 * as tabelas, não dá pra incluir). Conferir se o libxenon mudar. */
struct xcv_mode_s {
    char     *name;
    uint32_t *ana;
    int height, total_height, active_height;
    int width, total_width, real_active_width;
    int hsync_offset, vsync_offset;
    int composite_sync, is_progressive, rgb, hdmi, overscan;
};
extern int xenos_is_corona;
void     xenos_set_mode(struct xcv_mode_s *mode);
void     xenos_write32(int reg, uint32_t val);
uint32_t xenos_read32(int reg);
struct edid;
struct edid *xenos_get_edid();

#define VIDEO_FILE "VIDEO.TXT"
#define R_D1GRPH_PRIMARY_SURFACE_ADDRESS 0x6110
#define R_D1GRPH_PITCH                   0x6120
#define R_D1MODE_DESKTOP_HEIGHT          0x652c
#define R_D1SCL_UPDATE                   0x65cc
#define R_D1CRTC_UPDATE_LOCK             0x60e8

static int  g_av = -1, g_ovr = XCV_OVR_AUTO, g_mode = XCV_HDMI_720P, g_have = 0, g_board = 0;
static unsigned g_flags = 0;
static int  g_booted = 0, g_applied = 0;
static int  g_forced = 0;   /* v0.21.33: LB+RB no boot */
static char g_l_av[160], g_l_edid[320], g_l_mode[200], g_l_scl[200];
/* v0.21.21: estado dos registradores logo depois da troca (só UART no boot;
 * o menu_main repete pro upload, junto com uma leitura nova) */
#define XCV_REGLINES 10
static char g_reg[XCV_REGLINES][192];
static int  g_nreg = 0;
/* v0.21.21: a troca nas Configurações não grava do main (travava): fica
 * pendente e quem é dono do pendrive grava (xcv_save_pending). ovr+1, 0 = nada */
static volatile int g_save_pend = 0;

static struct xcv_mode_s g_m;
static uint32_t g_ana[256];

static int pads_lb_rb(void) {
    for (int k = 0; k < 5; k++) {   /* ~50 ms: o warm-up do main já sincronizou */
        usb_do_poll();
        for (int i = 0; i < 4; i++) {
            struct controller_data_s d;
            memset(&d, 0, sizeof(d));
            get_controller_data(&d, i);
            if (d.lb && d.rb) return 1;
        }
        xc_polite_udelay(10000);
    }
    return 0;
}

static int read_edid(xc_edid_info_t *ei) {
    uint8_t *e = (uint8_t *)xenos_get_edid();   /* NULL se não leu / inválido */
    if (!e) return -1;
    int ext = e[126] > XC_EDID_MAX_EXT ? XC_EDID_MAX_EXT : e[126];
    int r = xc_edid_parse(e, (size_t)XC_EDID_BLOCK * (size_t)(1 + ext), ei);
    free(e);
    return r;
}

/* ---- log dos registradores ---- */
static void reg_line(char *o, size_t n, const char *tag, const uint16_t *r, int cnt) {
    size_t k = (size_t)snprintf(o, n, "[video] %s", tag);
    for (int i = 0; i < cnt && k + 15 < n; i++)
        k += (size_t)snprintf(o + k, n - k, " %04x=%08x", r[i], (unsigned)xenos_read32(r[i]));
}

static void add_reg_line(const char *tag, const uint16_t *r, int cnt) {
    if (g_nreg < XCV_REGLINES) reg_line(g_reg[g_nreg++], sizeof(g_reg[0]), tag, r, cnt);
}

static const uint16_t R_CRTC[] = { 0x6000, 0x6004, 0x6008, 0x600c, 0x6010, 0x6014, 0x6018, 0x6024, 0x6028,
                                   0x6038, 0x603c, 0x6040 };
static const uint16_t R_CRTC2[] = { 0x6054, 0x6058, 0x6060, 0x6064, 0x6068, 0x60e8, 0x7900, 0x7910, 0x7938,
                                    0x793c, 0x6cbc };
static const uint16_t R_GRPH[] = { 0x6100, 0x6104, 0x6110, 0x6120, 0x6124, 0x6128, 0x612c, 0x6130, 0x6134,
                                   0x6138, 0x6148 };
static const uint16_t R_MODE[] = { 0x6520, 0x6524, 0x6528, 0x652c, 0x6550, 0x6578, 0x6580, 0x6584, 0x6588,
                                   0x658c, 0x65e0, 0x65e8 };
static const uint16_t R_SCL[] = { 0x6590, 0x6594, 0x65a0, 0x65a4, 0x65b0, 0x65b4, 0x65b8, 0x65c0, 0x65c4,
                                  0x65c8, 0x65d0, 0x65dc };
static const uint16_t R_CSC[] = { 0x6380, 0x6384, 0x6388, 0x638c, 0x6390, 0x6394, 0x6398, 0x639c, 0x63a0,
                                  0x63a4, 0x63a8, 0x63ac };
static const uint16_t R_LUT[] = { 0x63b0, 0x63b4, 0x6480, 0x6484, 0x6488, 0x649c, 0x64a0, 0x64c0, 0x64c4,
                                  0x64c8, 0x64cc, 0x64d0 };
#define NR(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* entradas da LUT pela porta (0x6488 índice, 0x6494 cor 10:10:10); só no boot */
static void lut_line(void) {
    if (g_nreg >= XCV_REGLINES) return;
    uint32_t sel = xenos_read32(0x6480), mode = xenos_read32(0x6484);
    static const int idx[] = { 0, 1, 64, 128, 192, 255 };
    char *o = g_reg[g_nreg];
    size_t k = (size_t)snprintf(o, sizeof(g_reg[0]), "[video] LUT d4=%08x d8=%08x", (unsigned)xenos_read32(0x64d4),
                                (unsigned)xenos_read32(0x64d8));
    xenos_write32(0x6480, 0);
    xenos_write32(0x6484, 0);
    for (int i = 0; i < NR(idx) && k + 16 < sizeof(g_reg[0]); i++) {
        xenos_write32(0x6488, (uint32_t)idx[i]);
        k += (size_t)snprintf(o + k, sizeof(g_reg[0]) - k, " [%d]=%08x", idx[i], (unsigned)xenos_read32(0x6494));
    }
    xenos_write32(0x6488, 0);
    xenos_write32(0x6484, mode);
    xenos_write32(0x6480, sel);
    g_nreg++;
}

static void ana_line(void) {
    static const uint8_t a[] = { 0x00, 0x12, 0x13, 0x14, 0x15, 0xcd, 0xd0, 0xd1, 0xd2, 0xd6, 0xd8, 0xdf };
    if (g_nreg >= XCV_REGLINES) return;
    char *o = g_reg[g_nreg++];
    size_t k = (size_t)snprintf(o, sizeof(g_reg[0]), "[video] ANA");
    for (int i = 0; i < NR(a) && k + 13 < sizeof(g_reg[0]); i++) {
        uint32_t v = 0xffffffffu;
        xenon_smc_ana_read(a[i], &v);
        k += (size_t)snprintf(o + k, sizeof(g_reg[0]) - k, " %02x=%08x", a[i], (unsigned)v);
    }
}

static void snapshot_boot(void) {
    g_nreg = 0;
    add_reg_line("CRTC", R_CRTC, NR(R_CRTC));
    add_reg_line("CRTC2", R_CRTC2, NR(R_CRTC2));
    add_reg_line("GRPH", R_GRPH, NR(R_GRPH));
    add_reg_line("MODE", R_MODE, NR(R_MODE));
    add_reg_line("SCL", R_SCL, NR(R_SCL));
    add_reg_line("CSC", R_CSC, NR(R_CSC));
    add_reg_line("LUTREG", R_LUT, NR(R_LUT));
    lut_line();
    ana_line();
}

static void write_list(const xcv_reg_t *p, int n, int skip_extra) {
    for (int i = 0; i < n; i++) {
        if (skip_extra && xcv_post_is_extra(p[i].reg)) continue;
        xenos_write32(p[i].reg, p[i].val);
    }
}

static uint32_t post_val(const xcv_reg_t *p, int n, uint16_t reg, uint32_t def) {
    for (int i = 0; i < n; i++)
        if (p[i].reg == reg) return p[i].val;
    return def;
}

static void apply_1080p(const xcv_table_t *t, unsigned flags) {
    memcpy(g_ana, t->ana, sizeof(g_ana));
    memset(&g_m, 0, sizeof(g_m));
    g_m.name = "HDMI 1080p";
    g_m.ana = g_ana;
    g_m.width = 1280;                     /* superfície / viewport: o fb do app */
    g_m.height = 720;
    g_m.active_height = t->crtc.active_h; /* CRTC: 1920x1080 */
    g_m.real_active_width = t->crtc.active_w;
    g_m.total_width = t->crtc.total_w;
    g_m.total_height = t->crtc.total_h;
    g_m.hsync_offset = t->crtc.hsync_offset;
    g_m.vsync_offset = t->crtc.vsync_offset;
    g_m.composite_sync = 0;
    g_m.is_progressive = 1;
    g_m.rgb = 1;                          /* igual ao "HDMI 720p" do libxenon */
    g_m.hdmi = 1;
    g_m.overscan = 1;

    uint32_t fb = xenos_read32(R_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    uint32_t pitch = xenos_read32(R_D1GRPH_PITCH);
    xenos_set_mode(&g_m);
    xenon_smc_ana_write(0xdf, 0);                    /* como o fim do xenos_init */
    xenos_write32(R_D1MODE_DESKTOP_HEIGHT, 0x300);   /* idem (a tabela troca) */

    xcv_reg_t tmp[8];
    const xcv_reg_t *p = t->post;
    int n = t->n_post;
    if (!p || n <= 0) {   /* tabela sem o bloco do scaler: conjunto de reserva */
        n = xcv_scaler_regs(1280, 720, t->crtc.active_w, t->crtc.active_h, tmp, 8);
        p = tmp;
    }
    xenos_write32(R_D1SCL_UPDATE, 1);
    if (flags & XCV_F_NOSCALE) {
        /* diagnóstico: scaler desligado (o do libxenon), só a altura do desktop */
        for (int i = 0; i < n; i++)
            if (p[i].reg == R_D1MODE_DESKTOP_HEIGHT) xenos_write32(p[i].reg, p[i].val);
    } else {
        write_list(p, n, (flags & XCV_F_NOEXTRA) != 0);
        /* v0.21.22: o POST traz o filtro de 6/5 taps do kernel, que lê a RAM
         * de coeficientes (vazia sem o kernel: imagem só com contornos).
         * Padrão: 2 taps com coeficientes bilineares fixos do hardware. */
        xcv_scl_t f;
        xcv_scl_filter(xcv_scl_kind(flags), post_val(p, n, 0x65b4, xcv_scale_ratio(1280, 1920) & ~0x1Fu),
                       post_val(p, n, 0x65c4, xcv_scale_ratio(720, 1080) & ~0x1Fu), post_val(p, n, 0x65c0, 0x01000000u), &f);
        xenos_write32(0x6594, f.tap);
        xenos_write32(0x65b0, f.hctl);
        xenos_write32(0x65b8, f.hinit);
        xenos_write32(0x65c0, f.vctl);
        xenos_write32(0x65c8, f.vinit);
        snprintf(g_l_scl, sizeof(g_l_scl), "[video] scaler: %s, taps %dx%d, 6594=%08x 65b0=%08x 65b8=%08x 65c0=%08x 65c8=%08x",
                 xcv_scl_name(xcv_scl_kind(flags)), f.taps_h, f.taps_v, (unsigned)f.tap, (unsigned)f.hctl,
                 (unsigned)f.hinit, (unsigned)f.vctl, (unsigned)f.vinit);
    }
    xenos_write32(R_D1SCL_UPDATE, 0);

    /* v0.21.21: MVP/TRIG do CRTC como o kernel deixou em 1080p (o libxenon
     * escreve constantes) */
    if (!(flags & XCV_F_LXCRTC) && t->crtc_post && t->n_crtc_post > 0) {
        xenos_write32(R_D1CRTC_UPDATE_LOCK, 1);
        write_list(t->crtc_post, t->n_crtc_post, 0);
        xenos_write32(R_D1CRTC_UPDATE_LOCK, 0);
    }

    /* o set_mode põe FB_BASE; o app (console/Xe) já usa o que estava */
    if (xenos_read32(R_D1GRPH_PRIMARY_SURFACE_ADDRESS) != fb) xenos_write32(R_D1GRPH_PRIMARY_SURFACE_ADDRESS, fb);
    if (xenos_read32(R_D1GRPH_PITCH) != pitch) xenos_write32(R_D1GRPH_PITCH, pitch);
    g_applied = 1;
}

void xcv_boot(int avpack) {
    if (g_booted) return;
    g_booted = 1;
    g_av = avpack;
    g_board = xenos_is_corona ? XCV_BOARD_CORONA : XCV_BOARD_PRE_CORONA;
    g_have = xcv_1080p_usable(g_board);

    char b[24];
    int n = xc_store_load(VIDEO_FILE, b, sizeof(b) - 1);
    if (n > 0) { b[n] = 0; g_ovr = xcv_ovr_parse_ex(b, (size_t)n, &g_flags); }
    int force = pads_lb_rb();
    g_forced = force;

    xc_edid_info_t ei;
    int got = -1;
    memset(&ei, 0, sizeof(ei));
    if (xcv_av_kind(avpack) == XCV_AV_HDMI) got = read_edid(&ei);

    const xcv_table_t *t = xcv_1080p_table(g_board);
    snprintf(g_l_av, sizeof(g_l_av), "[video] AV pack 0x%02x (%s), placa %s, tabela 1080p: %s",
             avpack & 0xff, xcv_av_name(avpack), g_board == XCV_BOARD_CORONA ? "Corona/Winchester" : "pre-Corona",
             g_have ? t->source : "nenhuma");
    if (xcv_av_kind(avpack) != XCV_AV_HDMI) {
        snprintf(g_l_edid, sizeof(g_l_edid), "[video] EDID: nao lido (cabo nao e HDMI)");
    } else {
        char s[280];
        xc_edid_summary(got == 0 ? &ei : NULL, s, sizeof(s));
        snprintf(g_l_edid, sizeof(g_l_edid), "[video] EDID: %s", s);
    }
    xcv_choice_t c = xcv_choose(avpack, got == 0 ? &ei : NULL, g_ovr, g_have, force);
    g_mode = c.mode;
    if (g_mode == XCV_HDMI_1080P) apply_1080p(t, g_flags);
    char fl[8];
    xcv_flags_str(g_flags, fl, sizeof(fl));
    snprintf(g_l_mode, sizeof(g_l_mode), "[video] VIDEO.TXT=%s%s%s%s -> %s (%s)", n > 0 ? xcv_ovr_name(g_ovr) : "(sem)",
             fl[0] ? "+" : "", fl, force ? " +LB/RB" : "", xcv_mode_name(g_mode), c.why);
    snapshot_boot();     /* 720p e 1080p: dá pra comparar os dois logs */
    xcv_log_summary();   /* aqui só UART; o menu_main repete pro upload */
}

void xcv_log_summary(void) {
    if (!g_booted) return;
    xc_log("%s", g_l_av);
    xc_log("%s", g_l_edid);
    xc_log("%s", g_l_mode);
    if (g_l_scl[0]) xc_log("%s", g_l_scl);
    for (int i = 0; i < g_nreg; i++) xc_log("%s", g_reg[i]);
    /* leitura nova (depois do fb_direct_init/gpu_present_init): pega quem
     * mexeu na superfície/viewport/scaler depois da troca */
    char o[192];
    reg_line(o, sizeof(o), "agora GRPH", R_GRPH, NR(R_GRPH));
    xc_log("%s", o);
    reg_line(o, sizeof(o), "agora SCL", R_SCL, NR(R_SCL));
    xc_log("%s", o);
    reg_line(o, sizeof(o), "agora MODE", R_MODE, NR(R_MODE));
    xc_log("%s", o);
}

void xcv_settings_state(xcv_state_t *o) {
    o->av_kind = g_av >= 0 ? xcv_av_kind(g_av) : -1;
    o->mode = g_mode;
    o->ovr = g_ovr;
    o->have_1080p = g_have;
    o->forced_720p = g_forced;
}

int xcv_boot_forced(void) { return g_forced; }

/* Só memória: nada de pendrive aqui (a v0.21.19 travava gravando do main
 * com o worker das capas/hw1 no USB). Grava em xcv_save_pending. */
int xcv_settings_set(int ovr) {
    if (ovr != XCV_OVR_720P && ovr != XCV_OVR_1080P) return 0;
    if (ovr == g_ovr) return 0;   /* mesma escolha: não mexe (nem nas letras de diagnóstico do VIDEO.TXT) */
    g_ovr = ovr;
    g_flags = 0;
    __sync_lock_test_and_set(&g_save_pend, g_ovr + 1);
    xc_log("[video] VIDEO.TXT -> %s (Configurações; pendente, vale no proximo boot)", xcv_ovr_name(g_ovr));
    return 1;
}

void xcv_save_pending(void) {
    int p = __sync_lock_test_and_set(&g_save_pend, 0);
    if (p <= 0) return;
    char b[16];
    int len = snprintf(b, sizeof(b), "%s\n", xcv_ovr_name(p - 1));
    int r = xc_store_save(VIDEO_FILE, b, (size_t)len);
    xc_log("[video] XC360/" VIDEO_FILE " = %s: %s", xcv_ovr_name(p - 1), r == 0 ? "gravado" : "falhou");
}
