/*
 * orbita — MOCKUP da proposta de Configurações em menu lateral (MENU-SETTINGS-PLAN.md).
 * Só host: desenha com as primitivas públicas do XUI (xui_gfx/xui_text + atlas
 * xui_assets.h) e grava PNGs. NÃO é o código do app: quando a proposta for
 * aprovada, o desenho vira xui_settings2_frame() em source/ui/xui_home.c.
 * Uso: settings_v2 <catalog_fixtures_dir> <out_dir>   (ver run_settings_v2.sh)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../../source/ui/xui.h"
#include "../../source/ui/xui_assets.h"
#include "../../source/catalog/xc_catalog.h"
#include "xui_png.h"

#define W 1280
#define H 720

/* ---- layout ---- */
#define LX 64            /* coluna de seções */
#define LY 88
#define LW 312
#define LH 548
#define RX (LX + LW + 20)
#define RY LY
#define RW (W - 64 - RX)
#define RH LH
#define RIN 40           /* margem interna do painel da direita */
#define IT_H 50          /* item da coluna */
#define IT_P 56
#define ROW_H 58         /* linha de opção */
#define ROW_P 66

static uint32_t px[W * H], bgpx[W * H];
static xui_canvas_t C;
static char outdir[512];
static int fails;

/* ---------------- pedaços ---------------- */
static void chevron(int x, int cy, uint32_t col) {
    for (int i = 0; i < 6; i++) {
        xui_fill(&C, x + i, cy - 6 + i, 3, 2, col);
        xui_fill(&C, x + i, cy + 5 - i, 3, 2, col);
    }
}

static void bars(int x, int base, int n, uint32_t on, uint32_t off) {
    for (int i = 0; i < 4; i++) {
        int h = 5 + i * 4;
        xui_round_rect(&C, x + i * 6, base - h, 4, h, 1, i < n ? on : off, 255);
    }
}

enum { IC_ACCOUNT, IC_NET, IC_SCREEN, IC_PAD, IC_RESTART, IC_POWER };
static void icon(int kind, int cx, int cy, uint32_t col) {
    switch (kind) {
    case IC_ACCOUNT:
        xui_circle(&C, cx, cy - 5, 5, col, 255);
        xui_round_rect(&C, cx - 9, cy + 2, 18, 9, 5, col, 255);
        break;
    case IC_NET: bars(cx - 11, cy + 9, 4, col, col); break;
    case IC_SCREEN:
        xui_round_frame(&C, cx - 9, cy - 8, 18, 12, 2, 2, col);
        xui_fill(&C, cx - 1, cy + 6, 2, 3, col);
        xui_fill(&C, cx - 6, cy + 9, 12, 2, col);
        break;
    case IC_PAD: xui_mask(&C, &xui_icon_pad, cx - 9, cy - 6, col, 255); break;
    case IC_RESTART: xui_mask(&C, &xui_icon_restart, cx - 10, cy - 10, col, 255); break;
    case IC_POWER: xui_mask(&C, &xui_icon_power, cx - 10, cy - 10, col, 255); break;
    }
}

/* dicas: "A:Abrir|B:Voltar|LB/RB:Seção|Guia:Fechar" (pílula pros que não são A/B/X/Y) */
static uint32_t btn_col(char b) {
    return b == 'A' ? XUI_C_GREEN : b == 'B' ? XUI_C_RED : b == 'X' ? XUI_C_BLUE : XUI_C_YELLOW;
}
static int hints(int x, int base, const char *h, int measure) {
    int x0 = x;
    char seg[96];
    while (h && *h) {
        const char *e = strchr(h, '|');
        size_t n = e ? (size_t)(e - h) : strlen(h);
        memcpy(seg, h, n); seg[n] = 0;
        h = e ? e + 1 : NULL;
        char *colon = strchr(seg, ':');
        const char *txt = colon ? colon + 1 : seg;
        if (colon) {
            *colon = 0;
            if (strlen(seg) == 1) {
                if (!measure) {
                    xui_circle(&C, x + 10, base - 5, 10, btn_col(seg[0]), 255);
                    xui_text_center(&C, XUI_F_LABEL, x, 20, base, seg[0] == 'Y' ? 0 : XUI_C_TEXT, seg, 0);
                }
                x += 27;
            } else {
                int pw = xui_text_width(XUI_F_LABEL, seg, 0) + 16;
                if (!measure) {
                    xui_round_rect(&C, x, base - 15, pw, 20, 10, XUI_C_BUTTON, 255);
                    xui_text_center(&C, XUI_F_LABEL, x, pw, base, XUI_C_TEXT, seg, 0);
                }
                x += pw + 7;
            }
        }
        x = measure ? x + xui_text_width(XUI_F_LABEL, txt, 0) : xui_text(&C, XUI_F_LABEL, x, base, XUI_C_DIM, txt, 0, 0);
        x += 22;
    }
    return x - x0 - 22;
}
static void hints_right(const char *h) {
    int w = hints(0, 0, h, 1);
    hints(W - 64 - w, LY + LH + 36, h, 0);
}

/* ---------------- fundo + coluna ---------------- */
static const char *k_sections[] = { "Conta e sobre", "Rede", "Vídeo e Som", "Controles" };
static const int k_sec_icon[] = { IC_ACCOUNT, IC_NET, IC_SCREEN, IC_PAD };
enum { FOC_LEFT = 0, FOC_RIGHT = 1 };

/* sec = seção aberta (0..3), ou 4/5 = Reiniciar/Desligar em foco; where = FOC_* */
static void frame_base(int sec, int where, const char *version) {
    memcpy(px, bgpx, sizeof(px));
    xui_clip_all(&C);
    xui_fill_a(&C, 0, 0, W, H, XUI_C_BG, 200);
    xui_sprite(&C, &xui_logo, 64, 21);
    xui_text(&C, XUI_F_LABEL, 64 + xui_logo.w + 12, 43, XUI_C_HAZE, version, 0, 200);
    /* coluna */
    xui_round_rect(&C, LX, LY, LW, LH, 14, XUI_C_PANEL, 245);
    xui_mask(&C, &xui_icon_gear, LX + 24, LY + 30, XUI_C_TEXT, 255);
    xui_text(&C, XUI_F_H2, LX + 54, LY + 48, XUI_C_TEXT, "Configurações", 0, LW - 70);
    int x = LX + 14, w = LW - 28;
    for (int i = 0; i < 6; i++) {
        int y = i < 4 ? LY + 82 + i * IT_P : LY + LH - 14 - (6 - i) * IT_P + (IT_P - IT_H);
        const char *lab = i < 4 ? k_sections[i] : i == 4 ? "Reiniciar" : "Desligar";
        int ic = i < 4 ? k_sec_icon[i] : i == 4 ? IC_RESTART : IC_POWER;
        int focus = (i == sec && where == FOC_LEFT);
        int open = (i == sec && where == FOC_RIGHT);
        uint32_t fg = XUI_C_TEXT;
        if (focus) {
            xui_round_rect(&C, x - 3, y - 3, w + 6, IT_H + 6, 10, XUI_C_TEXT, 255);
            fg = XUI_C_BG;
        } else if (open) {
            xui_round_rect(&C, x, y, w, IT_H, 8, XUI_C_BUTTON, 255);
            xui_round_rect(&C, x, y + 10, 4, IT_H - 20, 2, XUI_C_ACCENT, 255);
        } else if (i >= 4) {
            fg = XUI_C_DIM;
        }
        icon(ic, x + 30, y + IT_H / 2, fg);
        xui_text(&C, XUI_F_ROW, x + 58, y + IT_H / 2 + 8, fg, lab, 0, w - 70);
    }
    /* separador da energia */
    int sy = LY + LH - 14 - 2 * IT_P - 18;
    xui_text(&C, XUI_F_LABEL, x + 4, sy, XUI_C_FAINT, "ENERGIA", 1, 200);
    xui_fill(&C, x + 4, sy - 26, w - 8, 1, XUI_C_BUTTON);
    /* painel da direita */
    xui_round_rect(&C, RX, RY, RW, RH, 14, XUI_C_PANEL, where == FOC_RIGHT ? 245 : 225);
}

static void title(const char *t, const char *sub) {
    xui_text(&C, XUI_F_H2, RX + RIN, RY + 52, XUI_C_TEXT, t, 0, RW - 2 * RIN);
    if (sub) xui_text(&C, XUI_F_BODY, RX + RIN, RY + 82, XUI_C_DIM, sub, 0, RW - 2 * RIN);
}

/* linha de opção; value à direita; arrow = chevron; dim = indisponível */
static void row(int y, const char *lab, const char *value, int focus, int arrow, int dim, uint32_t vcol) {
    int x = RX + RIN - 12, w = RW - 2 * RIN + 24;
    uint32_t fg = focus ? XUI_C_BG : dim ? XUI_C_FAINT : XUI_C_TEXT;
    if (focus) xui_round_rect(&C, x - 3, y - 3, w + 6, ROW_H + 6, 10, XUI_C_TEXT, 255);
    else xui_round_rect(&C, x, y, w, ROW_H, 8, XUI_C_BUTTON, dim ? 110 : 170);
    int base = y + ROW_H / 2 + 8;
    int right = x + w - 22;
    if (arrow) { chevron(right - 8, y + ROW_H / 2, fg); right -= 26; }
    if (value) {
        int vw = xui_text_width(XUI_F_BODY, value, 0);
        if (vw > 420) vw = 420;
        xui_text(&C, XUI_F_BODY, right - vw, base - 1, focus ? XUI_C_BG : (vcol ? vcol : XUI_C_DIM), value, 0, 420);
        right -= vw + 20;
    }
    xui_text(&C, XUI_F_ROW, x + 22, base, fg, lab, 0, right - x - 22);
}

static void info(int x, int y, const char *label, const char *big, int bigf, const char *small) {
    xui_text(&C, XUI_F_LABEL, x, y, XUI_C_FAINT, label, 1, 400);
    xui_text(&C, bigf, x, y + (bigf == XUI_F_TITLE ? 40 : 30), XUI_C_TEXT, big, bigf == XUI_F_TITLE ? 2 : 0, 520);
    if (small) xui_text(&C, XUI_F_LABEL, x, y + (bigf == XUI_F_TITLE ? 64 : 52), XUI_C_HAZE, small, 0, 520);
}

static void note_box(int y, const char *t) {
    int x = RX + RIN - 12, w = RW - 2 * RIN + 24;
    xui_round_rect(&C, x, y, w, 44, 8, XUI_C_BG, 150);
    xui_text(&C, XUI_F_BODY, x + 18, y + 28, XUI_C_DIM, t, 0, w - 36);
}

static void save(const char *name) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s.png", outdir, name);
    if (xui_png_write(p, px, W, H, W) != 0) { printf("FAIL png %s\n", p); fails++; }
    else printf("  %s\n", p);
}

#define VER "v0.21.28"

/* ---------------- telas ---------------- */
static void s_conta(int where, int focus, int update_avail) {
    frame_base(0, where, VER);
    title("Conta e sobre", NULL);
    int y = RY + 92;
    info(RX + RIN, y, "ID DO CONSOLE", "A1B2-C3D4-E5F6", XUI_F_TITLE, "a1b2c3d4e5f60718293a4b5c6d7e8f90");
    info(RX + RIN + 430, y, "VERSÃO", "orbita " VER, XUI_F_ROW, "beta fechada  ·  canal beta");
    y += 96;
    row(y, "Chave beta", "*****-**P9H  ·  validada em 25/10 22:56", where && focus == 0, 1, 0, 0);
    y += ROW_P;
    row(y, "Novidades desta versão", VER, where && focus == 1, 1, 0, 0);
    y += ROW_P;
    if (update_avail) {
        row(y, "Atualizar para v0.22.0", "nova versão disponível", where && focus == 2, 1, 0, XUI_C_FLARE);
        if (!(where && focus == 2)) xui_circle(&C, RX + RIN - 12 + 8, y + 10, 5, XUI_C_FLARE, 255);
    } else {
        row(y, "Procurar atualização", "você está na versão mais nova", where && focus == 2, 1, 0, 0);
    }
    y += ROW_P + 14;
    note_box(y, "Para reportar um bug, envie uma foto desta tela junto.");
    hints_right(where ? "A:Abrir|B:Voltar|Guia:Fechar" : "A:Entrar|LB/RB:Seção|B:Fechar");
}

static void s_novidades(void) {
    frame_base(0, FOC_RIGHT, VER);
    xui_text(&C, XUI_F_LABEL, RX + RIN, RY + 30, XUI_C_FAINT, "CONTA E SOBRE / NOVIDADES", 1, 500);
    xui_text(&C, XUI_F_H2, RX + RIN, RY + 66, XUI_C_TEXT, "Novidades da " VER, 0, RW - 2 * RIN);
    xui_text(&C, XUI_F_BODY, RX + RIN, RY + 96, XUI_C_DIM, "Instalada em 26/10/2026 pela atualização automática", 0, RW - 2 * RIN);
    static const char *items[] = {
        "Wi-Fi interno do Xbox 360 S/E: procure redes, conecte com senha WPA2 e o orbita lembra da rede.",
        "Decodificação de vídeo mais rápida: menos quadros perdidos em cenas com muito movimento.",
        "Saída HDMI em 1080p em teste para TVs que não aceitam 720p.",
        "Correções da beta: travamento ao gravar no pendrive com o Wi-Fi ligado, catálogo que demorava a abrir.",
    };
    int y = RY + 150;
    for (int i = 0; i < 4; i++) {
        xui_circle(&C, RX + RIN + 6, y - 6, 4, XUI_C_ACCENT, 255);
        int n = xui_text_wrap(&C, XUI_F_BODY, RX + RIN + 24, y, RW - 2 * RIN - 24, 0, 3, 0, XUI_C_TEXT, items[i]);
        y += n * xui_line_h(XUI_F_BODY) + 18;
    }
    xui_text(&C, XUI_F_LABEL, RX + RIN, RY + RH - 28, XUI_C_FAINT,
             "As notas vêm do manifesto assinado da atualização (guardadas em XC360/NOTES.TXT).", 0, RW - 2 * RIN);
    hints_right("B:Voltar|Guia:Fechar");
}

static void status_card(int y, int ok, int kind, const char *t, const char *sub, const char *sub2) {
    int x = RX + RIN - 12, w = RW - 2 * RIN + 24, h = sub2 ? 104 : 80;
    xui_round_rect(&C, x, y, w, h, 10, XUI_C_BG, 150);
    if (kind == 1) bars(x + 20, y + 42, 3, XUI_C_ACCENT, XUI_C_PH);
    else xui_circle(&C, x + 28, y + 30, 7, ok ? XUI_C_ACCENT : XUI_C_FAINT, 255);
    xui_text(&C, XUI_F_ROW, x + 56, y + 38, XUI_C_TEXT, t, 0, w - 80);
    if (sub) xui_text(&C, XUI_F_BODY, x + 56, y + 66, XUI_C_DIM, sub, 0, w - 80);
    if (sub2) xui_text(&C, XUI_F_BODY, x + 56, y + 90, XUI_C_FAINT, sub2, 0, w - 80);
}

static void s_rede_cabo(void) {
    frame_base(1, FOC_LEFT, VER);
    title("Rede", "Como o Xbox está ligado à internet");
    int y = RY + 116;
    status_card(y, 1, 0, "Cabo de rede  ·  conectado à internet", "IP na rede local: 192.168.15.47", NULL);
    y += 104;
    xui_text_wrap(&C, XUI_F_BODY, RX + RIN, y + 4, RW - 2 * RIN, 0, 3, 0, XUI_C_DIM,
                  "Com o cabo ligado, o orbita usa só o cabo e o Wi-Fi fica desligado. "
                  "Para usar o Wi-Fi, tire o cabo e reinicie o Xbox.");
    hints_right("A:Abrir|LB/RB:Seção|B:Fechar");
}

static void s_rede_wifi(void) {
    frame_base(1, FOC_RIGHT, VER);
    title("Rede", "Como o Xbox está ligado à internet");
    int y = RY + 116;
    status_card(y, 1, 1, "Wi-Fi: Casa do Sérgio", "Conectado  ·  sinal bom (-58 dBm)  ·  IP 192.168.15.77",
                "Cabo de rede: não conectado");
    y += 128;
    row(y, "Wi-Fi", "procurar, conectar ou esquecer", 1, 1, 0, 0);
    hints_right("A:Abrir|B:Voltar|Guia:Fechar");
}

/* Vídeo e Som (pedido de 2026-10-27): vídeo em cima, som embaixo.
 * focus: 0 = 720p em foco, 1 = Volume em foco */
static void s_tela(int focus) {
    frame_base(2, FOC_RIGHT, VER);
    title("Vídeo e Som", "Imagem enviada à TV e volume do som");
    int x = RX + RIN - 12, w = RW - 2 * RIN + 24;
    int y = RY + 112;
    xui_text(&C, XUI_F_LABEL, RX + RIN, y, XUI_C_FAINT, "VÍDEO", 1, 200);
    y += 12;
    /* 720p: escolhida */
    int f720 = focus == 0;
    uint32_t fg = f720 ? XUI_C_BG : XUI_C_TEXT;
    if (f720) xui_round_rect(&C, x - 3, y - 3, w + 6, ROW_H + 6, 10, XUI_C_TEXT, 255);
    else xui_round_rect(&C, x, y, w, ROW_H, 8, XUI_C_BUTTON, 170);
    xui_circle(&C, x + 30, y + ROW_H / 2, 11, XUI_C_ACCENT, 255);
    xui_circle(&C, x + 30, y + ROW_H / 2, 4, XUI_C_TEXT, 255);
    xui_text(&C, XUI_F_ROW, x + 56, y + ROW_H / 2 + 8, fg, "720p", 0, 200);
    xui_text(&C, XUI_F_BODY, x + 120, y + ROW_H / 2 + 7, f720 ? XUI_C_BG : XUI_C_DIM, "1280 × 720  ·  60 Hz", 0, 300);
    xui_text(&C, XUI_F_LABEL, x + w - 90, y + ROW_H / 2 + 6, f720 ? XUI_C_BG : XUI_C_HAZE, "EM USO", 1, 100);
    y += ROW_P;
    /* 1080p: em breve */
    xui_round_rect(&C, x, y, w, ROW_H, 8, XUI_C_BUTTON, 110);
    xui_round_frame(&C, x + 21, y + ROW_H / 2 - 9, 18, 18, 9, 2, XUI_C_FAINT);
    xui_text(&C, XUI_F_ROW, x + 56, y + ROW_H / 2 + 8, XUI_C_FAINT, "1080p", 0, 200);
    xui_text(&C, XUI_F_BODY, x + 128, y + ROW_H / 2 + 7, XUI_C_FAINT, "1920 × 1080", 0, 300);
    int tw = xui_text_width(XUI_F_LABEL, "EM BREVE", 1) + 18;
    xui_round_rect(&C, x + w - tw - 20, y + ROW_H / 2 - 13, tw, 24, 5, XUI_C_FLARE, 255);
    xui_text_center(&C, XUI_F_LABEL, x + w - tw - 20, tw, y + ROW_H / 2 + 5, XUI_C_TEXT, "EM BREVE", 1);
    y += ROW_H + 26;
    xui_text(&C, XUI_F_LABEL, RX + RIN, y, XUI_C_HAZE,
             "O 1080p chega numa próxima versão.  Saída atual: HDMI  ·  720p  ·  60 Hz", 0, RW - 2 * RIN);
    /* divisória */
    y += 22;
    xui_fill(&C, RX + RIN, y, RW - 2 * RIN, 1, XUI_C_BUTTON);
    y += 30;
    xui_text(&C, XUI_F_LABEL, RX + RIN, y, XUI_C_FAINT, "SOM", 1, 200);
    y += 12;
    /* Volume: 4 opções */
    int fv = focus == 1;
    if (fv) xui_round_rect(&C, x - 3, y - 3, w + 6, ROW_H + 6, 10, XUI_C_TEXT, 255);
    else xui_round_rect(&C, x, y, w, ROW_H, 8, XUI_C_BUTTON, 170);
    xui_text(&C, XUI_F_ROW, x + 22, y + ROW_H / 2 + 8, fv ? XUI_C_BG : XUI_C_TEXT, "Volume", 0, 200);
    static const char *opts[] = { "100%", "150%", "200%", "250%" };
    int sel = fv ? 1 : 0;   /* com foco: mostra 150% escolhido (exemplo) */
    int pw = 92, gap = 10, px0 = x + w - 22 - 4 * pw - 3 * gap;
    for (int i = 0; i < 4; i++) {
        int bx = px0 + i * (pw + gap), by = y + ROW_H / 2 - 17;
        if (i == sel) {
            xui_round_rect(&C, bx, by, pw, 34, 17, XUI_C_ACCENT, 255);
            xui_text_center(&C, XUI_F_BODY, bx, pw, by + 23, XUI_C_TEXT, opts[i], 0);
        } else {
            xui_round_rect(&C, bx, by, pw, 34, 17, fv ? XUI_C_DIM : XUI_C_BG, fv ? 90 : 150);
            xui_text_center(&C, XUI_F_BODY, bx, pw, by + 23, fv ? XUI_C_BG : XUI_C_DIM, opts[i], 0);
        }
    }
    y += ROW_H + 22;
    xui_text_wrap(&C, XUI_F_BODY, RX + RIN, y, RW - 2 * RIN, 0, 3, 0, XUI_C_DIM,
                  "Se o volume estiver baixo, aumente aqui (por exemplo, fone ligado na "
                  "saída de áudio e vídeo atrás do Xbox 360 S/E). Vale na hora.");
    hints_right(fv ? "Esq/Dir:Volume|B:Voltar|LB/RB:Seção|Guia:Fechar" : "B:Voltar|LB/RB:Seção|Guia:Fechar");
}

static void s_controles(void) {
    frame_base(3, FOC_RIGHT, VER);
    title("Controles", "Zona morta dos analógicos (vale para todos os controles)");
    int y = RY + 116;
    row(y, "Analógico esquerdo", "mínimo 37%  ·  máximo 95%", 1, 1, 0, 0);
    y += ROW_P;
    row(y, "Analógico direito", "mínimo 27%  ·  máximo 95%", 0, 1, 0, 0);
    y += ROW_P;
    row(y, "Restaurar padrão", NULL, 0, 0, 0, 0);
    y += ROW_P + 18;
    xui_text_wrap(&C, XUI_F_BODY, RX + RIN, y, RW - 2 * RIN, 0, 4, 0, XUI_C_DIM,
                  "Zona morta mínima: movimentos menores que isso são ignorados (tira o drift). "
                  "Limite máximo: daqui pra fora o jogo recebe o analógico inteiro.");
    hints_right("A:Calibrar|B:Voltar|LB/RB:Seção|Guia:Fechar");
}

/* calibração: stick 0/1, valores em %, posição bruta (-1..1), foco 0..3 */
static void slider(int y, const char *lab, int pct, int focus, uint32_t col) {
    int x = RX + RIN - 12 + 400, w = RW - 2 * RIN + 24 - 400;
    uint32_t fg = focus ? XUI_C_BG : XUI_C_TEXT;
    if (focus) xui_round_rect(&C, x - 3, y - 3, w + 6, 82 + 6, 10, XUI_C_TEXT, 255);
    else xui_round_rect(&C, x, y, w, 82, 8, XUI_C_BUTTON, 170);
    xui_text(&C, XUI_F_ROW, x + 20, y + 32, fg, lab, 0, w - 120);
    char v[16];
    snprintf(v, sizeof(v), "%d%%", pct);
    int vw = xui_text_width(XUI_F_ROW, v, 0);
    xui_text(&C, XUI_F_ROW, x + w - 20 - vw, y + 32, fg, v, 0, 0);
    int tx = x + 20, tww = w - 40, ty = y + 56;
    xui_round_rect(&C, tx, ty, tww, 6, 3, focus ? XUI_C_DIM : XUI_C_PH, 255);
    xui_round_rect(&C, tx, ty, tww * pct / 100, 6, 3, col, 255);
    xui_circle(&C, tx + tww * pct / 100, ty + 3, 9, focus ? XUI_C_BG : XUI_C_TEXT, 255);
    if (focus) {   /* setinhas: o D-pad ajusta */
        for (int i = 0; i < 5; i++) {
            xui_fill(&C, tx - 14 + i, ty + 3 - i, 1, 2 * i + 1, XUI_C_BG);
            xui_fill(&C, tx + tww + 14 - i, ty + 3 - i, 1, 2 * i + 1, XUI_C_BG);
        }
    }
}

static void s_calibrar(int stick, int dz, int mx, float rx, float ry, int focus) {
    frame_base(3, FOC_RIGHT, VER);
    char t[64];
    snprintf(t, sizeof(t), "Calibrar analógico %s", stick ? "direito" : "esquerdo");
    xui_text(&C, XUI_F_LABEL, RX + RIN, RY + 30, XUI_C_FAINT, "CONTROLES / CALIBRAR", 1, 500);
    xui_text(&C, XUI_F_H2, RX + RIN, RY + 66, XUI_C_TEXT, t, 0, RW - 2 * RIN);
    /* passo 1 de 2 */
    const char *stp = stick ? "2 de 2" : "1 de 2";
    int sw = xui_text_width(XUI_F_LABEL, stp, 0) + 20;
    xui_round_rect(&C, RX + RW - RIN - sw, RY + 44, sw, 26, 13, XUI_C_ACCENT, 255);
    xui_text_center(&C, XUI_F_LABEL, RX + RW - RIN - sw, sw, RY + 62, XUI_C_BG, stp, 0);
    /* visualização */
    int cx = RX + RIN + 175, cy = RY + 290, R = 160;
    xui_circle(&C, cx, cy, R + 2, XUI_C_BG, 200);
    xui_round_frame(&C, cx - R, cy - R, 2 * R, 2 * R, R, 2, XUI_C_FAINT);              /* curso todo */
    int rm = R * mx / 100;
    xui_circle(&C, cx, cy, rm, XUI_C_ACCENT, 40);                                     /* área útil */
    xui_round_frame(&C, cx - rm, cy - rm, 2 * rm, 2 * rm, rm, 2, XUI_C_ACCENT);       /* limite máximo */
    int rd = R * dz / 100;
    xui_circle(&C, cx, cy, rd, XUI_C_FLARE, 70);                                      /* zona morta */
    xui_round_frame(&C, cx - rd, cy - rd, 2 * rd, 2 * rd, rd, 2, XUI_C_FLARE);
    xui_fill_a(&C, cx - R, cy, 2 * R, 1, XUI_C_TEXT, 50);
    xui_fill_a(&C, cx, cy - R, 1, 2 * R, XUI_C_TEXT, 50);
    /* rastro do último segundo + posição */
    for (int i = 1; i <= 8; i++) {
        float k = 1.0f - i * 0.06f;
        xui_circle(&C, cx + (int)(rx * R * k), cy - (int)(ry * R * k * 0.92f), 4, XUI_C_TEXT, 120 - i * 12);
    }
    int dx = cx + (int)(rx * R), dy = cy - (int)(ry * R);
    xui_circle(&C, dx, dy, 11, XUI_C_BG, 255);
    xui_circle(&C, dx, dy, 9, XUI_C_TEXT, 255);
    /* leitura */
    float mag = sqrtf(rx * rx + ry * ry) * 100.0f;
    float out = mag <= dz ? 0 : mag >= mx ? 100 : (mag - dz) / (mx - dz) * 100.0f;
    char l1[64], l2[64];
    snprintf(l1, sizeof(l1), "Posição: %d%%", (int)(mag + 0.5f));
    snprintf(l2, sizeof(l2), "Jogo recebe: %d%%", (int)(out + 0.5f));
    xui_text(&C, XUI_F_BODY, cx - R, cy + R + 36, XUI_C_DIM, l1, 0, 200);
    xui_text(&C, XUI_F_BODY, cx + 10, cy + R + 36, mag <= dz ? XUI_C_FLARE : XUI_C_ACCENT, l2, 0, 200);
    /* legenda */
    xui_circle(&C, cx - R + 6, cy + R + 60, 5, XUI_C_FLARE, 255);
    xui_text(&C, XUI_F_LABEL, cx - R + 18, cy + R + 65, XUI_C_FAINT, "ignorado", 0, 100);
    xui_circle(&C, cx - R + 100, cy + R + 60, 5, XUI_C_ACCENT, 255);
    xui_text(&C, XUI_F_LABEL, cx - R + 112, cy + R + 65, XUI_C_FAINT, "vai pro jogo", 0, 120);
    /* opções */
    int y = RY + 104;
    slider(y, "Zona morta mínima", dz, focus == 0, XUI_C_FLARE);
    y += 98;
    slider(y, "Limite máximo", mx, focus == 1, XUI_C_ACCENT);
    y += 98;
    {
        int x = RX + RIN - 12 + 400, w = RW - 2 * RIN + 24 - 400;
        const char *b1 = "Medir sozinho";
        const char *b2 = stick ? "Concluir" : "Próximo: direito";
        for (int i = 0; i < 2; i++) {
            int f = focus == 2 + i, by = y + i * 62;
            const char *b = i ? b2 : b1;
            if (f) xui_round_rect(&C, x - 3, by - 3, w + 6, 52, 10, XUI_C_TEXT, 255);
            else xui_round_rect(&C, x, by, w, 46, 8, XUI_C_BUTTON, 255);
            xui_text_center(&C, XUI_F_ROW, x, w, by + 30, f ? XUI_C_BG : XUI_C_TEXT, b, 0);
        }
        xui_text_wrap(&C, XUI_F_LABEL, x, y + 146, w, 0, 3, 0, XUI_C_FAINT,
                      "Medir sozinho: solte o analógico por 2 s, depois gire-o encostando na borda.");
    }
    hints_right("D-pad:Ajustar|Y:Padrão|B:Salvar e voltar");
}

int main(int argc, char **argv) {
    const char *fxdir = argc > 1 ? argv[1] : "fixtures";
    snprintf(outdir, sizeof(outdir), "%s", argc > 2 ? argv[2] : "out");
    xui_canvas_init(&C, px, W, H, W);
    /* fundo: um hero das fixtures (o app usa o hero do jogo em foco, escurecido) */
    memset(bgpx, 0, sizeof(bgpx));
    {
        char p[700];
        snprintf(p, sizeof(p), "%s/hero/BT5P2X999VH2.jpg", fxdir);
        FILE *f = fopen(p, "rb");
        if (f) {
            static uint8_t buf[1 << 20];
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            uint32_t *hp = xcc_decode_jpeg(buf, n, 1280, 720);
            if (hp) {
                xui_image_t im = { hp, 1280, 720, 1280, XUI_FMT_UIRGB };
                xui_scale_cover(bgpx, W, H, &im);
                free(hp);
            }
        } else {
            for (int i = 0; i < W * H; i++) bgpx[i] = XUI_C_BG;
        }
    }
    s_conta(FOC_LEFT, 0, 0);            save("60_cfg_conta");
    s_conta(FOC_RIGHT, 2, 1);           save("61_cfg_conta_atualizacao");
    s_novidades();                      save("62_cfg_novidades");
    s_rede_cabo();                      save("63_cfg_rede_cabo");
    s_rede_wifi();                      save("64_cfg_rede_wifi");
    s_tela(0);                          save("65_cfg_video_som");
    s_tela(1);                          save("65b_cfg_video_som_volume");
    s_controles();                      save("66_cfg_controles");
    s_calibrar(0, 37, 95, 0.22f, 0.12f, 0);   save("67_cfg_calibrar_esquerdo");
    s_calibrar(1, 27, 90, 0.55f, 0.48f, 1);   save("68_cfg_calibrar_direito");
    printf("settings_v2: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
