/*
 * orbita — hosttest do modo de vídeo (source/video): EDID reais,
 * tabela de escolha do modo, timing CEA e conta do scaler.
 *   video_test <pasta com os .hex>
 * Compilado 2x pelo run.sh: com a tabela 1080p do repo (placeholder ou
 * dump) e com uma tabela gerada de um dump SINTÉTICO (-DXCV_EXPECT_DUMP=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video/xc_edid.h"
#include "video/xc_video_mode.h"

#ifndef XCV_EXPECT_DUMP
#define XCV_EXPECT_DUMP 0
#endif

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) g_ok++; else { g_fail++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static size_t load_hex(const char *dir, const char *name, uint8_t *b, size_t cap) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "r");
    if (!f) { printf("sem fixture %s\n", p); g_fail++; return 0; }
    char line[512];
    size_t n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *s = line;
        unsigned x;
        int k;
        while (n < cap && sscanf(s, " %2x%n", &x, &k) == 1) { b[n++] = (uint8_t)x; s += k; }
    }
    fclose(f);
    return n;
}

typedef struct {
    const char *file;
    int hdmi, digital, has720, has1080, has1080i;
    const char *name;
    int pref_w, pref_h;
} edid_case_t;

static const edid_case_t EDIDS[] = {
    /* TV com 720p e 1080p: auto fica em 720p */
    { "sony_tv_720p_1080p.hex",          1, 1, 1, 1, 1, "SONY TV",    1360, 768 },
    { "acer_kg273_hdmi.hex",             1, 1, 1, 1, 1, "KG273",      1920, 1080 },
    { "hjw_capture_hdmi.hex",            1, 1, 1, 1, 1, "HD TO USB",  1920, 1080 },
    /* HDMI só 1080p (o caso do testador): auto vai pra 1080p */
    { "samsung_t24d390_1080p_only.hex",  1, 1, 0, 1, 0, "T24D390",    1920, 1080 },
    { "sony_avamp_no720.hex",            1, 1, 0, 1, 1, "SONY AVAMP", 1920, 1080 },
    /* DVI (sem extensão CEA) */
    { "samsung_syncmaster_dvi_1080p.hex", 0, 1, 0, 1, 0, "SyncMaster", 1920, 1080 },
    { "samsung_s24e450_dvi_720p.hex",    0, 1, 1, 1, 0, "S24E450",    1920, 1080 },
};

static void test_edid(const char *dir) {
    uint8_t b[1024];
    for (size_t i = 0; i < sizeof(EDIDS) / sizeof(EDIDS[0]); i++) {
        const edid_case_t *c = &EDIDS[i];
        size_t n = load_hex(dir, c->file, b, sizeof(b));
        xc_edid_info_t e;
        int r = xc_edid_parse(b, n, &e);
        CHECK(r == 0 && e.valid, "%s: parse r=%d", c->file, r);
        CHECK(e.hdmi == c->hdmi, "%s: hdmi=%d", c->file, e.hdmi);
        CHECK(e.digital == c->digital, "%s: digital=%d", c->file, e.digital);
        CHECK(e.has_720p60 == c->has720, "%s: 720p60=%d", c->file, e.has_720p60);
        CHECK(e.has_1080p60 == c->has1080, "%s: 1080p60=%d", c->file, e.has_1080p60);
        CHECK(e.has_1080i60 == c->has1080i, "%s: 1080i60=%d", c->file, e.has_1080i60);
        CHECK(!strcmp(e.name, c->name), "%s: nome '%s'", c->file, e.name);
        CHECK(e.n_dtd > 0 && e.dtd[0].w == c->pref_w && e.dtd[0].h == c->pref_h, "%s: pref %ux%u", c->file,
              e.n_dtd ? e.dtd[0].w : 0, e.n_dtd ? e.dtd[0].h : 0);
        CHECK(c->hdmi ? e.n_cea == 1 : e.n_cea == 0, "%s: n_cea=%d", c->file, e.n_cea);
        char s[400];
        xc_edid_summary(&e, s, sizeof(s));
        CHECK(strstr(s, c->name) != NULL, "%s: resumo '%s'", c->file, s);
        printf("  %-34s %s\n", c->file, s);
        /* resumo num buffer pequeno: trunca sem estourar */
        char t[16];
        memset(t, 'Z', sizeof(t));
        xc_edid_summary(&e, t, 10);
        CHECK(strlen(t) == 9 && t[10] == 'Z', "%s: resumo truncado", c->file);
    }
    /* T24D390: VIC 16 é nativo */
    size_t n = load_hex(dir, "samsung_t24d390_1080p_only.hex", b, sizeof(b));
    xc_edid_info_t e;
    xc_edid_parse(b, n, &e);
    CHECK(e.n_vic == 2 && e.vic[0] == 16 && e.vic_native[0] == 1 && e.vic[1] == 31, "T24D390 VICs");

    /* defeitos: checksum ruim, cabeçalho, curto, extensão truncada/ruim */
    n = load_hex(dir, "acer_kg273_hdmi.hex", b, sizeof(b));
    uint8_t c[1024];
    memcpy(c, b, n);
    c[20] ^= 0x01;
    CHECK(xc_edid_parse(c, n, &e) == -1 && !e.valid, "checksum ruim no bloco 0 tem que falhar");
    memcpy(c, b, n);
    c[1] = 0x00;
    CHECK(xc_edid_parse(c, n, &e) == -1, "cabeçalho ruim tem que falhar");
    CHECK(xc_edid_parse(b, 100, &e) == -1, "menos de 128 bytes tem que falhar");
    CHECK(xc_edid_parse(NULL, 256, &e) == -1, "NULL");
    CHECK(xc_edid_parse(b, 128, &e) == 0 && e.n_ext == 1 && e.n_cea == 0 && !e.hdmi && e.n_vic == 0,
          "só o bloco 0: extensão ignorada");
    CHECK(e.has_720p60 && e.has_1080p60, "Acer só bloco 0 ainda tem 720p/1080p (DTD+STD)");
    memcpy(c, b, n);
    c[128 + 10] ^= 0xFF;   /* estraga a CEA */
    CHECK(xc_edid_parse(c, n, &e) == 0 && e.n_cea == 0 && e.cea_bad == 1 && !e.hdmi, "CEA com checksum ruim ignorada");
    /* T24D390 sem a CEA: só DTD 1080p -> continua sem 720p */
    n = load_hex(dir, "samsung_t24d390_1080p_only.hex", b, sizeof(b));
    CHECK(xc_edid_parse(b, 128, &e) == 0 && !e.has_720p60 && e.has_1080p60, "T24D390 bloco 0");
}

static xc_edid_info_t edid_of(const char *dir, const char *f) {
    uint8_t b[1024];
    xc_edid_info_t e;
    size_t n = load_hex(dir, f, b, sizeof(b));
    xc_edid_parse(b, n, &e);
    return e;
}

static void test_choose(const char *dir) {
    xc_edid_info_t tv = edid_of(dir, "sony_tv_720p_1080p.hex");
    xc_edid_info_t only = edid_of(dir, "samsung_t24d390_1080p_only.hex");
    xc_edid_info_t dvi = edid_of(dir, "samsung_syncmaster_dvi_1080p.hex");
    xc_edid_info_t bad;
    memset(&bad, 0, sizeof(bad));
    struct { int av; const xc_edid_info_t *e; int ovr, have, force, want; } T[] = {
        /* HDMI, auto */
        { 0x1F, &tv,   XCV_OVR_AUTO,  1, 0, XCV_HDMI_720P },
        { 0x1F, &only, XCV_OVR_AUTO,  1, 0, XCV_HDMI_1080P },
        { 0x1F, &only, XCV_OVR_AUTO,  0, 0, XCV_HDMI_720P },   /* sem tabela pra placa */
        { 0x1F, &dvi,  XCV_OVR_AUTO,  1, 0, XCV_HDMI_1080P },  /* DVI só 1080p */
        { 0x1E, NULL,  XCV_OVR_AUTO,  1, 0, XCV_HDMI_720P },   /* sem EDID */
        { 0x13, &bad,  XCV_OVR_AUTO,  1, 0, XCV_HDMI_720P },   /* EDID inválido */
        /* HDMI, manual */
        { 0x1F, &only, XCV_OVR_720P,  1, 0, XCV_HDMI_720P },
        { 0x1F, &tv,   XCV_OVR_1080P, 1, 0, XCV_HDMI_1080P },
        { 0x1F, NULL,  XCV_OVR_1080P, 1, 0, XCV_HDMI_1080P },
        { 0x1F, &tv,   XCV_OVR_1080P, 0, 0, XCV_HDMI_720P },
        /* LB+RB no boot vence tudo no HDMI */
        { 0x1F, &only, XCV_OVR_1080P, 1, 1, XCV_HDMI_720P },
        { 0x1F, &only, XCV_OVR_AUTO,  1, 1, XCV_HDMI_720P },
        /* outros cabos: ignora EDID e escolha manual */
        { 0x0C, &only, XCV_OVR_1080P, 1, 0, XCV_YUV_720P },
        { 0x0F, NULL,  XCV_OVR_AUTO,  1, 0, XCV_YUV_720P },
        { 0x4F, NULL,  XCV_OVR_AUTO,  1, 0, XCV_YUV_720P },
        { 0x5B, &only, XCV_OVR_1080P, 1, 0, XCV_VGA_720P },
        { 0x59, NULL,  XCV_OVR_AUTO,  1, 0, XCV_VGA_720P },
        { 0x1B, NULL,  XCV_OVR_AUTO,  1, 0, XCV_VGA_720P },
        /* AV comum / desconhecido: como o main.c antes (HDMI 720p) */
        { 0x57, NULL,  XCV_OVR_AUTO,  1, 0, XCV_HDMI_720P },
        { 0x00, &only, XCV_OVR_AUTO,  1, 0, XCV_HDMI_1080P },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        xcv_choice_t c = xcv_choose(T[i].av, T[i].e, T[i].ovr, T[i].have, T[i].force);
        CHECK(c.mode == T[i].want && c.why && c.why[0], "caso %zu: av=0x%02x ovr=%d have=%d force=%d -> %s (%s)", i,
              T[i].av, T[i].ovr, T[i].have, T[i].force, xcv_mode_name(c.mode), c.why);
    }
    CHECK(xcv_av_kind(0x1F) == XCV_AV_HDMI && xcv_av_kind(0x0C) == XCV_AV_COMPONENT && xcv_av_kind(0x5B) == XCV_AV_VGA, "av_kind");
    CHECK(!strcmp(xcv_av_name(0x1F), "HDMI") && !strcmp(xcv_av_name(0x47), "AV comum/SCART"), "av_name");

    struct { const char *s; int want; } P[] = {
        { "auto\n", XCV_OVR_AUTO }, { "720p\n", XCV_OVR_720P }, { "1080p", XCV_OVR_1080P },
        { "  1080P \r\n", XCV_OVR_1080P }, { "720", XCV_OVR_720P }, { "1080", XCV_OVR_1080P },
        { "", XCV_OVR_AUTO }, { "4k", XCV_OVR_AUTO }, { "1080pz", XCV_OVR_AUTO }, { "\xef\xbb\xbf" "720p", XCV_OVR_AUTO },
    };
    for (size_t i = 0; i < sizeof(P) / sizeof(P[0]); i++)
        CHECK(xcv_ovr_parse(P[i].s, strlen(P[i].s)) == P[i].want, "ovr_parse '%s'", P[i].s);
    CHECK(xcv_ovr_parse(NULL, 4) == XCV_OVR_AUTO, "ovr_parse NULL");
    /* v0.21.21: letras de diagnóstico do 1080p */
    struct { const char *s; int want; unsigned fl; const char *str; } F[] = {
        { "1080\n", XCV_OVR_1080P, 0, "" }, { "1080p", XCV_OVR_1080P, 0, "" },
        { "1080n", XCV_OVR_1080P, XCV_F_NOSCALE, "n" }, { "1080pc\r\n", XCV_OVR_1080P, XCV_F_LXCRTC, "c" },
        { "1080CX", XCV_OVR_1080P, XCV_F_LXCRTC | XCV_F_NOEXTRA, "cx" },
        { "1080nxc", XCV_OVR_1080P, XCV_F_NOSCALE | XCV_F_LXCRTC | XCV_F_NOEXTRA, "ncx" },
        { "1080r", XCV_OVR_1080P, XCV_F_SCL_RHD, "r" }, { "1080e", XCV_OVR_1080P, XCV_F_SCL_NEAREST, "e" },
        { "1080k", XCV_OVR_1080P, XCV_F_SCL_KERNEL, "k" },
        { "1080q", XCV_OVR_AUTO, 0, "" }, { "720n", XCV_OVR_AUTO, 0, "" }, { "720p", XCV_OVR_720P, 0, "" },
    };
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        unsigned fl = 99;
        char fs[8];
        int o = xcv_ovr_parse_ex(F[i].s, strlen(F[i].s), &fl);
        xcv_flags_str(fl, fs, sizeof(fs));
        CHECK(o == F[i].want && fl == F[i].fl && !strcmp(fs, F[i].str), "parse_ex '%s' -> %d/%u '%s'", F[i].s, o, fl, fs);
    }
    CHECK(xcv_post_is_extra(0x6524) && xcv_post_is_extra(0x65e0) && !xcv_post_is_extra(0x6590), "extras");
    CHECK(xcv_ovr_parse("1080p", 3) == XCV_OVR_AUTO, "ovr_parse respeita n ('108')");
    for (int o = 0; o < XCV_OVR_COUNT; o++)
        CHECK(xcv_ovr_parse(xcv_ovr_name(o), strlen(xcv_ovr_name(o))) == o, "ida e volta %d", o);
    CHECK(xcv_ovr_next(XCV_OVR_AUTO) == XCV_OVR_720P && xcv_ovr_next(XCV_OVR_720P) == XCV_OVR_1080P &&
          xcv_ovr_next(XCV_OVR_1080P) == XCV_OVR_AUTO, "ciclo");
}

static void test_timing(void) {
    xcv_crtc_t c;
    /* 720p: tem que bater com o "HDMI 720p" do libxenon */
    xcv_crtc_from_timing(&XCV_CEA_720P60, &c);
    CHECK(c.total_w == 1650 && c.total_h == 750 && c.active_w == 1280 && c.active_h == 720 &&
          c.hsync_offset == 259 && c.vsync_offset == 25, "720p crtc %d %d %d %d", c.total_w, c.total_h, c.hsync_offset, c.vsync_offset);
    CHECK(xcv_refresh_x100(&XCV_CEA_720P60) == 6000, "720p 60 Hz");
    xcv_crtc_from_timing(&XCV_CEA_1080P60, &c);
    CHECK(c.total_w == 2200 && c.total_h == 1125 && c.active_w == 1920 && c.active_h == 1080 &&
          c.hsync_offset == 191 && c.vsync_offset == 41, "1080p crtc %d %d %d %d", c.total_w, c.total_h, c.hsync_offset, c.vsync_offset);
    CHECK(xcv_refresh_x100(&XCV_CEA_1080P60) == 6000, "1080p 60 Hz");
    /* 148.5 MHz / (2200*1125) = 60 exatos; 74.25 / (1650*750) idem */
    CHECK(148500000ull == 2200ull * 1125 * 60 && 74250000ull == 1650ull * 750 * 60, "pixel clock CEA");
    /* registradores que o xenos_set_mode_f1 escreveria (libxenon) */
    uint32_t hb = ((uint32_t)c.hsync_offset << 16) | (uint32_t)(c.active_w + c.hsync_offset);
    uint32_t vb = ((uint32_t)c.vsync_offset << 16) | (uint32_t)(c.active_h + c.vsync_offset);
    CHECK(c.total_w - 1 == 0x897 && c.total_h - 1 == 0x464 && hb == 0x00BF083F && vb == 0x00290461,
          "f1 1080p: H_TOTAL 0x%x V 0x%x HB 0x%08x VB 0x%08x", c.total_w - 1, c.total_h - 1, hb, vb);
}

static void test_scaler(void) {
    CHECK(xcv_scale_ratio(1280, 1920) == 0x00AAAAABu, "1280->1920 0x%08x", xcv_scale_ratio(1280, 1920));
    CHECK(xcv_scale_ratio(720, 1080) == 0x00AAAAABu, "720->1080");
    CHECK(xcv_scale_ratio(1280, 1280) == 0x01000000u, "1:1");
    CHECK(xcv_scale_ratio(1920, 1280) == 0x01800000u, "1.5 (reduz)");
    CHECK(xcv_scale_ratio(0, 1080) == 0x01000000u && xcv_scale_ratio(720, 0) == 0x01000000u, "zero = 1:1");
    xcv_reg_t r[8];
    int n = xcv_scaler_regs(1280, 720, 1920, 1080, r, 8);
    CHECK(n == 5, "n=%d", n);
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (r[i].reg == 0x6584) { CHECK(r[i].val == ((1280u << 16) | 720u), "viewport 0x%08x", r[i].val); seen |= 1; }
        if (r[i].reg == 0x6590) { CHECK(r[i].val == 1, "scaler ligado"); seen |= 2; }
        if (r[i].reg == 0x65b4 || r[i].reg == 0x65c4) { CHECK(r[i].val == 0x00AAAAA0u, "razão 0x%08x", r[i].val); seen |= 4; }
        CHECK(r[i].reg != 0x65cc && r[i].reg != 0x6110 && r[i].reg != 0x6120, "não mexe em UPDATE/superfície");
    }
    CHECK(seen == 7, "viewport+enable+razão");
    n = xcv_scaler_regs(1280, 720, 1280, 720, r, 8);
    CHECK(r[2].reg == 0x6590 && r[2].val == 0, "1:1 = scaler desligado");
    CHECK(xcv_scaler_regs(1280, 720, 1920, 1080, r, 2) == 2, "respeita cap");
}

static void test_table(void) {
    uint32_t ana[256];
    char why[64];
    memset(ana, 0, sizeof(ana));
    CHECK(xcv_ana_complete(ana, 1, why, sizeof(why)) && why[0] == 0, "tabela zerada = completa");
    ana[0xd2] = 0xffffffffu;
    CHECK(xcv_ana_complete(ana, 0, NULL, 0), "0xd2 só importa na Corona");
    CHECK(!xcv_ana_complete(ana, 1, why, sizeof(why)) && strstr(why, "0xd2"), "Corona sem 0xd2: %s", why);
    ana[0xd2] = 0;
    ana[0x30] = 0xffffffffu;
    CHECK(!xcv_ana_complete(ana, 0, why, sizeof(why)) && strstr(why, "0x30"), "níveis: %s", why);
    ana[0x30] = 0;
    ana[0x80] = 0xffffffffu;   /* índice que o libxenon não usa */
    CHECK(xcv_ana_complete(ana, 1, NULL, 0), "0x80 não importa");
    CHECK(xcv_1080p_table(-1) == NULL && xcv_1080p_table(XCV_BOARD_COUNT) == NULL, "board inválido");
#if XCV_EXPECT_DUMP
    /* tabela gerada pelo dump2mode.py de um dump SINTÉTICO Corona */
    const xcv_table_t *t = xcv_1080p_table(XCV_BOARD_CORONA);
    CHECK(t->have && xcv_1080p_usable(XCV_BOARD_CORONA), "Corona usável (%s)", t->source);
    CHECK(!xcv_1080p_usable(XCV_BOARD_PRE_CORONA), "pre-Corona continua placeholder");
    CHECK(t->crtc.total_w == 2200 && t->crtc.total_h == 1125 && t->crtc.hsync_offset == 191 && t->crtc.vsync_offset == 41,
          "crtc do dump");
    CHECK(t->ana[0xd0] == 0x33333333u && t->ana[0x13] == 0x82500898u && t->ana[0x58] == 0xffffffffu, "ana do dump");
    int scl = 0, upd = 0;
    for (int i = 0; i < t->n_post; i++) {
        if ((t->post[i].reg == 0x65b4 || t->post[i].reg == 0x65c4) && t->post[i].val == (xcv_scale_ratio(1280, 1920) & ~0x1Fu)) scl++;
        if (t->post[i].reg == 0x65cc || t->post[i].reg < 0x6500) upd++;
    }
    CHECK(scl == 2, "post tem as razões do scaler (gpu bswap detectado)");
    CHECK(upd == 0, "post não tem D1SCL_UPDATE nem D1GRPH/CRTC");
#else
    for (int b = 0; b < XCV_BOARD_COUNT; b++) {
        const xcv_table_t *t = xcv_1080p_table(b);
        CHECK(t != NULL, "tabela %d", b);
        if (!t->have) CHECK(!xcv_1080p_usable(b), "placeholder %d não pode ser usável", b);
        else CHECK(xcv_1080p_usable(b), "tabela %d do repo (%s) tem que ser usável", b, t->source);
        printf("  tabela 1080p %s: %s\n", b ? "Corona" : "pre-Corona", t->have ? t->source : "placeholder (fica 720p)");
        if (!t->have) continue;
        /* tabela real: CRTC = CEA 1080p60 (o mesmo que o f1 do libxenon escreve) */
        xcv_crtc_t c;
        xcv_crtc_from_timing(&XCV_CEA_1080P60, &c);
        CHECK(!memcmp(&c, &t->crtc, sizeof(c)), "tabela %d: crtc != CEA", b);
        /* HANA: totais HDMI no 0x13 (2200) e 0x15 (1125), como 0x672/0x2ee no 720p */
        CHECK((t->ana[0x13] & 0xFFF) == 2200 && (t->ana[0x15] & 0xFFF) == 1125, "tabela %d: ana 0x13/0x15", b);
        int seen = 0;
        for (int i = 0; i < t->n_post; i++) {
            const xcv_reg_t *p = &t->post[i];
            if (p->reg == 0x6584) { CHECK(p->val == ((1280u << 16) | 720u), "viewport 0x%08x", p->val); seen |= 1; }
            if (p->reg == 0x6590) { CHECK(p->val == 1, "scaler ligado"); seen |= 2; }
            if (p->reg == 0x65b4 || p->reg == 0x65c4) { CHECK(p->val == (xcv_scale_ratio(1280, 1920) & ~0x1Fu), "razão 0x%08x", p->val); seen |= 4; }
            if (p->reg == 0x652c) { CHECK(p->val == 1080, "desktop height %u", p->val); seen |= 8; }
            CHECK(p->reg >= 0x6500 && p->reg != 0x65cc, "post 0x%04x fora do bloco D1MODE/D1SCL", p->reg);
        }
        CHECK(seen == 15, "tabela %d: post incompleto (%d)", b, seen);
        /* v0.21.21: extras do D1MODE e o MVP/TRIG do CRTC do dump */
        int ex = 0;
        for (int i = 0; i < t->n_post; i++) {
            if (t->post[i].reg == 0x6524) { CHECK(t->post[i].val == 0, "0x6524 do kernel = 0"); ex |= 1; }
            if (t->post[i].reg == 0x65e0) { CHECK(t->post[i].val == 1, "0x65e0 do kernel = 1"); ex |= 2; }
        }
        CHECK(ex == 3, "tabela %d: extras no post", b);
        CHECK(t->crtc_post && t->n_crtc_post == 8, "tabela %d: crtc_post %d", b, t->n_crtc_post);
        for (int i = 0; t->crtc_post && i < t->n_crtc_post; i++)
            CHECK(t->crtc_post[i].reg >= 0x6038 && t->crtc_post[i].reg <= 0x6068, "crtc_post 0x%04x", t->crtc_post[i].reg);
        xcv_reg_t r[8];
        int n = xcv_scaler_regs(1280, 720, 1920, 1080, r, 8);
        for (int i = 0; i < n; i++)
            for (int k = 0; k < t->n_post; k++)
                if (r[i].reg == t->post[k].reg)
                    CHECK(r[i].val == t->post[k].val, "reserva 0x%04x 0x%08x != dump 0x%08x", r[i].reg, r[i].val, t->post[k].val);
    }
#endif
}


/* ---- v0.21.22: modelo do filtro polifásico do scaler ----
 * Saída j amostra a fonte na posição pos = j * razão (16.16). Cada fase φ
 * tem os pesos dos taps; o hardware usa coeficientes de 12 bits de fração
 * (1.0 = 4096). Bilinear 2 taps: (1-φ, φ). */
#define SCL_ONE 4096
static void bilinear_coefs(int phase, int nphase, int w[2]) {
    w[1] = (phase * SCL_ONE + nphase / 2) / nphase;
    w[0] = SCL_ONE - w[1];
}

/* 1D: src (n) -> dst (m) com razão n/m, 2 taps, 64 fases */
static void scale_1d(const int *src, int n, int *dst, int m, int highpass) {
    const int NPH = 64;
    unsigned long long ratio = ((unsigned long long)n << 16) / (unsigned)m;
    for (int j = 0; j < m; j++) {
        unsigned long long pos = (unsigned long long)j * ratio;
        int i0 = (int)(pos >> 16), ph = (int)(((pos & 0xFFFF) * NPH) >> 16);
        int w[2];
        bilinear_coefs(ph, NPH, w);
        if (highpass) { w[0] = SCL_ONE; w[1] = -SCL_ONE; }   /* a falha vista: soma 0 */
        int a = src[i0], b = src[i0 + 1 < n ? i0 + 1 : n - 1];
        dst[j] = (a * w[0] + b * w[1] + SCL_ONE / 2) / SCL_ONE;
    }
}

static void test_scl_model(void) {
    /* fases: soma 1.0 e sem peso negativo (sem ringing) */
    for (int ph = 0; ph < 64; ph++) {
        int w[2];
        bilinear_coefs(ph, 64, w);
        CHECK(w[0] + w[1] == SCL_ONE && w[0] >= 0 && w[1] >= 0, "fase %d: %d+%d", ph, w[0], w[1]);
    }
    enum { N = 1280, M = 1920 };
    static int src[N], dst[M];
    /* campo liso fica liso (ganho DC 1.0) */
    for (int i = 0; i < N; i++) src[i] = 200;
    scale_1d(src, N, dst, M, 0);
    int flat = 1;
    for (int j = 0; j < M; j++) flat &= dst[j] == 200;
    CHECK(flat, "campo liso 200 nao ficou liso");
    /* a falha (filtro passa-alta) zera o liso: o teste pega */
    scale_1d(src, N, dst, M, 1);
    CHECK(dst[100] == 0, "modelo passa-alta deveria dar 0 no liso");
    /* degrau: monotônico e dentro de [16, 235] (sem overshoot) */
    for (int i = 0; i < N; i++) src[i] = i < 640 ? 16 : 235;
    scale_1d(src, N, dst, M, 0);
    int mono = 1, inside = 1;
    for (int j = 1; j < M; j++) mono &= dst[j] >= dst[j - 1];
    for (int j = 0; j < M; j++) inside &= dst[j] >= 16 && dst[j] <= 235;
    CHECK(mono && inside, "degrau: mono=%d dentro=%d", mono, inside);
    /* rampa: erro máximo pequeno vs interpolação exata */
    for (int i = 0; i < N; i++) src[i] = i % 256;
    scale_1d(src, N, dst, M, 0);
    CHECK(dst[0] == 0 && dst[3] == 2 && dst[300] == 200, "rampa %d %d %d", dst[0], dst[3], dst[300]);

    /* fase inicial: a fórmula reproduz o que o kernel escreveu nos 2 dumps */
    struct { uint32_t r; int taps; uint32_t want; } I[] = {
        { 0x00aaaaa0, 6, 0x3d600 }, { 0x00aaaaa0, 5, 0x35600 },   /* 1080p B: 0x504 */
        { 0x00555560, 6, 0x3aa00 }, { 0x0071c720, 6, 0x3b800 },   /* 1080p A: 0x505 */
        { 0x00800000, 10, 0x5c000 }, { 0x00aaaaa0, 6, 0x3d600 },  /* 720p A: 0x905 */
        { 0x01000000, 10, 0x60000 }, { 0x01000000, 5, 0x38000 },  /* 720p B: 0x904 */
    };
    for (size_t i = 0; i < sizeof(I) / sizeof(I[0]); i++)
        CHECK(xcv_scl_init(I[i].r, I[i].taps) == I[i].want, "init(0x%08x, %d) = 0x%05x (dump 0x%05x)", I[i].r,
              I[i].taps, xcv_scl_init(I[i].r, I[i].taps), I[i].want);

    xcv_scl_t f;
    xcv_scl_filter(XCV_SCL_BILINEAR, 0x00aaaaa0, 0x00aaaaa0, 0x01000000, &f);
    CHECK(f.tap == 0x101 && f.hctl == 0x100 && f.vctl == 0x01000100 && f.hinit == 0x1d600 && f.vinit == 0x1d600,
          "bilinear: tap %x h %x v %x hi %x vi %x", f.tap, f.hctl, f.vctl, f.hinit, f.vinit);
    xcv_scl_filter(XCV_SCL_BILINEAR_RHD, 0x00aaaaa0, 0x00aaaaa0, 0x01000000, &f);
    CHECK(f.tap == 0x101 && f.hctl == 0x00030100 && f.vctl == 0x00030100, "rhd");
    xcv_scl_filter(XCV_SCL_NEAREST, 0x00aaaaa0, 0x00aaaaa0, 0x01000000, &f);
    CHECK(f.tap == 0 && (f.hctl & 1) && (f.vctl & 1) && !(f.hctl & 0x100), "nearest");
    xcv_scl_filter(XCV_SCL_KERNEL, 0x00aaaaa0, 0x00aaaaa0, 0x01000000, &f);
    CHECK(f.tap == 0x504 && f.hinit == 0x3d600 && f.vinit == 0x35600 && f.vctl == 0x01000000, "kernel = dump");
    CHECK(xcv_scl_kind(0) == XCV_SCL_BILINEAR && xcv_scl_kind(XCV_F_SCL_KERNEL | XCV_F_SCL_RHD) == XCV_SCL_KERNEL,
          "scl_kind");
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "edid";
    test_edid(dir);
    test_choose(dir);
    test_timing();
    test_scaler();
    test_scl_model();
    test_table();
    printf("video_test%s: %d ok, %d falhas\n", XCV_EXPECT_DUMP ? " (dump sintetico)" : "", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
