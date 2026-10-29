/*
 * orbita v0.21.33 — testes do host das Configurações (MENU-SETTINGS-PLAN.md):
 *   - xc_cfgnav: coluna/painel/subpáginas, LB/RB, Guia, confirmação, B/A;
 *   - xc_padcfg: padrão = valores de compilação, % <-> bruto, limites, PAD.TXT
 *     (ida e volta, arquivos ruins), a conta do jogo IGUAL à do main.c antigo,
 *     Medir sozinho com sequências gravadas;
 *   - xc_cfgview: opções por seção (cabo/Wi-Fi/sem adaptador, 1080p, áudio),
 *     volume 100/150/200/250 e personalizado, textos do vídeo, calibração;
 *   - xc_notes: NOTES.TXT (formato, arquivo ruim, corte em 512 B).
 * Uso: hosttest/settings/run_settings.sh
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/settings/xc_cfgnav.h"
#include "../../source/settings/xc_cfgview.h"
#include "../../source/settings/xc_notes.h"
#include "../../source/input/xc_padcfg.h"
#include "../../source/video/xc_video_mode.h"
#include "../../source/update/xc_update.h"

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("  FALHOU %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- a conta do main.c até a v0.21.32 (apply_deadzone + apply_curve), copiada ---- */
static void old_deadzone(int16_t *x, int16_t *y, float dz) {
    float fx = *x, fy = *y;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag <= dz) { *x = 0; *y = 0; return; }
    float m = mag > STICK_OUTER ? STICK_OUTER : mag;
    float k = (m - dz) / (STICK_OUTER - dz) * 32767.0f / mag;
    float ox = fx * k, oy = fy * k;
    if (ox > 32767.0f) ox = 32767.0f; else if (ox < -32767.0f) ox = -32767.0f;
    if (oy > 32767.0f) oy = 32767.0f; else if (oy < -32767.0f) oy = -32767.0f;
    *x = (int16_t)ox;
    *y = (int16_t)oy;
}
static void old_curve(int16_t *x, int16_t *y) {
    float fx = *x, fy = *y;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag < 1.0f) return;
    float m = mag / 32767.0f;
    if (m > 1.0f) m = 1.0f;
    float out = RSTICK_ADZ + (RSTICK_GAIN - RSTICK_ADZ) * powf(m, RSTICK_EXPO);
    float k = out * 32767.0f / mag;
    *x = (int16_t)(fx * k);
    *y = (int16_t)(fy * k);
}

static void t_padcfg(void) {
    printf("zona morta (xc_padcfg)\n");
    xpc_cfg_t c, d;
    xpc_defaults(&c);
    CHECK(c.dz[XPC_LEFT] == STICK_DZ && c.dz[XPC_RIGHT] == RSTICK_DZ);
    CHECK(c.outer[XPC_LEFT] == STICK_OUTER && c.outer[XPC_RIGHT] == STICK_OUTER);
    CHECK(xpc_pct(12000) == 37 && xpc_pct(31000) == 95 && xpc_pct(9000) == 27);
    CHECK(xpc_raw(0) == 0 && xpc_raw(100) == 32767 && xpc_pct(xpc_raw(42)) == 42);
    /* padrão passa pelo clamp sem mudar um bit (quem não mexe não sente) */
    d = c;
    xpc_clamp_stick(&d, 0, 0);
    xpc_clamp_stick(&d, 1, 1);
    CHECK(xpc_equal(&c, &d) && xpc_is_default(&d));

    /* a conta: com o padrão, idêntica à antiga em todo o curso */
    srand(1234);
    int diffs = 0;
    for (int i = 0; i < 200000; i++) {
        int16_t x = (int16_t)(rand() % 65536 - 32768), y = (int16_t)(rand() % 65536 - 32768);
        if (i < 4000) { x = (int16_t)((i % 80 - 40) * 800); y = (int16_t)((i / 80 - 25) * 1300); }
        int16_t ax = x, ay = y, bx = x, by = y, cx = x, cy = y, dx = x, dy = y;
        old_deadzone(&ax, &ay, STICK_DZ);
        xpc_apply(XPC_LEFT, c.dz[0], c.outer[0], &bx, &by);
        old_deadzone(&cx, &cy, RSTICK_DZ);
        old_curve(&cx, &cy);
        xpc_apply(XPC_RIGHT, c.dz[1], c.outer[1], &dx, &dy);
        if (ax != bx || ay != by || cx != dx || cy != dy) diffs++;
    }
    CHECK(diffs == 0);
    if (diffs) printf("  %d diferenças contra a conta antiga\n", diffs);
    /* config pela metade (zona morta >= limite) não divide por zero */
    {
        int16_t x = 20000, y = 0;
        xpc_apply(XPC_LEFT, 20000, 20000, &x, &y);
        CHECK(x == 0 && y == 0);
        x = 30000; y = 0;
        xpc_apply(XPC_LEFT, 20000, 10000, &x, &y);
        CHECK(x >= 32766 && y == 0);   /* tudo depois da zona morta = cheio */
    }
    CHECK(xpc_out_pct(XPC_LEFT, 12000, 31000, 8000, 0) == 0);
    CHECK(xpc_out_pct(XPC_LEFT, 12000, 31000, 32767, 0) == 100);
    CHECK(xpc_out_pct(XPC_RIGHT, 9000, 31000, 9500, 0) >= 19);   /* câmera: curva começa em 20% */

    /* limites: mínimo 0..60, máximo 60..100, máximo >= mínimo + 10 */
    d = c;
    for (int i = 0; i < 100; i++) xpc_step(&d, 0, 0, +1);
    CHECK(xpc_pct(d.dz[0]) == 60 && xpc_pct(d.outer[0]) == 95);
    for (int i = 0; i < 100; i++) xpc_step(&d, 0, 1, -1);
    CHECK(xpc_pct(d.outer[0]) == 70 && xpc_pct(d.dz[0]) == 60);   /* parou em mínimo + 10 */
    for (int i = 0; i < 100; i++) xpc_step(&d, 0, 0, -1);
    CHECK(xpc_pct(d.dz[0]) == 0);
    for (int i = 0; i < 100; i++) xpc_step(&d, 0, 1, -1);
    CHECK(xpc_pct(d.outer[0]) == 60);
    for (int i = 0; i < 100; i++) xpc_step(&d, 0, 1, +1);
    CHECK(xpc_pct(d.outer[0]) == 100 && d.outer[0] == XPC_FULL);
    CHECK(d.dz[1] == c.dz[1] && d.outer[1] == c.outer[1]);   /* o outro analógico não mexe */
    CHECK(xpc_step(&c, 0, 0, +1) == 1 && xpc_pct(c.dz[0]) == 38);
    xpc_default_stick(&c, 0);
    CHECK(xpc_is_default(&c));

    /* PAD.TXT */
    char b[128];
    xpc_cfg_t e;
    int n = xpc_format(&c, b, sizeof(b));
    CHECK(n > 0 && !strcmp(b, "XCPAD1\nL 12000 31000\nR 9000 31000\n"));
    CHECK(xpc_parse(b, (size_t)n, &e) == 0 && xpc_equal(&c, &e));
    const char *ok2 = "XCPAD1\r\nR 5000 30000\r\nL 3277 32767\r\n\r\n";
    CHECK(xpc_parse(ok2, strlen(ok2), &e) == 0 && e.dz[0] == 3277 && e.outer[0] == 32767 && e.dz[1] == 5000);
    const char *clampme = "XCPAD1\nL 30000 20000\nR 0 0\n";   /* zona morta > 60%, limite < 60% */
    /* arquivo: zona morta presa em 60%; a distância de 10% faz a zona morta ceder (51% / 61%) */
    CHECK(xpc_parse(clampme, strlen(clampme), &e) == 0 && xpc_pct(e.dz[0]) == 51 && xpc_pct(e.outer[0]) == 61 &&
          xpc_pct(e.outer[1]) == 60 && e.dz[1] == 0);
    static const char *bad[] = { "", "XCPAD", "XCPAD2\nL 1 2\nR 1 2\n", "XCPAD1\nL 12000 31000\n",
                                 "XCPAD1\nL 12000\nR 9000 31000\n", "XCPAD1\nL 12000 31000\nR 9000 31000 5\n",
                                 "XCPAD1\nL -1 31000\nR 9000 31000\n", "XCPAD1\nL 12000 99999\nR 9000 31000\n",
                                 "XCPAD1\nL 12000 31000\nR 9000 31000\nX 1 2\n", "lixo\nL 1 2\nR 1 2\n",
                                 "XCPAD1 x\nL 12000 31000\nR 9000 31000\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        e.dz[0] = 1;
        CHECK(xpc_parse(bad[i], strlen(bad[i]), &e) == -1 && xpc_is_default(&e));
    }
    CHECK(xpc_parse(NULL, 0, &e) == -1);
    CHECK(xpc_format(&c, b, 10) == -1);
}

/* Medir sozinho: repouso com drift de ~6%, depois um círculo de raio 0,92 */
static void t_measure(void) {
    printf("medir sozinho\n");
    xpc_cfg_t c;
    xpc_meas_t m;
    xpc_defaults(&c);
    xpc_meas_start(&m, 1000);
    uint32_t t = 1000;
    for (; t < 3000; t += 8) {   /* 2 s solto: drift até 6% */
        int ang = (int)t % 360;
        int r = 1000 + (int)(t % 997);   /* < 2000 + 966 */
        if (t == 2000) r = (int)(0.06 * 32767);
        CHECK(xpc_meas_feed(&m, t, (int)(r * cos(ang)), (int)(r * sin(ang)), &c, 0) == XPC_M_REST);
    }
    CHECK(xpc_meas_left_s(&m, t) == 0 || m.stage == XPC_M_REST);
    int st = 0;
    for (int i = 0; t < 8100; t += 8, i++) {   /* 5 s girando: raio 92% */
        double a = i * 0.05;
        st = xpc_meas_feed(&m, t, (int)(0.92 * 32767 * cos(a)), (int)(0.92 * 32767 * sin(a)), &c, 0);
        if (st == XPC_M_DONE) break;
    }
    CHECK(st == XPC_M_DONE && m.spun);
    CHECK(m.dz_pct == 10);                 /* 6% * 1,25 + 2% = 9,5 -> 10 */
    CHECK(m.out_pct == 89);                /* 92% * 0,97 = 89,2 */
    CHECK(xpc_pct(c.dz[0]) == 10 && xpc_pct(c.outer[0]) == 89);
    CHECK(c.dz[1] == RSTICK_DZ);           /* o outro não mexe */

    /* não girou: o máximo fica; repouso perfeito: mínimo 5% */
    xpc_defaults(&c);
    xpc_meas_start(&m, 0);
    for (t = 0; t <= 2000; t += 10) xpc_meas_feed(&m, t, 0, 0, &c, 1);
    CHECK(m.stage == XPC_M_SPIN);
    for (; t <= 7100; t += 10) st = xpc_meas_feed(&m, t, 30000, 0, &c, 1);   /* só um setor */
    CHECK(st == XPC_M_DONE && !m.spun && xpc_meas_sectors(&m) == 1);
    CHECK(xpc_pct(c.dz[1]) == 5 && c.outer[1] == STICK_OUTER);

    /* drift enorme: mínimo preso em 60%, máximo empurrado pra 70% */
    xpc_defaults(&c);
    xpc_meas_start(&m, 0);
    for (t = 0; t <= 2000; t += 10) xpc_meas_feed(&m, t, 20000, 0, &c, 0);
    for (int i = 0; t <= 7100; t += 10, i++) {
        double a = i * 0.07;
        xpc_meas_feed(&m, t, (int)(0.6 * 32767 * cos(a)), (int)(0.6 * 32767 * sin(a)), &c, 0);
    }
    CHECK(m.stage == XPC_M_DONE && xpc_pct(c.dz[0]) == 60 && xpc_pct(c.outer[0]) == 70);
}

static xcf_ev_t key(xcf_nav_t *n, int k) { return xcf_key(n, k); }

static void t_nav(void) {
    printf("navegação (xc_cfgnav)\n");
    xcf_nav_t n;
    xcf_init(&n);
    CHECK(key(&n, XCF_K_A).act == XCF_A_NONE);   /* fechada: nada */
    xcf_open(&n);
    CHECK(n.open && n.sec == XCF_SEC_ACCOUNT && n.item == 0 && !n.right);
    xcf_set_options(&n, XCF_SEC_ACCOUNT, 3, 0);
    xcf_set_options(&n, XCF_SEC_NET, 0, 0);       /* cabo: nada pra focar */
    xcf_set_options(&n, XCF_SEC_AV, 3, 1u << 2);  /* volume aceita esquerda/direita */
    xcf_set_options(&n, XCF_SEC_PADS, 3, 0);
    /* coluna: ↓ troca de seção e o painel acompanha */
    CHECK(key(&n, XCF_K_DOWN).act == XCF_A_MOVED && n.sec == XCF_SEC_NET && n.item == 1);
    CHECK(key(&n, XCF_K_RIGHT).act == XCF_A_NONE && !n.right);   /* Rede com cabo: → não faz nada */
    CHECK(key(&n, XCF_K_A).act == XCF_A_NONE && !n.right);
    CHECK(key(&n, XCF_K_LEFT).act == XCF_A_NONE);
    key(&n, XCF_K_DOWN);
    CHECK(n.sec == XCF_SEC_AV);
    CHECK(key(&n, XCF_K_RIGHT).act == XCF_A_MOVED && n.right && n.focus[XCF_SEC_AV] == 0);
    CHECK(key(&n, XCF_K_UP).act == XCF_A_NONE);
    key(&n, XCF_K_DOWN);
    key(&n, XCF_K_DOWN);
    CHECK(key(&n, XCF_K_DOWN).act == XCF_A_NONE && n.focus[XCF_SEC_AV] == 2);
    xcf_ev_t e = key(&n, XCF_K_RIGHT);
    CHECK(e.act == XCF_A_ADJUST && e.dir == 1 && e.sec == XCF_SEC_AV && e.opt == 2);
    e = key(&n, XCF_K_LEFT);
    CHECK(e.act == XCF_A_ADJUST && e.dir == -1 && n.right);         /* no volume, ← ajusta */
    e = key(&n, XCF_K_A);
    CHECK(e.act == XCF_A_OPTION && e.opt == 2);
    key(&n, XCF_K_UP);
    CHECK(key(&n, XCF_K_RIGHT).act == XCF_A_NONE);                   /* opção comum: → nada */
    CHECK(key(&n, XCF_K_LEFT).act == XCF_A_MOVED && !n.right && n.item == XCF_SEC_AV);   /* ← = volta */
    key(&n, XCF_K_A);
    CHECK(n.right && n.focus[XCF_SEC_AV] == 0);                      /* entra na 1ª opção */
    CHECK(key(&n, XCF_K_B).act == XCF_A_MOVED && !n.right);          /* B no painel: coluna */
    /* LB/RB de qualquer lugar, foco vai pra coluna, sem dar a volta */
    key(&n, XCF_K_A);
    CHECK(key(&n, XCF_K_RB).act == XCF_A_MOVED && n.sec == XCF_SEC_PADS && !n.right);
    for (int i = 0; i < XCF_NSEC; i++) key(&n, XCF_K_RB);
    CHECK(n.sec == XCF_NSEC - 1);                                     /* sem dar a volta */
    for (int i = 0; i < XCF_NSEC; i++) key(&n, XCF_K_LB);
    CHECK(n.sec == XCF_SEC_ACCOUNT && n.item == 0);
    /* energia: A abre a confirmação; Cancelar é o padrão; Guia não faz nada nela */
    for (int i = 0; i < XCF_NITEMS + 2; i++) key(&n, XCF_K_DOWN);
    CHECK(n.item == XCF_IT_SHUTDOWN && n.sec == XCF_NSEC - 1);       /* o painel fica na última seção */
    CHECK(key(&n, XCF_K_RIGHT).act == XCF_A_NONE);
    key(&n, XCF_K_A);
    CHECK(n.confirm == XCF_CF_SHUTDOWN && n.confirm_yes == 0);
    CHECK(key(&n, XCF_K_GUIDE).act == XCF_A_NONE && n.open && n.confirm);
    CHECK(key(&n, XCF_K_A).act == XCF_A_MOVED && !n.confirm);        /* A em Cancelar */
    key(&n, XCF_K_UP);
    CHECK(n.item == XCF_IT_REBOOT);
    key(&n, XCF_K_A);
    key(&n, XCF_K_RIGHT);
    CHECK(key(&n, XCF_K_A).act == XCF_A_REBOOT);
    key(&n, XCF_K_A);
    key(&n, XCF_K_RIGHT);
    CHECK(key(&n, XCF_K_B).act == XCF_A_MOVED && !n.confirm);        /* B fecha o diálogo */
    /* LB a partir da energia: seção anterior à mostrada */
    CHECK(key(&n, XCF_K_LB).act == XCF_A_MOVED && n.sec == XCF_NSEC - 2 && n.item == XCF_NSEC - 2);
    /* B na coluna fecha; reabre na última seção */
    e = key(&n, XCF_K_B);
    CHECK(e.act == XCF_A_CLOSE && !n.open);
    xcf_open(&n);
    CHECK(n.open && n.sec == XCF_NSEC - 2 && n.item == XCF_NSEC - 2 && !n.right && n.page == XCF_PG_MAIN);
    /* subpágina das notas: ↑/↓ vão pra ela; B/← voltam pra seção; LB/RB trocam de seção */
    for (int i = 0; i < XCF_NSEC; i++) key(&n, XCF_K_LB);
    key(&n, XCF_K_A); key(&n, XCF_K_DOWN);
    xcf_set_page(&n, XCF_PG_NOTES);
    e = key(&n, XCF_K_DOWN);
    CHECK(e.act == XCF_A_PAGE && e.page == XCF_PG_NOTES && e.key == XCF_K_DOWN);
    e = key(&n, XCF_K_B);
    CHECK(e.act == XCF_A_PAGE_BACK && e.page == XCF_PG_NOTES && n.page == XCF_PG_MAIN && n.right &&
          n.focus[XCF_SEC_ACCOUNT] == 1);
    xcf_set_page(&n, XCF_PG_NOTES);
    CHECK(key(&n, XCF_K_LEFT).act == XCF_A_PAGE_BACK);
    xcf_set_page(&n, XCF_PG_NOTES);
    e = key(&n, XCF_K_RB);
    CHECK(e.act == XCF_A_PAGE_BACK && e.page == XCF_PG_NOTES && n.sec == XCF_SEC_NET && n.page == XCF_PG_MAIN);
    /* calibração: tudo vai pra ela, LB/RB não saem, B salva e volta, Guia fecha tudo */
    key(&n, XCF_K_DOWN); key(&n, XCF_K_DOWN);
    key(&n, XCF_K_A);
    xcf_set_page(&n, XCF_PG_CALIB);
    CHECK(key(&n, XCF_K_LB).act == XCF_A_NONE && n.page == XCF_PG_CALIB);
    e = key(&n, XCF_K_LEFT);
    CHECK(e.act == XCF_A_PAGE && e.key == XCF_K_LEFT);
    CHECK(key(&n, XCF_K_Y).act == XCF_A_PAGE);
    e = key(&n, XCF_K_B);
    CHECK(e.act == XCF_A_PAGE_BACK && e.page == XCF_PG_CALIB && n.right && n.page == XCF_PG_MAIN);
    xcf_set_page(&n, XCF_PG_CALIB);
    e = key(&n, XCF_K_GUIDE);
    CHECK(e.act == XCF_A_CLOSE && e.page == XCF_PG_CALIB && !n.open && n.page == XCF_PG_MAIN);
    /* Guia no painel e na coluna também fecham */
    xcf_open(&n);
    CHECK(key(&n, XCF_K_GUIDE).act == XCF_A_CLOSE && !n.open);
    /* a seção aberta perdeu as opções (o cabo pegou DHCP): foco volta pra coluna */
    xcf_open(&n);
    xcf_set_options(&n, XCF_SEC_NET, 1, 0);
    for (int i = 0; i < XCF_NSEC; i++) key(&n, XCF_K_LB);
    key(&n, XCF_K_RB);
    key(&n, XCF_K_A);
    CHECK(n.sec == XCF_SEC_NET && n.right);
    xcf_set_options(&n, XCF_SEC_NET, 0, 0);
    CHECK(!n.right && n.item == XCF_SEC_NET);
    /* restaurar padrão: confirmação própria */
    xcf_ask(&n, XCF_CF_PADRESET);
    key(&n, XCF_K_RIGHT);
    CHECK(key(&n, XCF_K_A).act == XCF_A_PADRESET);
    /* X não existe aqui: qualquer tecla desconhecida = nada */
    CHECK(key(&n, 99).act == XCF_A_NONE);
}

static void base_in(xcs_in_t *in) {
    memset(in, 0, sizeof(*in));
    in->beta_build = 1;
    in->key_row = 1;
    in->console_short = "A1B2-C3D4-E5F6";
    in->console_full = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
    in->version = "v0.21.33";
    in->channel = "beta";
    in->key_masked = "*****-**P9H";
    in->key_when = "validada em 25/10 22:56";
    in->upd_phase = XCU_UP_TO_DATE;
    in->cable = 1;
    in->cable_ip = (10u << 24) | 7;
    in->av_kind = XCV_AV_HDMI;
    in->mode = XCV_HDMI_720P;
    in->ovr = XCV_OVR_AUTO;
    in->have_1080p = 1;
    in->audio = 1;
    in->volume = 100;
    xpc_defaults(&in->pad);
}

static int find_row(const xcs_view_t *v, const char *label) {
    for (int i = 0; i < v->cfg.nrows; i++)
        if (v->rows[i].label && !strcmp(v->rows[i].label, label)) return i;
    return -1;
}

static void t_view(void) {
    printf("telas (xc_cfgview)\n");
    static xcs_view_t v;
    xcs_in_t in;
    xcf_nav_t n;
    int ids[XCF_MAX_OPT];
    unsigned adj;
    base_in(&in);
    xcf_init(&n);
    xcf_open(&n);
    for (int s = 0; s < XCF_NSEC; s++) xcf_set_options(&n, s, xcs_options(&in, s, ids, &adj), adj);
    /* Conta e sobre: com chave / build pública */
    CHECK(xcs_options(&in, XCF_SEC_ACCOUNT, ids, &adj) == 3 && ids[0] == XCS_OPT_KEY && ids[2] == XCS_OPT_UPDATE);
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(v.cfg.ninfo == 2 && !strcmp(v.cfg.info[1].big, "orbita v0.21.33") && strstr(v.cfg.info[1].small, "canal beta"));
    CHECK(find_row(&v, "Chave beta") == 0 && strstr(v.rows[0].value, "validada em 25/10 22:56"));
    CHECK(v.cfg.note && strstr(v.cfg.note, "foto desta tela"));
    CHECK(!strcmp(v.cfg.hint, "A:Selecionar|LB/RB:Seção|B:Fechar"));
    in.beta_build = 0; in.key_row = 0;
    CHECK(xcs_options(&in, XCF_SEC_ACCOUNT, ids, &adj) == 2 && ids[0] == XCS_OPT_NOTES);
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(find_row(&v, "Chave beta") < 0 && !strcmp(v.cfg.info[1].small, "versão pública"));
    base_in(&in);
    in.upd_avail = 1; in.upd_latest = "0.22.0"; in.upd_phase = XCU_AVAILABLE;
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(find_row(&v, "Atualizar para v0.22.0") == 2 && (v.rows[2].flags & XUI_CR_WARN));
    in.upd_avail = 0; in.upd_busy = 1;
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(!strcmp(v.rows[2].value, "procurando…"));
    /* Rede */
    base_in(&in);
    CHECK(xcs_options(&in, XCF_SEC_NET, ids, &adj) == 2 && ids[0] == XCS_OPT_WIFI && ids[1] == XCS_OPT_SPEED);   /* v0.21.43 */
    n.sec = n.item = XCF_SEC_NET;
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(v.cfg.card == 1 && v.cfg.card_ok && !strcmp(v.cfg.card_text, "IP na rede local: 10.0.0.7") && v.cfg.nrows == 2);
    CHECK(strstr(v.cfg.text, "abra o Wi-Fi") != NULL);
    CHECK(find_row(&v, "Teste de velocidade") == 1);
    in.cable = 0; in.wifi = XCS_W_UP; in.wifi_ssid = "Casa"; in.wifi_rssi = -58; in.wifi_dhcp = 1;
    in.wifi_ip = (192u << 24) | (168u << 16) | 77;
    CHECK(xcs_options(&in, XCF_SEC_NET, ids, &adj) == 2 && ids[0] == XCS_OPT_WIFI && ids[1] == XCS_OPT_SPEED);
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(v.cfg.card == 2 && v.cfg.card_bars == 3 && !strcmp(v.cfg.card_title, "Wi-Fi: Casa"));
    CHECK(strstr(v.cfg.card_text, "sinal bom (-58 dBm)") && strstr(v.cfg.card_text, "192.168.0.77"));
    CHECK(!strcmp(v.cfg.card_text2, "Cabo de rede: não conectado"));
    in.wifi = XCS_W_NONE;
    CHECK(xcs_options(&in, XCF_SEC_NET, ids, &adj) == 0);
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(strstr(v.cfg.card_text, "não tem Wi-Fi interno") != NULL);
    /* Vídeo e Som */
    base_in(&in);
    n.sec = n.item = XCF_SEC_AV;
    CHECK(xcs_options(&in, XCF_SEC_AV, ids, &adj) == 3 && ids[0] == XCS_OPT_720 && ids[1] == XCS_OPT_1080 &&
          ids[2] == XCS_OPT_VOLUME && adj == 4u);
    xcs_build(&v, &in, &n, NULL, 0, 0);
    int r720 = find_row(&v, "720p"), r1080 = find_row(&v, "1080p"), rvol = find_row(&v, "Volume");
    CHECK(r720 == 0 && r1080 == 1 && rvol == 2);
    CHECK((v.rows[r720].flags & XUI_CR_ON) && !strcmp(v.rows[r720].tag, "EM USO") && !(v.rows[r1080].flags & XUI_CR_ON));
    CHECK(!(v.rows[r1080].flags & XUI_CR_DIM) && v.rows[r1080].tag == NULL);   /* sem "EM BREVE" */
    CHECK(strstr(v.rows[r1080].after, "Saída atual: HDMI  ·  720p  ·  60 Hz") && strstr(v.rows[r1080].after, "LB+RB"));
    CHECK(v.rows[rvol].choice == 0 && v.rows[rvol].nchoices == 4 && !strcmp(v.rows[rvol].choices[3], "250%"));
    in.ovr = XCV_OVR_1080P;   /* escolhido 1080p, TV ainda em 720p */
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK((v.rows[r1080].flags & XUI_CR_ON) && !strcmp(v.rows[r1080].tag, "AO REINICIAR"));
    CHECK(strstr(v.rows[r1080].after, "vale ao reiniciar") != NULL);
    in.mode = XCV_HDMI_1080P;
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(!strcmp(v.rows[r1080].tag, "EM USO") && v.rows[r720].tag == NULL && strstr(v.rows[r1080].after, "1080p  ·  60 Hz"));
    in.ovr = XCV_OVR_AUTO;   /* automático: o rádio segue o que está na TV */
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK((v.rows[r1080].flags & XUI_CR_ON) && !(v.rows[r720].flags & XUI_CR_ON));
    in.forced_720p = 1; in.mode = XCV_HDMI_720P; in.ovr = XCV_OVR_1080P;
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(strstr(v.rows[r1080].after, "720p forçado") != NULL);
    base_in(&in);
    in.av_kind = XCV_AV_COMPONENT; in.mode = XCV_YUV_720P; in.ovr = XCV_OVR_1080P;
    CHECK(xcs_options(&in, XCF_SEC_AV, ids, &adj) == 2 && ids[1] == XCS_OPT_VOLUME && adj == 2u);
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK((v.rows[r1080].flags & XUI_CR_DIM) && !strcmp(v.rows[r1080].value, "só no cabo HDMI"));
    CHECK((v.rows[r720].flags & XUI_CR_ON) && strstr(v.rows[r1080].after, "Componente"));
    base_in(&in);
    in.have_1080p = 0;
    CHECK(xcs_options(&in, XCF_SEC_AV, ids, &adj) == 2);
    in.audio = 0;
    CHECK(xcs_options(&in, XCF_SEC_AV, ids, &adj) == 1 && adj == 0);
    base_in(&in);
    in.volume = 180;
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(v.rows[rvol].choice == -1 && strstr(v.rows[rvol].after, "personalizado (VOLUME.TXT)"));
    n.right = 1; n.focus[XCF_SEC_AV] = 2;
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(v.cfg.focus == rvol && !strcmp(v.cfg.hint, "Esq/Dir:Volume|B:Voltar|LB/RB:Seção|Guia:Fechar"));
    in.have_1080p = 0; n.focus[XCF_SEC_AV] = 1;   /* 1080p apagado: a 2ª opção é o volume (linha 2) */
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(v.cfg.focus == rvol);
    /* volume */
    CHECK(xcs_vol_index(150) == 1 && xcs_vol_index(180) == -1);
    CHECK(xcs_vol_step(100, 1) == 150 && xcs_vol_step(250, 1) == 250 && xcs_vol_step(100, -1) == 100);
    CHECK(xcs_vol_step(180, 1) == 200 && xcs_vol_step(180, -1) == 150);
    CHECK(xcs_vol_step(400, -1) == 250 && xcs_vol_step(400, 1) == 400 && xcs_vol_step(50, 1) == 100);
    /* Controles */
    base_in(&in);
    n.sec = n.item = XCF_SEC_PADS; n.right = 0;
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(!strcmp(v.rows[0].value, "mínimo 37%  ·  máximo 95%") && !strcmp(v.rows[1].value, "mínimo 27%  ·  máximo 95%"));
    xcf_ask(&n, XCF_CF_PADRESET);
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(v.cfg.confirm && strstr(v.cfg.confirm->text, "esquerdo 37% / 95%, direito 27% / 95%") &&
          v.cfg.confirm->focus == 0);
    n.confirm = 0;
    /* notas */
    CHECK(xcs_notes_paras("- a\n- b\n\n  \n- c") == 3 && xcs_notes_paras("") == 0 && xcs_notes_paras(NULL) == 0);
    n.sec = XCF_SEC_ACCOUNT;
    xcf_set_page(&n, XCF_PG_NOTES);
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(v.cfg.page == XUI_CFG_NOTES && !v.cfg.notes && strstr(v.cfg.notes_empty, "próxima atualização"));
    in.notes = "x"; in.notes_from_file = 1; in.notes_when = "26/10/2026";
    xcs_build(&v, &in, &n, NULL, 0, 0);
    CHECK(!strcmp(v.cfg.title, "Novidades da v0.21.33") && !strcmp(v.cfg.subtitle, "Instalada em 26/10/2026 pela atualização do orbita"));
    CHECK(!strcmp(xcs_out_text(XCV_VGA_720P), "VGA  ·  1280 × 720"));
}

static void t_calib(void) {
    printf("calibração (xcs_cal)\n");
    xpc_cfg_t c;
    xpc_defaults(&c);
    static xcs_cal_t k;
    xcs_cal_begin(&k, &c, 2);
    CHECK(k.stick == XPC_LEFT && k.focus == 0 && k.port == 2);
    CHECK(xcs_cal_key(&k, XCF_K_RIGHT, 0) == XCS_CAL_CHANGED && xpc_pct(k.cfg.dz[0]) == 38);
    CHECK(xcs_cal_key(&k, XCF_K_Y, 0) == XCS_CAL_CHANGED && xpc_is_default(&k.cfg));
    CHECK(xcs_cal_key(&k, XCF_K_Y, 0) == XCS_CAL_REDRAW);
    CHECK(xcs_cal_key(&k, XCF_K_UP, 0) == XCS_CAL_NONE);
    xcs_cal_key(&k, XCF_K_DOWN, 0);
    CHECK(xcs_cal_key(&k, XCF_K_LEFT, 0) == XCS_CAL_CHANGED && xpc_pct(k.cfg.outer[0]) == 94);
    xcs_cal_key(&k, XCF_K_DOWN, 0);
    CHECK(xcs_cal_key(&k, XCF_K_LEFT, 0) == XCS_CAL_NONE);   /* no botão: ←/→ nada */
    CHECK(xcs_cal_key(&k, XCF_K_A, 100) == XCS_CAL_REDRAW && k.meas.stage == XPC_M_REST);
    CHECK(xcs_cal_key(&k, XCF_K_DOWN, 100) == XCS_CAL_NONE);   /* medindo: só A (cancela) e Y */
    xcs_cal_feed(&k, 200, 500, 0);
    CHECK(strstr(k.status, "Solte o analógico") != NULL);
    CHECK(xcs_cal_key(&k, XCF_K_A, 300) == XCS_CAL_REDRAW && k.meas.stage == XPC_M_IDLE);
    /* medição inteira pela tela */
    xcs_cal_key(&k, XCF_K_A, 1000);
    uint32_t t = 1000;
    int r = 0;
    for (; t <= 3000; t += 10) xcs_cal_feed(&k, t, 0, 0);
    for (int i = 0; t <= 8200 && r != XCS_CAL_CHANGED; t += 10, i++)
        r = xcs_cal_feed(&k, t, (int)(32767 * cos(i * 0.05)), (int)(32767 * sin(i * 0.05)));
    CHECK(r == XCS_CAL_CHANGED && k.last_result && strstr(k.status, "Medido: mínimo 5%, máximo 97%"));
    CHECK(xpc_pct(k.cfg.dz[0]) == 5 && xpc_pct(k.cfg.outer[0]) == 97);
    /* rastro: anda com a posição, some parado */
    for (int i = 0; i < 20; i++) xcs_cal_feed(&k, 10000 + i * 40, i * 1000, 0);
    CHECK(k.ntrail == XCS_CAL_TRAIL && k.tx[0] == 18000);
    for (int i = 0; i < 20; i++) xcs_cal_feed(&k, 11000 + i * 40, 19000, 0);
    CHECK(k.ntrail == 0);
    /* Próximo: direito -> Concluir */
    xcs_cal_key(&k, XCF_K_DOWN, 0);
    CHECK(k.focus == 3 && xcs_cal_key(&k, XCF_K_A, 0) == XCS_CAL_REDRAW && k.stick == XPC_RIGHT && k.focus == 0);
    xcs_cal_key(&k, XCF_K_RIGHT, 0);
    CHECK(xpc_pct(k.cfg.dz[1]) == 28 && xpc_pct(k.cfg.dz[0]) == 5);
    for (int i = 0; i < 3; i++) xcs_cal_key(&k, XCF_K_DOWN, 0);
    CHECK(xcs_cal_key(&k, XCF_K_A, 0) == XCS_CAL_DONE);
    /* a tela */
    static xcs_view_t v;
    xcs_in_t in;
    base_in(&in);
    xcf_nav_t n;
    xcf_init(&n);
    xcf_open(&n);
    n.sec = XCF_SEC_PADS;
    xcf_set_page(&n, XCF_PG_CALIB);
    xcs_build(&v, &in, &n, &k, 0, 0);
    CHECK(v.cfg.page == XUI_CFG_CALIB && v.cfg.calib && !strcmp(v.cfg.calib->step, "2 de 2") &&
          !strcmp(v.cfg.calib->next, "Concluir") && v.cfg.calib->dz_pct == 28);
    CHECK(!strcmp(v.cfg.title, "Calibrar analógico direito") && !strcmp(v.cfg.hint, "A:Selecionar|Y:Padrão|B:Salvar e voltar"));
    k.focus = 0;
    k.x = 32767; k.y = 0;
    xcs_build(&v, &in, &n, &k, 0, 0);
    CHECK(!strcmp(v.cfg.hint, "D-pad:Ajustar|Y:Padrão|B:Salvar e voltar") && v.cfg.calib->pos_pct == 100 &&
          v.cfg.calib->game_pct == 70 && v.cfg.calib->pad == NULL);   /* câmera: 0,20 + 0,50 = 70% no fim */
    k.multi = 1;
    xcs_build(&v, &in, &n, &k, 0, 0);
    CHECK(!strcmp(v.cfg.calib->pad, "Controle 3"));
}

static void t_notes(void) {
    printf("NOTES.TXT (xc_notes)\n");
    char b[XCN_FILE_MAX];
    xcn_notes_t o;
    int n = xcn_notes_format("0.21.33", 1761500000LL, "- Volume na tela\n- Calibração", b, sizeof(b));
    {
        const char *head = "XCN1\n0.21.33\n1761500000\n- Volume";
        CHECK(n > 0 && !strncmp(b, head, strlen(head)));
    }
    CHECK(xcn_notes_parse(b, (size_t)n, &o) == 0 && !strcmp(o.version, "0.21.33") && o.installed == 1761500000LL &&
          !strcmp(o.notes, "- Volume na tela\n- Calibração"));
    n = xcn_notes_format("0.21.33", -1, "", b, sizeof(b));
    CHECK(xcn_notes_parse(b, (size_t)n, &o) == 0 && o.installed == -1 && o.notes[0] == 0);
    const char *crlf = "XCN1\r\n0.21.34\r\n0\r\nNotas\r\n\r\n";
    CHECK(xcn_notes_parse(crlf, strlen(crlf), &o) == 0 && !strcmp(o.version, "0.21.34") && o.installed == -1 &&
          !strcmp(o.notes, "Notas"));
    static const char *bad[] = { "", "XCN2\n1\n0\nx", "XCN1\n\n0\nx", "XCN1\n0.21.33\nabc\nx", "XCN1\n0.21.33" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(xcn_notes_parse(bad[i], strlen(bad[i]), &o) == -1);
    CHECK(xcn_notes_format("1.0\n", 0, "x", b, sizeof(b)) == -1 && xcn_notes_format("", 0, "x", b, sizeof(b)) == -1);
    char big[2000];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    n = xcn_notes_format("0.21.33", 5, big, b, sizeof(b));
    CHECK(n > 0 && xcn_notes_parse(b, (size_t)n, &o) == 0 && strlen(o.notes) == XCN_NOTES_MAX);
}

int main(void) {
    t_padcfg();
    t_measure();
    t_nav();
    t_view();
    t_calib();
    t_notes();
    printf("settings: %d checks, %d falhas\n", g_checks, g_fails);
    printf("%s\n", g_fails ? "SETTINGS HOST TESTS FAILED" : "SETTINGS HOST TESTS PASSED");
    return g_fails ? 1 : 0;
}
