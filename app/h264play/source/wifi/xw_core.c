/* orbita v0.21.0 — driver do Wi-Fi interno (máquina de estados). Ver xw_core.h.
 * Sequência de boot/init e formatos derivados do mwifiex do Linux (GPL-2.0)
 * e do patch Free60Project/linux-xbox360-wifi; ver THIRD_PARTY.md. */
#include "xw_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PROBE_WAIT_MS   2500
#define PROBE_TRIES     3
#define BOOT_WAIT_MS    10000
#define CMD_TMO_MS      4000
#define ASSOC_TMO_MS    12000
#define AUTH_TMO_MS     7000
#define KEYS_DELAY_MS   30
#define SCAN_ACTIVE_MS  40
#define RETRY_MAX_MS    30000
#define AUTH_FAILS_MAX  3

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static void xlog(xw_core_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void xlog(xw_core_t *c, const char *fmt, ...) {
    if (!c->ops || !c->ops->log) return;
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(c->lb, sizeof(c->lb), "[wifi] ");
    vsnprintf(c->lb + n, sizeof(c->lb) - (size_t)n, fmt, ap);
    va_end(ap);
    c->ops->log(c->ops->u, c->lb);
}

const char *xw_err_text(int err) {
    switch (err) {
    case XW_E_NOTFOUND:    return "Rede não encontrada";
    case XW_E_UNSUPPORTED: return "Segurança desta rede não suportada (só aberta ou WPA2)";
    case XW_E_NEEDPASS:    return "Esta rede pede senha";
    case XW_E_CHANGED:     return "A rede salva agora está sem senha: esqueça e conecte de novo";
    case XW_E_ASSOC:       return "O roteador recusou a conexão";
    case XW_E_AUTH:        return "Senha incorreta: o roteador recusou a confirmação da senha";
    case XW_E_HANDSHAKE:   return "O handshake WPA2 não terminou";
    case XW_E_FWTIMEOUT:   return "O adaptador Wi-Fi demorou a responder (tentando de novo)";
    case XW_E_LOST:        return "A conexão caiu";
    case XW_E_DEVICE:      return "O adaptador Wi-Fi não respondeu";
    default:               return "";
    }
}

/* ------------------------------------------------------------ comandos */

/* v0.21.9: o relógio do core nunca anda pra trás */
static uint32_t clk(xw_core_t *c, uint32_t now) {
    if (!c->now_set || (int32_t)(now - c->now) > 0) c->now = now;
    c->now_set = 1;
    return c->now;
}

static void cmd_push(xw_core_t *c, size_t n, uint32_t tmo) {
    if (c->qn >= XW_QLEN || n > sizeof(c->q[0].b)) {
        xlog(c, "fila de comandos cheia, comando 0x%04x perdido", mwx_get16(c->cbuf + 4));
        return;
    }
    xw_qcmd_t *e = &c->q[(c->qh + c->qn) % XW_QLEN];
    memcpy(e->b, c->cbuf, n);
    e->n = (uint16_t)n;
    e->cmd = mwx_get16(c->cbuf + 4);
    e->tmo = tmo;
    c->qn++;
}

static void cmd_kick(xw_core_t *c) {
    if (c->busy || !c->qn) return;
    xw_qcmd_t *e = &c->q[c->qh];
    if (c->ops->send_cmd(c->ops->u, e->b, e->n) != 0) return;   /* tenta no próximo tick */
    c->busy = 1;
    c->busy_cmd = e->cmd;
    c->busy_tmo = e->tmo;
    c->t_sent = c->now;
    wipe(e->b, e->n);   /* chaves não ficam na fila */
    c->qh = (c->qh + 1) % XW_QLEN;
    c->qn--;
}

static void cmd_flush_queue(xw_core_t *c) { c->qn = 0; }

static uint16_t nseq(xw_core_t *c) { return ++c->seq; }

/* --------------------------------------------------------------- estado */

static void set_link(xw_core_t *c, int l) {
    c->link = l;
    c->t_link = c->now;
}

static void link_down(xw_core_t *c) {
    if (c->link == XW_LINK_UP && c->ops->link) c->ops->link(c->ops->u, 0);
    xw_sup_wipe(&c->sup);
    wipe(&c->keys, sizeof(c->keys));
    c->keys_pending = 0;
    c->keys_left = 0;
}

static void schedule_retry(xw_core_t *c) {
    set_link(c, c->want ? XW_LINK_WAIT : XW_LINK_IDLE);
    if (!c->want) return;
    uint32_t d = 3000u << (c->retry > 4 ? 4 : c->retry);
    if (d > RETRY_MAX_MS) d = RETRY_MAX_MS;
    c->retry++;
    c->t_retry = c->now + d;
    xlog(c, "nova tentativa em %u s", (unsigned)(d / 1000));
}

static void fail_link(xw_core_t *c, int err, int code) {
    c->err = err;
    c->err_code = code;
    if (err != XW_E_HANDSHAKE && err != XW_E_AUTH && err != XW_E_ASSOC && err != XW_E_FWTIMEOUT) c->err_detail[0] = 0;
    xlog(c, "falha: %s%s%s (código %d)", xw_err_text(err), c->err_detail[0] ? " — " : "", c->err_detail, code);
    link_down(c);
    if (err == XW_E_AUTH || err == XW_E_HANDSHAKE) {
        c->auth_fails++;
        if (c->user || c->auth_fails >= AUTH_FAILS_MAX) {
            xlog(c, "desisto de reconectar (handshake falhou %d vez(es))", c->auth_fails);
            c->want = 0;
        }
    }
    if (err == XW_E_UNSUPPORTED || err == XW_E_NEEDPASS || err == XW_E_CHANGED) c->want = 0;
    schedule_retry(c);
}

static void dev_fail(xw_core_t *c, const char *why) {
    xlog(c, "adaptador falhou: %s", why);
    c->dev = XW_DEV_FAILED;
    c->t_dev = c->now;
    c->err = XW_E_DEVICE;
    if (c->link == XW_LINK_UP) link_down(c);
    set_link(c, XW_LINK_IDLE);
    cmd_flush_queue(c);
    c->busy = 0;
    c->job = 0;
}

static void start_init(xw_core_t *c) {
    c->dev = XW_DEV_INIT;
    c->t_dev = c->now;
    cmd_push(c, mwx_build_func_init(c->cbuf, nseq(c)), CMD_TMO_MS);
    cmd_push(c, mwx_build_get_hw_spec(c->cbuf, nseq(c)), CMD_TMO_MS);
    cmd_push(c, mwx_build_mac_control(c->cbuf, nseq(c), MWX_MAC_RX_ON | MWX_MAC_TX_ON | MWX_MAC_ETH2), CMD_TMO_MS);
}

/* ------------------------------------------------------------------ scan */

static const uint8_t k_grp[6][4] = { { 1 }, { 2, 3, 4, 5 }, { 6 }, { 7, 8, 9, 10 }, { 11 }, { 12, 13 } };
static const int k_grp_n[6] = { 1, 4, 1, 4, 1, 2 };

static void start_user_scan(xw_core_t *c) {
    c->want_user_scan = 0;
    c->job = 1;
    c->ntmp = 0;
    c->job_left = 6;
    for (int i = 0; i < 6; i++)
        cmd_push(c, mwx_build_scan(c->cbuf, nseq(c), k_grp[i], k_grp_n[i], SCAN_ACTIVE_MS, NULL, 0),
                 CMD_TMO_MS + 200u * (unsigned)k_grp_n[i]);
    xlog(c, "procurando redes (13 canais)");
}

static void start_target_scan(xw_core_t *c) {
    static const uint8_t all[13] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    c->want_connect = 0;
    c->job = 2;
    c->ntmp = 0;
    c->job_left = 1;
    set_link(c, XW_LINK_SCAN);
    if (c->fast_chan >= 1 && c->fast_chan <= 13) {
        uint8_t one = (uint8_t)c->fast_chan;
        c->fast_chan = 0;
        cmd_push(c, mwx_build_scan(c->cbuf, nseq(c), &one, 1, SCAN_ACTIVE_MS, c->tssid, c->tssid_len), CMD_TMO_MS + 200u);
        xlog(c, "procurando \"%s\" só no canal %u (reconexão rápida)", c->tssid, (unsigned)one);
        return;
    }
    cmd_push(c, mwx_build_scan(c->cbuf, nseq(c), all, 13, SCAN_ACTIVE_MS, c->tssid, c->tssid_len),
             CMD_TMO_MS + 13u * 200u);
    xlog(c, "procurando \"%s\"", c->tssid);
}

static void scan_cb(void *u, const mwx_bss_t *b) {
    xw_core_t *c = (xw_core_t *)u;
    if (c->ntmp < XW_MAX_BSS) c->tmp[c->ntmp++] = *b;
}

/* lista do usuário: sem SSID oculto, um por SSID (o mais forte), por sinal */
static void scan_publish(xw_core_t *c) {
    int n = 0;
    mwx_bss_t *o = c->bss;
    for (int i = 0; i < c->ntmp; i++) {
        const mwx_bss_t *b = &c->tmp[i];
        if (!b->ssid_len || !b->ssid[0]) continue;
        int k;
        for (k = 0; k < n; k++)
            if (o[k].ssid_len == b->ssid_len && !memcmp(o[k].ssid, b->ssid, b->ssid_len)) break;
        if (k < n) {
            if (b->rssi > o[k].rssi) o[k] = *b;
        } else if (n < XW_MAX_BSS) {
            o[n++] = *b;
        }
    }
    for (int i = 1; i < n; i++) {   /* inserção: mais forte primeiro */
        mwx_bss_t t = o[i];
        int j = i - 1;
        while (j >= 0 && o[j].rssi < t.rssi) { o[j + 1] = o[j]; j--; }
        o[j + 1] = t;
    }
    c->nbss = n;
    c->scan_seq++;
    xlog(c, "scan: %d redes (%d respostas)", n, c->ntmp);
}

static void begin_assoc(xw_core_t *c);
static void on_eapol(xw_core_t *c, const uint8_t *p, size_t n);

static void target_chosen(xw_core_t *c) {
    const mwx_bss_t *best = NULL;
    for (int i = 0; i < c->ntmp; i++) {
        const mwx_bss_t *b = &c->tmp[i];
        if (b->ssid_len != c->tssid_len || memcmp(b->ssid, c->tssid, (size_t)c->tssid_len)) continue;
        if (!best || b->rssi > best->rssi) best = b;
    }
    if (best && c->avoid_on && !memcmp(best->bssid, c->avoid_bssid, 6)) {
        const mwx_bss_t *alt = NULL;
        for (int i = 0; i < c->ntmp; i++) {
            const mwx_bss_t *b = &c->tmp[i];
            if (b->ssid_len != c->tssid_len || memcmp(b->ssid, c->tssid, (size_t)c->tssid_len)) continue;
            if (!memcmp(b->bssid, c->avoid_bssid, 6)) continue;
            if (!alt || b->rssi > alt->rssi) alt = b;
        }
        if (alt) {
            xlog(c, "outro ponto de acesso da mesma rede (o anterior não deu DHCP)");
            best = alt;
        }
    }
    if (!best) {
        xlog(c, "\"%s\" não apareceu no scan", c->tssid);
        fail_link(c, XW_E_NOTFOUND, 0);
        return;
    }
    xlog(c, "\"%s\": canal %d, %d dBm, %s (grupo %d), BSSID %02x:%02x:%02x:..", c->tssid, best->channel,
         best->rssi, mwx_sec_name(best->sec), best->group_cipher, best->bssid[0], best->bssid[1], best->bssid[2]);
    if (best->sec == MWX_SEC_OPEN && c->tsecure) { fail_link(c, XW_E_CHANGED, 0); return; }
    if (best->sec == MWX_SEC_WPA2 && !c->tsecure) { fail_link(c, XW_E_NEEDPASS, 0); return; }
    if (best->sec != MWX_SEC_OPEN && best->sec != MWX_SEC_WPA2) { fail_link(c, XW_E_UNSUPPORTED, best->sec); return; }
    c->cur = *best;
    c->rssi = best->rssi;
    begin_assoc(c);
}

static void scan_job_step(xw_core_t *c) {
    if (--c->job_left > 0) return;
    int j = c->job;
    c->job = 0;
    if (j == 1) scan_publish(c);
    else if (j == 2 && c->link == XW_LINK_SCAN) target_chosen(c);
}

/* ---------------------------------------------------------- associação */

static void hs_summary(xw_core_t *c, const char *what) {
    snprintf(c->err_detail, sizeof(c->err_detail),
             "%s; msg1 recebida %u, msg2 enviada %u (saiu pelo USB %u), msg1 de novo depois %u, msg3 %u, MIC ruim %u",
             what, c->hs_msg1, c->hs_msg2_q, c->hs_tx_done, c->hs_msg1_after2, c->hs_msg3, c->hs_mic_bad);
}

/* a senha está errada? só com prova (ver xw_core.h) */
static int wrong_password_proven(const xw_core_t *c, int deauth_reason) {
    if (c->hs_mic_bad) return 1;
    if (!c->hs_tx_done) return 0;                    /* a nossa msg 2 nem saiu: não dá pra culpar a senha */
    if (deauth_reason == 14 || deauth_reason == 15) return 1;
    return c->hs_msg1_after2 > 0;                    /* o AP ignorou a msg 2 (MIC) e repetiu a msg 1 */
}

void xw_note_data_tx(xw_core_t *c, unsigned n) {
    if (!n) return;
    if (c->link == XW_LINK_AUTH || c->link == XW_LINK_KEYS) {
        unsigned before = c->hs_tx_done;
        c->hs_tx_done += n;
        if (before < 6) xlog(c, "EAPOL saiu pelo USB (EP2 OUT concluído, total %u)", c->hs_tx_done);
    }
}

static void begin_assoc(xw_core_t *c) {
    const uint8_t *ie = NULL;
    c->rsn_len = 0;
    if (c->tsecure) {
        c->rsn_len = mwx_build_rsn_ie(c->rsn_ie, c->cur.group_cipher);
        ie = c->rsn_ie;
        xw_sup_init(&c->sup, c->tpmk, c->cur.bssid, c->mac, c->rsn_ie, c->rsn_len, c->cur.group_cipher);
    }
    set_link(c, XW_LINK_ASSOC);
    c->hs_msg1 = c->hs_msg2_q = c->hs_tx_done = c->hs_msg1_after2 = c->hs_msg3 = c->hs_mic_bad = c->hs_msg4_q = 0;
    c->hs_fmt_bad = 0;
    c->rx_log = 0;
    c->err_detail[0] = 0;
    c->pend_len = 0;
    size_t an = mwx_build_assoc(c->cbuf, nseq(c), &c->cur, ie, c->rsn_len);
    cmd_push(c, an, ASSOC_TMO_MS);
    xlog(c, "associando (%s): canal %d, cap 0x%04x, %d taxas, IE RSN %u B (grupo %d), comando %u B",
         c->tsecure ? "WPA2-PSK" : "aberta", c->cur.channel, c->cur.cap, c->cur.nrates, (unsigned)c->rsn_len,
         c->cur.group_cipher, (unsigned)an);
}

static void go_up(xw_core_t *c) {
    set_link(c, XW_LINK_UP);
    c->err = XW_E_NONE;
    c->retry = 0;
    c->auth_fails = 0;
    c->user = 0;
    xlog(c, "conectado em \"%s\" (canal %d)", c->tssid, c->cur.channel);
    if (c->ops->link) c->ops->link(c->ops->u, 1);
    /* v0.21.10: economia de energia. Com PS o AP guarda os quadros até o
     * beacon/PS-poll (buracos de 0,1–1,5 s no RX). Lê o estado e desliga. */
    uint8_t ps[4];
    mwx_put16(ps, MWX_PS_GET);
    mwx_put16(ps + 2, MWX_PS_BITMAP_STA);
    c->ps_get_result = c->ps_dis_result = -1;
    cmd_push(c, mwx_build_cmd(c->cbuf, nseq(c), MWX_CMD_PS_MODE_ENH, ps, 4), CMD_TMO_MS);
    mwx_put16(ps, MWX_PS_DIS_AUTO);
    mwx_put16(ps + 2, MWX_PS_BITMAP_STA);
    cmd_push(c, mwx_build_cmd(c->cbuf, nseq(c), MWX_CMD_PS_MODE_ENH, ps, 4), CMD_TMO_MS);
}

int xw_fw_query(xw_core_t *c) {
    if (c->dev != XW_DEV_READY || c->link != XW_LINK_UP || c->busy || c->qn) return 0;
    int n = 0;
    if (!(c->fwq_unsup & 1)) { cmd_push(c, mwx_build_cmd(c->cbuf, nseq(c), MWX_CMD_GET_LOG, NULL, 76), CMD_TMO_MS); n++; }
    if (!(c->fwq_unsup & 2)) {
        uint8_t r[6];
        mwx_put16(r, 0);       /* GET */
        mwx_put16(r + 2, 8);   /* médias de 8 quadros de dados / beacons */
        mwx_put16(r + 4, 8);
        cmd_push(c, mwx_build_cmd(c->cbuf, nseq(c), MWX_CMD_RSSI_INFO, r, 6), CMD_TMO_MS);
        n++;
    }
    if (!(c->fwq_unsup & 4)) { cmd_push(c, mwx_build_cmd(c->cbuf, nseq(c), MWX_CMD_TX_RATE_QUERY, NULL, 2), CMD_TMO_MS); n++; }
    cmd_kick(c);
    return n > 0;
}

/* taxa legado do mwifiex (índice -> 500 kbps) */
static unsigned rate_x2(int idx) {
    static const uint8_t t[13] = { 0x02, 0x04, 0x0b, 0x16, 0x00, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };
    return idx >= 0 && idx < 13 ? t[idx] : 0;
}

void xw_fwstats_text(const xw_core_t *c, char *o, size_t n) {
    char a[200] = "GET_LOG -", b[96] = "RSSI -", r[64] = "taxa -";
    if (c->fws_n >= 12) {
        const uint32_t *f = c->fws;
        int k = snprintf(a, sizeof(a), "tx %u falhas %u retry %u multi %u dup %u ack_falha %u rx_frag %u fcs %u",
                         (unsigned)f[11], (unsigned)f[1], (unsigned)f[2], (unsigned)f[3], (unsigned)f[4], (unsigned)f[7],
                         (unsigned)f[8], (unsigned)f[10]);
        if (c->fws_n >= 19 && k > 0 && (size_t)k < sizeof(a))
            snprintf(a + k, sizeof(a) - (size_t)k, " beacons %u perdidos %u", (unsigned)f[17], (unsigned)f[18]);
    } else if (c->fwq_unsup & 1) {
        snprintf(a, sizeof(a), "GET_LOG recusado");
    }
    if (c->rssi_ok)
        snprintf(b, sizeof(b), "RSSI dados %d/%d beacon %d/%d dBm, ruído %d, SNR %d", c->rssi_data_last, c->rssi_data_avg,
                 c->rssi_bcn_last, c->rssi_bcn_avg, c->nf_bcn_last, c->snr_bcn_last);
    else if (c->fwq_unsup & 2)
        snprintf(b, sizeof(b), "RSSI_INFO recusado");
    if (c->rate_ok) {
        unsigned x2 = c->tx_rate_ht ? 0 : rate_x2(c->tx_rate_idx);
        if (x2) snprintf(r, sizeof(r), "taxa TX %u%s Mb/s (índice %d)", x2 / 2, (x2 & 1) ? ",5" : "", c->tx_rate_idx);
        else snprintf(r, sizeof(r), "taxa TX índice %d ht 0x%02x", c->tx_rate_idx, c->tx_rate_ht);
    } else if (c->fwq_unsup & 4) {
        snprintf(r, sizeof(r), "TX_RATE recusado");
    }
    snprintf(o, n, "%s; %s; %s", a, b, r);
}

static void push_key(xw_core_t *c, int unicast) {
    size_t n;
    int type = unicast ? MWX_KEY_AES : (c->cur.group_cipher == 2 ? MWX_KEY_TKIP : MWX_KEY_AES);
    const uint8_t *k = unicast ? c->keys.tk : c->keys.gtk;
    size_t kl = unicast ? 16 : c->keys.gtk_len;
    if (c->key_v2) {
        static const uint8_t bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        n = mwx_build_key_v2(c->cbuf, nseq(c), type, k, kl, unicast, unicast ? c->cur.bssid : bc,
                             unicast ? 0 : c->keys.gtk_idx, unicast ? NULL : c->keys.gtk_rsc);
    } else {
        n = mwx_build_key_v1(c->cbuf, nseq(c), type, k, kl, unicast);
    }
    cmd_push(c, n, CMD_TMO_MS);
    wipe(c->cbuf, n);   /* a cópia da fila é apagada quando sai (cmd_kick) */
}

/* ------------------------------------------------------------- respostas */

static void on_resp(xw_core_t *c, const mwx_resp_t *r) {
    switch (r->cmd) {
    case MWX_CMD_PS_MODE_ENH: {
        uint16_t act = r->body_len >= 2 ? mwx_get16(r->body) : 0xffff;
        uint16_t bm = r->body_len >= 4 ? mwx_get16(r->body + 2) : 0;
        if (act == MWX_PS_GET || c->ps_get_result < 0) {
            c->ps_get_result = r->result;
            c->ps_bitmap = bm;
            xlog(c, "economia de energia: estado lido (resultado %u, bitmap 0x%04x: %s)", r->result, bm,
                 r->result ? "o firmware não respondeu ao GET" : (bm & MWX_PS_BITMAP_STA) ? "PS LIGADO" : "PS desligado");
        } else {
            c->ps_dis_result = r->result;
            xlog(c, "economia de energia: desligar (DIS_AUTO_PS) -> resultado %u%s", r->result,
                 r->result ? " (recusado: o firmware fica como estava)" : " (desligada)");
        }
        break;
    }
    case MWX_CMD_GET_LOG:
        if (r->result) { c->fwq_unsup |= 1; c->fws_n = 0; xlog(c, "GET_LOG recusado (resultado %u): sem estatísticas do ar", r->result); break; }
        c->fws_n = 0;
        for (size_t i = 0; i + 4 <= r->body_len && c->fws_n < 19; i += 4) c->fws[c->fws_n++] = mwx_get32(r->body + i);
        c->fws_seq++;
        break;
    case MWX_CMD_RSSI_INFO:
        if (r->result || r->body_len < 26) {
            c->fwq_unsup |= 2;
            c->rssi_ok = 0;
            xlog(c, "RSSI_INFO recusado (resultado %u, %u B)", r->result, (unsigned)r->body_len);
            break;
        }
        c->rssi_data_last = (int16_t)mwx_get16(r->body + 6);
        c->rssi_data_avg = (int16_t)mwx_get16(r->body + 10);
        c->snr_bcn_last = (int16_t)mwx_get16(r->body + 14);
        c->nf_bcn_last = (int16_t)mwx_get16(r->body + 18);
        c->rssi_bcn_last = (int16_t)mwx_get16(r->body + 20);
        c->rssi_bcn_avg = (int16_t)mwx_get16(r->body + 24);
        c->rssi_ok = 1;
        if (c->rssi_bcn_last) c->rssi = c->rssi_bcn_last;
        break;
    case MWX_CMD_TX_RATE_QUERY:
        if (r->result || r->body_len < 2) { c->fwq_unsup |= 4; c->rate_ok = 0; xlog(c, "TX_RATE_QUERY recusado (resultado %u)", r->result); break; }
        c->tx_rate_idx = r->body[0];
        c->tx_rate_ht = r->body[1];
        c->rate_ok = 1;
        break;
    case MWX_CMD_FUNC_INIT:
        if (r->result) xlog(c, "FUNC_INIT resultado %u (seguindo)", r->result);
        break;
    case MWX_CMD_GET_HW_SPEC: {
        mwx_hw_spec_t hs;
        if (r->result || mwx_parse_hw_spec(r->body, r->body_len, &hs) != 0) {
            dev_fail(c, "GET_HW_SPEC");
            return;
        }
        memcpy(c->mac, hs.mac, 6);
        c->fw_release = hs.fw_release;
        c->key_v2 = hs.key_api_major == 2;
        xlog(c, "firmware %u.%u.%u.p%u, região 0x%02x, cap 0x%08x (supplicant no firmware: %s, não usado), API de chave %d.%d",
             (unsigned)(hs.fw_release >> 24) & 255, (unsigned)(hs.fw_release >> 16) & 255,
             (unsigned)(hs.fw_release >> 8) & 255, (unsigned)hs.fw_release & 255, hs.region,
             (unsigned)hs.fw_cap, (hs.fw_cap & (1u << 21)) ? "sim" : "não",
             hs.key_api_major ? hs.key_api_major : 1, hs.key_api_minor);
        break;
    }
    case MWX_CMD_MAC_CONTROL:
        if (r->result) xlog(c, "MAC_CONTROL resultado %u", r->result);
        if (c->dev == XW_DEV_INIT) {
            c->dev = XW_DEV_READY;
            c->t_dev = c->now;
            xlog(c, "adaptador pronto");
        }
        break;
    case MWX_CMD_SCAN:
        if (r->result) xlog(c, "SCAN resultado %u", r->result);
        else mwx_parse_scan_rsp(r->body, r->body_len, scan_cb, c);
        if (c->job) scan_job_step(c);
        break;
    case MWX_CMD_ASSOCIATE: {
        if (c->link != XW_LINK_ASSOC) break;
        uint16_t st = 0xffff, cap = 0, aid = 0;
        if (r->result || mwx_parse_assoc_rsp(r->body, r->body_len, &st, &cap, &aid) != 0 || st != 0) {
            xlog(c, "associação recusada: resultado %u, status %u, cap 0x%04x", r->result, st, cap);
            c->assoc_status = st;
            snprintf(c->err_detail, sizeof(c->err_detail), "resultado %u, status 802.11 %u", r->result, st);
            fail_link(c, XW_E_ASSOC, st);
            break;
        }
        xlog(c, "associado (aid %u, status 0, cap 0x%04x, %u B de resposta)", aid, cap, (unsigned)r->body_len);
        if (!c->tsecure) go_up(c);
        else {
            set_link(c, XW_LINK_AUTH);
            xlog(c, "esperando o handshake WPA2");
            if (c->pend_len) {   /* a msg 1 veio pelo EP2 antes desta resposta pelo EP1 */
                size_t pn = c->pend_len;
                c->pend_len = 0;
                xlog(c, "processando o EAPOL que chegou antes da resposta da associação");
                on_eapol(c, c->pend_eapol, pn);
            }
        }
        break;
    }
    case MWX_CMD_KEY_MATERIAL:
        if (r->result) {
            xlog(c, "firmware recusou a chave (resultado %u)", r->result);
            if (c->link == XW_LINK_KEYS) {
                snprintf(c->err_detail, sizeof(c->err_detail), "KEY_MATERIAL resultado %u", r->result);
                fail_link(c, XW_E_DEVICE, r->result);
            }
            break;
        }
        xlog(c, "chave instalada no firmware (%s)", c->link == XW_LINK_KEYS ? (c->keys_left == 2 ? "par" : "grupo") : "grupo, troca");
        if (c->link == XW_LINK_KEYS && --c->keys_left <= 0) {
            wipe(&c->keys, sizeof(c->keys));
            go_up(c);
        }
        break;
    case MWX_CMD_DEAUTH:
        break;
    default:
        break;
    }
}

static void on_cmd_timeout(xw_core_t *c, uint16_t cmd) {
    xlog(c, "comando 0x%04x sem resposta em %u ms", cmd, (unsigned)c->busy_tmo);
    if (c->dev == XW_DEV_INIT) { dev_fail(c, "init sem resposta"); return; }
    if (cmd == MWX_CMD_GET_LOG) c->fwq_unsup |= 1;
    else if (cmd == MWX_CMD_RSSI_INFO) c->fwq_unsup |= 2;
    else if (cmd == MWX_CMD_TX_RATE_QUERY) c->fwq_unsup |= 4;
    if (cmd == MWX_CMD_SCAN && c->job) scan_job_step(c);
    /* v0.21.9: tempo esgotado não é recusa do roteador (essa vem com status 802.11) */
    else if (cmd == MWX_CMD_ASSOCIATE && c->link == XW_LINK_ASSOC) {
        snprintf(c->err_detail, sizeof(c->err_detail), "o firmware não respondeu ao ASSOCIATE");
        fail_link(c, XW_E_FWTIMEOUT, -1);
    } else if (cmd == MWX_CMD_KEY_MATERIAL && c->link == XW_LINK_KEYS) {
        snprintf(c->err_detail, sizeof(c->err_detail), "o firmware não respondeu à instalação da chave");
        fail_link(c, XW_E_FWTIMEOUT, -1);
    }
}

void xw_ev_summary(const xw_core_t *c, char *o, size_t n) {
    size_t w = 0;
    o[0] = 0;
    for (int i = 0; i < c->ev_n && w < n; i++) {
        int r = snprintf(o + w, n - w, "%s0x%04x x%u", i ? " " : "", c->ev_id[i], (unsigned)c->ev_cnt[i]);
        if (r < 0) break;
        w += (size_t)r;
    }
    if (!c->ev_n) snprintf(o, n, "nenhum");
}

static void ev_note(xw_core_t *c, uint32_t evt, size_t n) {
    c->ev_total++;
    int i;
    for (i = 0; i < c->ev_n; i++) if (c->ev_id[i] == (uint16_t)evt) break;
    if (i == c->ev_n) {
        if (c->ev_n >= 16) i = 15;   /* tabela cheia: o último junta o resto */
        else { c->ev_n++; c->ev_id[i] = (uint16_t)evt; c->ev_cnt[i] = 0; }
    }
    c->ev_cnt[i]++;
    if (c->ev_cnt[i] <= 3)
        xlog(c, "evento do firmware 0x%04x (%u B de corpo, %u.º deste tipo, estado %d)", (unsigned)evt, (unsigned)n,
             (unsigned)c->ev_cnt[i], c->link);
}

static void on_event(xw_core_t *c, uint32_t evt, const uint8_t *body, size_t n) {
    uint16_t reason = n >= 2 ? mwx_get16(body) : 0;
    ev_note(c, evt, n);
    switch (evt) {
    case MWX_EVT_INIT_DONE:
        if (c->dev == XW_DEV_BOOT) {
            xlog(c, "firmware da flash no ar (INIT_DONE)");
            start_init(c);
        }
        break;
    case MWX_EVT_LINK_LOST:
    case MWX_EVT_DEAUTH:
    case MWX_EVT_DISASSOC:
        if (c->link >= XW_LINK_ASSOC && c->link <= XW_LINK_UP) {
            xlog(c, "evento %s (motivo %u) em %s", evt == MWX_EVT_LINK_LOST ? "LINK_LOST" :
                 evt == MWX_EVT_DEAUTH ? "DEAUTH" : "DISASSOC", reason,
                 c->link == XW_LINK_UP ? "conectado" : "conexão");
            if (c->link == XW_LINK_AUTH || c->link == XW_LINK_KEYS) {
                int wrong = wrong_password_proven(c, reason);
                char w[48];
                snprintf(w, sizeof(w), "o roteador desconectou (motivo %u)", reason);
                hs_summary(c, w);
                fail_link(c, wrong ? XW_E_AUTH : XW_E_HANDSHAKE, reason);
            } else {
                fail_link(c, XW_E_LOST, reason);
            }
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- dados */

/* só pros testes do host: 1 = TxPD sempre de 20 (o comportamento até a v0.21.7) */
int xw_tx_nopad_test = 0;

static int tx_frame(xw_core_t *c, const uint8_t *f, size_t n) {
    if (n > sizeof(c->txbuf) - MWX_TXPD_LEN - MWX_TXPD_MAXPAD) return -1;
    /* v0.21.8: nunca múltiplo de 64 (sem ZLP no EP2 OUT): pad entre o TxPD e o
     * quadro, o mesmo mecanismo do pad de alinhamento do mwifiex */
    size_t off = xw_tx_nopad_test ? MWX_TXPD_LEN : mwx_txpd_offset(n);
    if (off != MWX_TXPD_LEN) c->tx_padded++;
    mwx_build_txpd_off(c->txbuf, n, off);
    memcpy(c->txbuf + off, f, n);
    if (c->ops->send_data(c->ops->u, c->txbuf, off + n) != 0) { c->tx_drop++; return -1; }
    c->tx_frames++;
    return 0;
}

static int send_eapol(xw_core_t *c, const uint8_t *p, size_t n) {
    uint8_t f[14 + sizeof(((xw_sup_out_t *)0)->reply)];
    memcpy(f, c->cur.bssid, 6);
    memcpy(f + 6, c->mac, 6);
    f[12] = 0x88; f[13] = 0x8e;
    memcpy(f + 14, p, n);
    return tx_frame(c, f, 14 + n);
}

static void on_eapol(xw_core_t *c, const uint8_t *p, size_t n) {
    c->eapol_rx++;
    uint16_t ki = n >= 7 ? (uint16_t)((p[5] << 8) | p[6]) : 0;
    if (c->tsecure && c->link == XW_LINK_ASSOC && n <= sizeof(c->pend_eapol)) {
        memcpy(c->pend_eapol, p, n);
        c->pend_len = n;
        xlog(c, "EAPOL (key info 0x%04x) chegou antes da resposta do ASSOCIATE: guardado", ki);
        return;
    }
    if (!c->tsecure || (c->link != XW_LINK_AUTH && c->link != XW_LINK_UP && c->link != XW_LINK_KEYS)) {
        xlog(c, "EAPOL ignorado: estado %d (key info 0x%04x, %u B)", c->link, ki, (unsigned)n);
        return;
    }
    int is_msg1 = (ki & XW_KI_PAIRWISE) && (ki & XW_KI_ACK) && !(ki & XW_KI_MIC);
    int is_msg3 = (ki & XW_KI_PAIRWISE) && (ki & XW_KI_ACK) && (ki & XW_KI_MIC);
    if (is_msg1) {
        c->hs_msg1++;
        if (c->hs_tx_done) c->hs_msg1_after2++;
    }
    if (is_msg3) c->hs_msg3++;
    xlog(c, "EAPOL recebido: versão %u, tipo %u, descritor %u, key info 0x%04x (%s), %u B", n > 0 ? p[0] : 0,
         n > 1 ? p[1] : 0, n > 4 ? p[4] : 0, ki, is_msg1 ? "msg 1/4" : is_msg3 ? "msg 3/4" : "grupo/outro", (unsigned)n);
    xw_sup_out_t o;
    int r = xw_sup_rx(&c->sup, p, n, &o);
    switch (r) {
    case XW_SUP_MSG2:
        c->hs_msg2_q++;
        xlog(c, "EAPOL 1/4 -> 2/4 (%u B na fila do USB, envio %s)", (unsigned)o.reply_len,
             send_eapol(c, o.reply, o.reply_len) == 0 ? "ok" : "RECUSADO pela fila");
        break;
    case XW_SUP_MSG4:
        xlog(c, "EAPOL 3/4 -> 4/4 (grupo %s, índice %d)", o.gtk_len == 32 ? "TKIP" : "CCMP", o.gtk_idx);
        c->hs_msg4_q++;
        if (send_eapol(c, o.reply, o.reply_len) != 0) xlog(c, "msg 4/4 RECUSADA pela fila do USB");
        c->keys = o;
        c->keys_pending = 1;
        c->t_keys = c->now + KEYS_DELAY_MS;   /* a msg 4 sai sem cifra antes da PTK */
        set_link(c, XW_LINK_KEYS);
        break;
    case XW_SUP_GROUP2:
        xlog(c, "EAPOL grupo 1/2 -> 2/2 (troca da GTK, índice %d)", o.gtk_idx);
        send_eapol(c, o.reply, o.reply_len);
        c->keys = o;
        push_key(c, 0);
        break;
    case XW_SUP_ERR_MIC:
        c->hs_mic_bad++;
        xlog(c, "EAPOL: MIC da msg 3 não confere (a senha/PMK não bate com a do roteador)");
        if (c->link == XW_LINK_AUTH) {
            hs_summary(c, "MIC da msg 3 não confere");
            cmd_push(c, mwx_build_deauth(c->cbuf, nseq(c), c->cur.bssid, 2), CMD_TMO_MS);
            fail_link(c, XW_E_AUTH, 0);
        }
        break;
    case XW_SUP_IGNORED:
        break;
    default:
        c->hs_fmt_bad++;
        xlog(c, "EAPOL recusado (%d: %s)", r, r == XW_SUP_ERR_FMT ? "formato/versão do descritor" :
             r == XW_SUP_ERR_REPLAY ? "replay" : r == XW_SUP_ERR_RAND ? "sem números aleatórios" : "?");
        break;
    }
    wipe(&o, sizeof(o));
}

void xw_on_data_in(xw_core_t *c, uint8_t *b, size_t n, uint32_t now) {
    clk(c, now);
    uint8_t *f;
    size_t fl;
    int rssi;
    if (c->link != XW_LINK_UP && c->rx_log < 24 && n >= 8) {
        c->rx_log++;
        unsigned off = mwx_get16(b + 4), plen = mwx_get16(b + 2), ty = mwx_get16(b + 6);
        unsigned et = 0;
        if (off + 14 <= n) {
            const uint8_t *q = b + off;
            et = (off + 22 <= n && q[14] == 0xaa && q[15] == 0xaa) ? (unsigned)((q[20] << 8) | q[21])
                                                                    : (unsigned)((q[12] << 8) | q[13]);
        }
        xlog(c, "dados RX %u B (estado %d): RxPD tipo 0x%04x, offset %u, tamanho %u, ethertype 0x%04x", (unsigned)n,
             c->link, ty, off, plen, et);
    }
    if (mwx_rx_to_eth(b, n, &f, &fl, &rssi) != 0) { c->rx_drop++; return; }
    if (rssi < 0 && rssi > -110) c->rssi = rssi;
    if (fl >= 14 && f[12] == 0x88 && f[13] == 0x8e) { on_eapol(c, f + 14, fl - 14); return; }
    if (c->link != XW_LINK_UP) { c->rx_drop++; return; }
    c->rx_frames++;
    if (c->ops->rx_eth) c->ops->rx_eth(c->ops->u, f, fl);
}

int xw_tx_eth(xw_core_t *c, const uint8_t *f, size_t n) {
    if (c->link != XW_LINK_UP) { c->tx_drop++; return -1; }
    return tx_frame(c, f, n);
}

/* ------------------------------------------------------------- entradas */

void xw_init(xw_core_t *c, const xw_ops_t *ops) {
    memset(c, 0, sizeof(*c));
    c->ops = ops;
    c->sup.rand = ops->rand;
    c->sup.rand_u = ops->u;
}

void xw_start(xw_core_t *c, uint32_t now) {
    clk(c, now);
    c->dev = XW_DEV_PROBE;
    c->t_dev = now;
    c->probe_tries = 1;
    c->busy = 0;
    c->qn = 0;
    size_t n = mwx_build_probe(c->cbuf);
    c->ops->send_cmd(c->ops->u, c->cbuf, n);
    xlog(c, "adaptador 045e:0765: perguntando se o firmware já está rodando");
}

void xw_on_cmd_in(xw_core_t *c, const uint8_t *b, size_t n, uint32_t now) {
    clk(c, now);
    if (c->dev == XW_DEV_PROBE) {
        int k = mwx_probe_classify(b, n);
        if (k == 1) {
            xlog(c, "adaptador em modo ROM: mandando \"MRVL\" (firmware da flash)");
            size_t m = mwx_build_mrvl(c->cbuf);
            c->ops->send_cmd(c->ops->u, c->cbuf, m);
            c->dev = XW_DEV_BOOT;
            c->t_dev = now;
        } else if (k == 0) {
            xlog(c, "firmware já estava rodando");
            start_init(c);
        }
        if (k != 0) return;
        /* o próprio pacote pode ser a resposta do probe: nada a fazer */
        return;
    }
    mwx_resp_t r;
    uint32_t evt = 0;
    const uint8_t *eb = NULL;
    size_t el = 0;
    int k = mwx_parse_cmd_ep(b, n, &r, &evt, &eb, &el);
    if (k == 2) { on_event(c, evt, eb, el); return; }
    if (k != 1) return;
    if (!c->busy || r.cmd != (c->busy_cmd & 0x7fff)) {   /* atrasada / do probe */
        if (c->dev == XW_DEV_READY) {
            c->cmd_late++;
            xlog(c, "resposta atrasada do comando 0x%04x (resultado %u): ignorada", r.cmd, r.result);
        }
        return;
    }
    c->busy = 0;
    {
        uint32_t ms = c->now - c->t_sent;
        c->cmd_total++;
        if (ms > c->cmd_worst_ms) { c->cmd_worst_ms = ms; c->cmd_worst = r.cmd; }
        if (ms >= 1000) c->cmd_slow++;
        /* depois de pronto: todo comando (menos a varredura) com o tempo */
        int periodic = r.cmd == MWX_CMD_GET_LOG || r.cmd == MWX_CMD_RSSI_INFO || r.cmd == MWX_CMD_TX_RATE_QUERY;
        if (c->dev == XW_DEV_READY && r.cmd != MWX_CMD_SCAN && (!periodic || ms >= 200))
            xlog(c, "comando 0x%04x: resposta em %u ms (resultado %u)", r.cmd, (unsigned)ms, r.result);
    }
    on_resp(c, &r);
    cmd_kick(c);
}

void xw_tick(xw_core_t *c, uint32_t now) {
    now = clk(c, now);
    switch (c->dev) {
    case XW_DEV_PROBE:
        if (now - c->t_dev > PROBE_WAIT_MS) {
            if (c->probe_tries >= PROBE_TRIES) { dev_fail(c, "sem resposta ao probe"); return; }
            c->probe_tries++;
            c->t_dev = now;
            size_t n = mwx_build_probe(c->cbuf);
            c->ops->send_cmd(c->ops->u, c->cbuf, n);
            xlog(c, "probe sem resposta, tentativa %d", c->probe_tries);
        }
        return;
    case XW_DEV_BOOT:
        if (now - c->t_dev > BOOT_WAIT_MS) dev_fail(c, "sem INIT_DONE depois do MRVL");
        return;
    case XW_DEV_OFF:
    case XW_DEV_FAILED:
        return;
    default:
        break;
    }
    if (c->busy && now - c->t_sent > c->busy_tmo) {
        uint16_t cmd = c->busy_cmd;
        c->busy = 0;
        on_cmd_timeout(c, cmd);
        if (c->dev == XW_DEV_FAILED) return;
    }
    if (c->dev == XW_DEV_READY) {
        if (c->keys_pending && (int32_t)(now - c->t_keys) >= 0) {
            c->keys_pending = 0;
            c->keys_left = 2;
            push_key(c, 1);
            push_key(c, 0);
        }
        if (c->link == XW_LINK_AUTH && now - c->t_link > AUTH_TMO_MS) {
            int wrong = wrong_password_proven(c, 0);
            hs_summary(c, c->hs_msg1 ? "o roteador parou de responder" : "a msg 1 do roteador nunca chegou");
            xlog(c, "handshake WPA2 não terminou em %u ms (%u quadros EAPOL): %s", (unsigned)AUTH_TMO_MS,
                 (unsigned)c->eapol_rx, c->err_detail);
            cmd_push(c, mwx_build_deauth(c->cbuf, nseq(c), c->cur.bssid, 3), CMD_TMO_MS);
            fail_link(c, wrong ? XW_E_AUTH : XW_E_HANDSHAKE, 0);
        }
        if (c->link == XW_LINK_WAIT && c->want && (int32_t)(now - c->t_retry) >= 0) c->want_connect = 1;
        int quiet = !c->job && c->link != XW_LINK_ASSOC && c->link != XW_LINK_AUTH && c->link != XW_LINK_KEYS &&
                    c->link != XW_LINK_SCAN;
        if (quiet && c->want_connect && c->want) {
            c->eapol_rx = 0;
            start_target_scan(c);
        } else if (quiet && c->want_user_scan) {
            start_user_scan(c);
        }
    }
    cmd_kick(c);
}

/* -------------------------------------------------------------- pedidos */

int xw_scan(xw_core_t *c) {
    if (c->dev == XW_DEV_FAILED || c->dev == XW_DEV_OFF) return -1;
    if (c->job == 1) return 0;
    c->want_user_scan = 1;
    return 0;
}

int xw_connect(xw_core_t *c, const char *ssid, int ssid_len, const uint8_t *pmk, int user) {
    if (ssid_len < 1 || ssid_len > MWX_SSID_MAX) return -1;
    if (c->link >= XW_LINK_ASSOC && c->link <= XW_LINK_UP) {
        cmd_push(c, mwx_build_deauth(c->cbuf, nseq(c), c->cur.bssid, 3), CMD_TMO_MS);
        link_down(c);
    }
    memcpy(c->tssid, ssid, (size_t)ssid_len);
    c->tssid[ssid_len] = 0;
    c->tssid_len = ssid_len;
    c->tsecure = pmk != NULL;
    if (pmk) memcpy(c->tpmk, pmk, 32);
    else wipe(c->tpmk, 32);
    c->want = 1;
    c->user = user;
    c->retry = 0;
    c->auth_fails = 0;
    c->err = XW_E_NONE;
    c->want_connect = 1;
    set_link(c, XW_LINK_IDLE);
    return 0;
}

int xw_reconnect(xw_core_t *c) {
    if (c->tssid_len < 1) return -1;
    char ssid[MWX_SSID_MAX + 1];
    uint8_t pmk[32];
    int sl = c->tssid_len, sec = c->tsecure, ch = c->cur.channel;
    memcpy(ssid, c->tssid, sizeof(ssid));
    if (sec) memcpy(pmk, c->tpmk, sizeof(pmk));
    int r = xw_connect(c, ssid, sl, sec ? pmk : NULL, 0);
    wipe(pmk, sizeof(pmk));
    c->fast_chan = ch;
    return r;
}

void xw_avoid_current(xw_core_t *c) {
    memcpy(c->avoid_bssid, c->cur.bssid, 6);
    c->avoid_on = 1;
}

void xw_disconnect(xw_core_t *c) {
    c->want = 0;
    c->want_connect = 0;
    if (c->link >= XW_LINK_ASSOC && c->link <= XW_LINK_UP && c->dev == XW_DEV_READY) {
        cmd_push(c, mwx_build_deauth(c->cbuf, nseq(c), c->cur.bssid, 3), CMD_TMO_MS);
        xlog(c, "desconectando de \"%s\"", c->tssid);
    }
    link_down(c);
    wipe(c->tpmk, 32);
    c->tsecure = 0;
    c->tssid[0] = 0;
    c->tssid_len = 0;
    c->err = XW_E_NONE;
    set_link(c, XW_LINK_IDLE);
}
