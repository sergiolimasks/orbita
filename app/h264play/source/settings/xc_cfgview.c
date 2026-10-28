/*
 * orbita — o que as Configurações mostram (v0.21.33). Ver xc_cfgview.h.
 */
#include "xc_cfgview.h"
#include "../video/xc_video_mode.h"
#include "../update/xc_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int XCS_VOLUMES[XCS_NVOL] = { 100, 150, 200, 250 };
static const char *const k_vol_txt[XCS_NVOL] = { "100%", "150%", "200%", "250%" };
static const char *const k_sections[XCF_NSEC] = { "Conta e sobre", "Rede", "Vídeo e Som", "Controles" };

int xcs_vol_index(int pct) {
    for (int i = 0; i < XCS_NVOL; i++)
        if (XCS_VOLUMES[i] == pct) return i;
    return -1;
}

/* personalizado: vai pro primeiro maior (direita) / último menor (esquerda) */
int xcs_vol_step(int pct, int dir) {
    if (dir > 0) {
        for (int i = 0; i < XCS_NVOL; i++)
            if (XCS_VOLUMES[i] > pct) return XCS_VOLUMES[i];
        return XCS_VOLUMES[XCS_NVOL - 1] >= pct ? XCS_VOLUMES[XCS_NVOL - 1] : pct;
    }
    for (int i = XCS_NVOL - 1; i >= 0; i--)
        if (XCS_VOLUMES[i] < pct) return XCS_VOLUMES[i];
    return XCS_VOLUMES[0] <= pct ? XCS_VOLUMES[0] : pct;
}

const char *xcs_out_text(int mode) {
    switch (mode) {
    case XCV_HDMI_1080P: return "HDMI  ·  1080p  ·  60 Hz";
    case XCV_YUV_720P:   return "Componente  ·  720p  ·  60 Hz";
    case XCV_VGA_720P:   return "VGA  ·  1280 × 720";
    default:             return "HDMI  ·  720p  ·  60 Hz";
    }
}

static int res_of_mode(int mode) { return mode == XCV_HDMI_1080P ? 1080 : 720; }

/* resolução escolhida (o rádio marcado): a do VIDEO.TXT; no automático, a que está na TV */
static int res_selected(const xcs_in_t *in) {
    if (in->ovr == XCV_OVR_1080P) return 1080;
    if (in->ovr == XCV_OVR_720P) return 720;
    return res_of_mode(in->mode);
}

static int can_1080(const xcs_in_t *in) { return in->av_kind == XCV_AV_HDMI && in->have_1080p; }

int xcs_options(const xcs_in_t *in, int sec, int *ids, unsigned *adj) {
    int n = 0;
    *adj = 0;
    switch (sec) {
    case XCF_SEC_ACCOUNT:
        if (in->key_row) ids[n++] = XCS_OPT_KEY;
        ids[n++] = XCS_OPT_NOTES;
        ids[n++] = XCS_OPT_UPDATE;
        break;
    case XCF_SEC_NET:
        if (!in->cable && in->wifi != XCS_W_NONE) ids[n++] = XCS_OPT_WIFI;
        break;
    case XCF_SEC_AV:
        ids[n++] = XCS_OPT_720;
        if (can_1080(in)) ids[n++] = XCS_OPT_1080;
        if (in->audio) { *adj |= 1u << n; ids[n++] = XCS_OPT_VOLUME; }
        break;
    case XCF_SEC_PADS:
        ids[n++] = XCS_OPT_PAD_L;
        ids[n++] = XCS_OPT_PAD_R;
        ids[n++] = XCS_OPT_PAD_RESET;
        break;
    default:
        break;
    }
    return n;
}

int xcs_notes_paras(const char *p) {
    int n = 0, inpara = 0;
    for (; p && *p; p++) {
        if (*p == '\n') { inpara = 0; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r' || ((*p == '-' || *p == '*') && !inpara)) continue;
        if (!inpara) { n++; inpara = 1; }
    }
    return n;
}

static void ipstr(char *o, size_t n, uint32_t ip) {
    snprintf(o, n, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 8 & 255),
             (unsigned)(ip & 255));
}

static int wifi_bars(int rssi) { return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1; }
static const char *wifi_quality(int rssi) {
    return rssi >= -55 ? "ótimo" : rssi >= -65 ? "bom" : rssi >= -75 ? "fraco" : "muito fraco";
}

/* ---------------------------------------------------------------- seções */

static void sec_account(xcs_view_t *o, const xcs_in_t *in, int *nr) {
    xui_cfg_t *s = &o->cfg;
    s->title = "Conta e sobre";
    s->ninfo = 2;
    s->info[0].label = "ID DO CONSOLE";
    s->info[0].big = in->console_short ? in->console_short : "—";
    s->info[0].small = in->console_full;
    s->info[0].big_title = 1;
    snprintf(o->b_info1, sizeof(o->b_info1), "orbita %s", in->version ? in->version : "");
    s->info[1].label = "VERSÃO";
    s->info[1].big = o->b_info1;
    if (in->beta_build) {
        const char *ch = in->channel && in->channel[0] ? (strcmp(in->channel, "stable") ? "canal beta" : "canal estável")
                                                       : NULL;
        snprintf(o->b_info2, sizeof(o->b_info2), "beta fechada%s%s", ch ? "  ·  " : "", ch ? ch : "");
    } else {
        snprintf(o->b_info2, sizeof(o->b_info2), "versão pública");
    }
    s->info[1].small = o->b_info2;
    xui_cfg_row_t *r;
    if (in->key_row) {
        r = &o->rows[(*nr)++];
        r->label = "Chave beta";
        snprintf(o->b_key, sizeof(o->b_key), "%s%s%s", in->key_masked ? in->key_masked : "—",
                 in->key_when ? "  ·  " : "", in->key_when ? in->key_when : "");
        r->value = o->b_key;
        r->flags = XUI_CR_ARROW;
    }
    r = &o->rows[(*nr)++];
    r->label = "Novidades desta versão";
    r->value = in->version;
    r->flags = XUI_CR_ARROW;
    r = &o->rows[(*nr)++];
    r->flags = XUI_CR_ARROW;
    if (in->upd_avail && in->upd_latest && in->upd_latest[0]) {
        snprintf(o->b_upd, sizeof(o->b_upd), "Atualizar para v%.24s", in->upd_latest);
        r->label = o->b_upd;
        r->value = "nova versão disponível";
        r->flags |= XUI_CR_WARN;
    } else {
        r->label = "Procurar atualização";
        r->value = in->upd_busy || in->upd_phase == XCU_CHECKING ? "procurando…"
                 : in->upd_phase == XCU_UP_TO_DATE ? "você está na versão mais nova"
                 : in->upd_phase == XCU_DONE ? "instalada: reinicie o Xbox"
                 : in->upd_phase == XCU_FAILED ? "não deu pra procurar agora"
                 : NULL;
    }
    s->note = "Para reportar um bug, envie uma foto desta tela junto.";
}

static void sec_net(xcs_view_t *o, const xcs_in_t *in, int *nr) {
    xui_cfg_t *s = &o->cfg;
    s->title = "Rede";
    s->subtitle = "Como o Xbox está ligado à internet";
    char ip[24];
    if (in->cable) {
        s->card = 1;
        s->card_ok = 1;
        s->card_title = "Cabo de rede  ·  conectado à internet";
        ipstr(ip, sizeof(ip), in->cable_ip);
        snprintf(o->b_card_x, sizeof(o->b_card_x), "IP na rede local: %s", in->cable_ip ? ip : "—");
        s->card_text = o->b_card_x;
        s->text = "Com o cabo ligado, o orbita usa só o cabo e o Wi-Fi fica desligado. "
                  "Para usar o Wi-Fi, tire o cabo e reinicie o Xbox.";
        return;
    }
    s->card_text2 = "Cabo de rede: não conectado";
    if (in->wifi == XCS_W_NONE) {
        s->card = 1;
        s->card_title = "Sem Wi-Fi interno";
        s->card_text = "Este Xbox não tem Wi-Fi interno (só 360 S/E). Use o cabo de rede.";
        return;
    }
    s->card = 2;
    const char *ssid = in->wifi_ssid && in->wifi_ssid[0] ? in->wifi_ssid : NULL;
    if (ssid) snprintf(o->b_card_t, sizeof(o->b_card_t), "Wi-Fi: %.60s", ssid);
    else snprintf(o->b_card_t, sizeof(o->b_card_t), "Wi-Fi");
    s->card_title = o->b_card_t;
    switch (in->wifi) {
    case XCS_W_UP:
        s->card_bars = wifi_bars(in->wifi_rssi);
        s->card_ok = in->wifi_dhcp;
        if (in->wifi_dhcp) {
            ipstr(ip, sizeof(ip), in->wifi_ip);
            snprintf(o->b_card_x, sizeof(o->b_card_x), "Conectado  ·  sinal %s (%d dBm)  ·  IP %s",
                     wifi_quality(in->wifi_rssi), in->wifi_rssi, ip);
        } else {
            snprintf(o->b_card_x, sizeof(o->b_card_x), "Conectado, pedindo endereço IP…");
        }
        break;
    case XCS_W_CONNECTING: snprintf(o->b_card_x, sizeof(o->b_card_x), "Conectando…"); break;
    case XCS_W_STARTING: snprintf(o->b_card_x, sizeof(o->b_card_x), "Ligando o adaptador…"); break;
    case XCS_W_FAILED:
        snprintf(o->b_card_t, sizeof(o->b_card_t), "Wi-Fi com problema");
        snprintf(o->b_card_x, sizeof(o->b_card_x), "Reinicie o Xbox; se continuar, mande o log.");
        break;
    default: snprintf(o->b_card_x, sizeof(o->b_card_x), "Desconectado"); break;
    }
    s->card_text = o->b_card_x;
    xui_cfg_row_t *r = &o->rows[(*nr)++];
    r->label = "Wi-Fi";
    r->value = "procurar, conectar ou esquecer";
    r->flags = XUI_CR_ARROW;
}

static void sec_av(xcs_view_t *o, const xcs_in_t *in, int *nr) {
    xui_cfg_t *s = &o->cfg;
    s->title = "Vídeo e Som";
    s->subtitle = in->audio ? "Imagem enviada à TV e volume do som" : "Imagem enviada à TV";
    int sel = res_selected(in), use = res_of_mode(in->mode), ok1080 = can_1080(in);
    if (!ok1080) sel = 720;
    xui_cfg_row_t *r = &o->rows[(*nr)++];
    r->head = "VÍDEO";
    r->label = "720p";
    r->detail = "1280 × 720  ·  60 Hz";
    r->flags = XUI_CR_RADIO | (sel == 720 ? XUI_CR_ON : 0);
    r->tag = use == 720 ? "EM USO" : sel == 720 ? "AO REINICIAR" : NULL;
    r = &o->rows[(*nr)++];
    r->label = "1080p";
    r->detail = "1920 × 1080  ·  60 Hz";
    r->flags = XUI_CR_RADIO | (sel == 1080 ? XUI_CR_ON : 0) | (ok1080 ? 0 : XUI_CR_DIM);
    if (!ok1080)
        r->value = in->av_kind != XCV_AV_HDMI ? "só no cabo HDMI" : "indisponível neste console";
    else
        r->tag = use == 1080 ? "EM USO" : sel == 1080 ? "AO REINICIAR" : NULL;
    snprintf(o->b_after, sizeof(o->b_after), "Saída atual: %s%s\n%s", xcs_out_text(in->mode),
             sel != use ? "  ·  a troca vale ao reiniciar o Xbox" : "",
             in->forced_720p ? "LB+RB segurados ao ligar: 720p forçado desta vez."
             : in->av_kind == XCV_AV_HDMI ? "Se a TV ficar sem imagem, segure LB+RB ao ligar o Xbox: volta pra 720p."
             : "");
    r->after = o->b_after;
    if (!in->audio) return;
    r = &o->rows[(*nr)++];
    r->head = "SOM";
    r->label = "Volume";
    r->flags = XUI_CR_CHOICES;
    r->choices = k_vol_txt;
    r->nchoices = XCS_NVOL;
    r->choice = xcs_vol_index(in->volume);
    if (r->choice < 0) {
        snprintf(o->b_vol, sizeof(o->b_vol), "Agora: %d%%, personalizado (VOLUME.TXT)", in->volume);
        r->after = o->b_vol;
    }
    s->text = "Se o volume estiver baixo, aumente aqui (por exemplo, fone ligado na saída de áudio e vídeo "
              "atrás do Xbox 360 S/E). Vale na hora.";
}

static void sec_pads(xcs_view_t *o, const xcs_in_t *in, int *nr) {
    xui_cfg_t *s = &o->cfg;
    s->title = "Controles";
    s->subtitle = "Zona morta dos analógicos (vale para todos os controles)";
    xui_cfg_row_t *r = &o->rows[(*nr)++];
    r->label = "Analógico esquerdo";
    snprintf(o->b_lpad, sizeof(o->b_lpad), "mínimo %d%%  ·  máximo %d%%", xpc_pct(in->pad.dz[XPC_LEFT]),
             xpc_pct(in->pad.outer[XPC_LEFT]));
    r->value = o->b_lpad;
    r->flags = XUI_CR_ARROW;
    r = &o->rows[(*nr)++];
    r->label = "Analógico direito";
    snprintf(o->b_rpad, sizeof(o->b_rpad), "mínimo %d%%  ·  máximo %d%%", xpc_pct(in->pad.dz[XPC_RIGHT]),
             xpc_pct(in->pad.outer[XPC_RIGHT]));
    r->value = o->b_rpad;
    r->flags = XUI_CR_ARROW;
    r = &o->rows[(*nr)++];
    r->label = "Restaurar padrão";
    r->value = xpc_is_default(&in->pad) ? "em uso" : NULL;
    s->text = "Zona morta mínima: movimentos menores que isso são ignorados (tira o drift). "
              "Limite máximo: daqui pra fora o jogo recebe o analógico inteiro.";
}

/* ---------------------------------------------------------------- calibração */

void xcs_cal_begin(xcs_cal_t *k, const xpc_cfg_t *cur, int port) {
    memset(k, 0, sizeof(*k));
    k->stick = XPC_LEFT;   /* começa sempre pelo esquerdo ("1 de 2") */
    k->cfg = *cur;
    k->port = port;
}

static int measuring(const xcs_cal_t *k) { return k->meas.stage == XPC_M_REST || k->meas.stage == XPC_M_SPIN; }

int xcs_cal_key(xcs_cal_t *k, int key, uint32_t now_ms) {
    if (measuring(k)) {
        if (key == XCF_K_A || key == XCF_K_Y) {   /* cancela a medição (o Y ainda volta ao padrão abaixo) */
            k->meas.stage = XPC_M_IDLE;
            k->status[0] = 0;
            if (key == XCF_K_A) return XCS_CAL_REDRAW;
        } else {
            return XCS_CAL_NONE;
        }
    }
    switch (key) {
    case XCF_K_UP:
    case XCF_K_DOWN: {
        int f = k->focus + (key == XCF_K_UP ? -1 : 1);
        if (f < 0) f = 0;
        if (f > 3) f = 3;
        if (f == k->focus) return XCS_CAL_NONE;
        k->focus = f;
        return XCS_CAL_REDRAW;
    }
    case XCF_K_LEFT:
    case XCF_K_RIGHT:
        if (k->focus > 1) return XCS_CAL_NONE;
        if (!xpc_step(&k->cfg, k->stick, k->focus, key == XCF_K_LEFT ? -1 : 1)) return XCS_CAL_NONE;
        k->last_result = 0;
        k->status[0] = 0;
        return XCS_CAL_CHANGED;
    case XCF_K_Y: {
        xpc_cfg_t was = k->cfg;
        xpc_default_stick(&k->cfg, k->stick);
        k->last_result = 0;
        snprintf(k->status, sizeof(k->status), "Padrão: mínimo %d%%, máximo %d%%.", xpc_pct(k->cfg.dz[k->stick]),
                 xpc_pct(k->cfg.outer[k->stick]));
        return xpc_equal(&was, &k->cfg) ? XCS_CAL_REDRAW : XCS_CAL_CHANGED;
    }
    case XCF_K_A:
        if (k->focus == 2) {
            xpc_meas_start(&k->meas, now_ms);
            k->last_result = 0;
            snprintf(k->status, sizeof(k->status), "Solte o analógico…");
            return XCS_CAL_REDRAW;
        }
        if (k->focus == 3) {
            if (k->stick == XPC_LEFT) {   /* Próximo: direito */
                k->stick = XPC_RIGHT;
                k->focus = 0;
                k->ntrail = 0;
                k->last_result = 0;
                k->status[0] = 0;
                k->meas.stage = XPC_M_IDLE;
                return XCS_CAL_REDRAW;
            }
            return XCS_CAL_DONE;
        }
        return XCS_CAL_NONE;
    default:
        return XCS_CAL_NONE;
    }
}

int xcs_cal_feed(xcs_cal_t *k, uint32_t now_ms, int x, int y) {
    int mx = x, my = y;   /* a medição usa o valor cru */
    /* tela: menos de ~0,5 px no círculo (32767 = 160 px) não conta como mexer;
     * o ruído do analógico parado não redesenha a prévia a cada quadro */
    if (abs(x - k->x) < 100 && abs(y - k->y) < 100) { x = k->x; y = k->y; }
    if (now_ms - k->trail_ms >= 30 && (x != k->x || y != k->y || k->ntrail)) {
        k->trail_ms = now_ms;
        if (x == k->x && y == k->y) {
            if (k->ntrail) k->ntrail--;   /* parado: o rastro some */
        } else {
            int n = k->ntrail < XCS_CAL_TRAIL ? k->ntrail + 1 : XCS_CAL_TRAIL;
            for (int i = n - 1; i > 0; i--) { k->tx[i] = k->tx[i - 1]; k->ty[i] = k->ty[i - 1]; }
            k->tx[0] = k->x;
            k->ty[0] = k->y;
            k->ntrail = n;
        }
    }
    k->x = x;
    k->y = y;
    if (!measuring(k)) return XCS_CAL_NONE;
    int st = xpc_meas_feed(&k->meas, now_ms, mx, my, &k->cfg, k->stick);
    if (st == XPC_M_REST) {
        snprintf(k->status, sizeof(k->status), "Solte o analógico… %d s", xpc_meas_left_s(&k->meas, now_ms));
    } else if (st == XPC_M_SPIN) {
        snprintf(k->status, sizeof(k->status), "Gire o analógico encostando na borda… %d s (%d de %d setores)",
                 xpc_meas_left_s(&k->meas, now_ms), xpc_meas_sectors(&k->meas), XPC_M_SECTORS);
    } else if (st == XPC_M_DONE) {
        k->last_result = 1;
        if (k->meas.spun)
            snprintf(k->status, sizeof(k->status), "Medido: mínimo %d%%, máximo %d%%. Dá pra ajustar no D-pad.",
                     k->meas.dz_pct, k->meas.out_pct);
        else
            snprintf(k->status, sizeof(k->status), "Medido: mínimo %d%%. A borda não foi medida (gire encostando "
                     "na borda); o máximo ficou em %d%%.", k->meas.dz_pct, k->meas.out_pct);
        return XCS_CAL_CHANGED;
    }
    return XCS_CAL_NONE;
}

/* ---------------------------------------------------------------- tela */

static const char *hint_for(const xcs_in_t *in, const xcf_nav_t *nav, int opt_id, const xcs_cal_t *cal) {
    (void)in;
    if (nav->confirm) return "A:Selecionar|B:Cancelar";
    if (nav->page == XCF_PG_NOTES) return "B:Voltar|LB/RB:Seção|Guia:Fechar";
    if (nav->page == XCF_PG_CALIB) {
        if (cal && measuring(cal)) return "A:Cancelar|B:Salvar e voltar";
        return cal && cal->focus >= 2 ? "A:Selecionar|Y:Padrão|B:Salvar e voltar" : "D-pad:Ajustar|Y:Padrão|B:Salvar e voltar";
    }
    if (!nav->right) {
        if (nav->item >= XCF_NSEC) return "A:Selecionar|LB/RB:Seção|B:Fechar";
        return nav->nopt[nav->sec] > 0 ? "A:Selecionar|LB/RB:Seção|B:Fechar" : "LB/RB:Seção|B:Fechar";
    }
    if (opt_id == XCS_OPT_VOLUME) return "Esq/Dir:Volume|B:Voltar|LB/RB:Seção|Guia:Fechar";
    return "A:Selecionar|B:Voltar|LB/RB:Seção|Guia:Fechar";
}

void xcs_build(xcs_view_t *o, const xcs_in_t *in, const xcf_nav_t *nav, const xcs_cal_t *cal, int scroll,
               uint32_t now_ms) {
    (void)now_ms;
    memset(o, 0, sizeof(*o));
    xui_cfg_t *s = &o->cfg;
    for (int i = 0; i < XCF_NSEC; i++) s->sections[i] = k_sections[i];
    s->item = nav->right || nav->page != XCF_PG_MAIN ? nav->sec : nav->item;
    s->right = nav->right || nav->page != XCF_PG_MAIN;
    s->focus = -1;
    s->rows = o->rows;
    s->notice = in->notice;
    int ids[XCF_MAX_OPT];
    unsigned adj;
    int nopt = xcs_options(in, nav->sec, ids, &adj);
    int opt_id = nav->right && nav->focus[nav->sec] < nopt ? ids[nav->focus[nav->sec]] : XCS_OPT_NONE;
    if (nav->page == XCF_PG_NOTES) {
        s->page = XUI_CFG_NOTES;
        s->crumb = "CONTA E SOBRE / NOVIDADES";
        snprintf(o->b_title, sizeof(o->b_title), "Novidades da %s", in->version ? in->version : "versão");
        s->title = o->b_title;
        if (in->notes && in->notes[0]) {
            if (in->notes_from_file && in->notes_when)
                snprintf(o->b_sub, sizeof(o->b_sub), "Instalada em %s pela atualização do orbita", in->notes_when);
            else if (in->notes_from_file)
                snprintf(o->b_sub, sizeof(o->b_sub), "Instalada pela atualização do orbita");
            else
                snprintf(o->b_sub, sizeof(o->b_sub), "Notas da versão publicada");
            s->subtitle = o->b_sub;
        }
        s->notes = in->notes;
        s->notes_empty = "As notas desta versão não estão neste console. Elas aparecem na próxima atualização.";
        s->scroll = scroll;
        s->footer = "As notas vêm do manifesto assinado da atualização (guardadas em XC360/NOTES.TXT).";
    } else if (nav->page == XCF_PG_CALIB && cal) {
        s->page = XUI_CFG_CALIB;
        s->crumb = "CONTROLES / CALIBRAR";
        s->title = cal->stick == XPC_RIGHT ? "Calibrar analógico direito" : "Calibrar analógico esquerdo";
        xui_calib_t *k = &o->cal;
        k->stick = cal->stick;
        k->dz_pct = xpc_pct(cal->cfg.dz[cal->stick]);
        k->out_pct = xpc_pct(cal->cfg.outer[cal->stick]);
        k->x = cal->x;
        k->y = cal->y;
        k->ntrail = cal->ntrail;
        for (int i = 0; i < cal->ntrail; i++) { k->tx[i] = cal->tx[i]; k->ty[i] = cal->ty[i]; }
        k->pos_pct = xpc_mag_pct(cal->x, cal->y);
        k->game_pct = xpc_out_pct(cal->stick, cal->cfg.dz[cal->stick], cal->cfg.outer[cal->stick], cal->x, cal->y);
        k->focus = cal->focus;
        k->step = cal->stick == XPC_RIGHT ? "2 de 2" : "1 de 2";
        k->next = cal->stick == XPC_RIGHT ? "Concluir" : "Próximo: direito";
        k->measuring = measuring(cal);
        if (cal->status[0]) {
            snprintf(o->b_cal_status, sizeof(o->b_cal_status), "%s", cal->status);
            k->status = o->b_cal_status;
        }
        if (cal->multi) {
            snprintf(o->b_cal_pad, sizeof(o->b_cal_pad), "Controle %d", cal->port + 1);
            k->pad = o->b_cal_pad;
        }
        s->calib = k;
    } else {
        s->page = XUI_CFG_ROWS;
        int nr = 0;
        switch (nav->sec) {
        case XCF_SEC_ACCOUNT: sec_account(o, in, &nr); break;
        case XCF_SEC_NET: sec_net(o, in, &nr); break;
        case XCF_SEC_AV: sec_av(o, in, &nr); break;
        default: sec_pads(o, in, &nr); break;
        }
        s->nrows = nr;
        /* opção i -> linha (as apagadas não recebem foco) */
        int k = 0;
        for (int i = 0; i < nr && k < XCF_MAX_OPT; i++)
            if (!(o->rows[i].flags & XUI_CR_DIM)) o->opt_row[k++] = i;
        if (nav->right && nav->focus[nav->sec] < k) s->focus = o->opt_row[nav->focus[nav->sec]];
    }
    if (nav->confirm) {
        xui_confirm_t *c = &o->confirm;
        if (nav->confirm == XCF_CF_PADRESET) {
            c->title = "Restaurar o padrão?";
            xpc_cfg_t d;
            xpc_defaults(&d);
            snprintf(o->b_conf, sizeof(o->b_conf), "A zona morta e o limite máximo dos dois analógicos voltam ao "
                     "padrão (esquerdo %d%% / %d%%, direito %d%% / %d%%).", xpc_pct(d.dz[XPC_LEFT]),
                     xpc_pct(d.outer[XPC_LEFT]), xpc_pct(d.dz[XPC_RIGHT]), xpc_pct(d.outer[XPC_RIGHT]));
            c->text = o->b_conf;
            c->yes = "Restaurar";
        } else {
            int reboot = nav->confirm == XCF_CF_REBOOT;
            c->title = reboot ? "Reiniciar o Xbox?" : "Desligar o Xbox?";
            c->text = "Se houver uma sessão na nuvem aberta, ela é encerrada antes (até 3 s).";
            c->yes = reboot ? "Reiniciar" : "Desligar";
        }
        c->no = "Cancelar";
        c->focus = nav->confirm_yes;
        s->confirm = c;
    }
    s->hint = hint_for(in, nav, opt_id, cal);
}
