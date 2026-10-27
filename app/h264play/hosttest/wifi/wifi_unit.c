/*
 * orbita v0.21.0 — testes do host do Wi-Fi (source/wifi): protocolo do
 * firmware, cripto WPA2 (vetores do 802.11 / RFC 3394) e o driver inteiro
 * contra um firmware falso (ROM -> MRVL -> INIT_DONE, comandos, scan, assoc)
 * com um AP WPA2 falso (autenticador do 4-way e do rekey de grupo).
 * Uso: wifi_unit            (tudo)
 *      wifi_unit ptk <pmk> <aa> <spa> <anonce> <snonce>  (imprime a PTK, pro run.sh comparar com Python)
 *      wifi_unit pmk <senha> <ssid>
 */
#include "mwx_proto.h"
#include "xw_core.h"
#include "xw_wpa.h"
#include "xw_store.h"
#include "xc_keypad.h"
#include "xw_boot.h"
#include "xc_wdog.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int unhex(const char *h, uint8_t *o, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return -1;
        o[i] = (uint8_t)v;
    }
    return 0;
}
static void hex(const uint8_t *b, size_t n, char *o) {
    for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", b[i]);
}
static int eqhex(const uint8_t *b, size_t n, const char *h) {
    char t[512];
    hex(b, n, t);
    return strcmp(t, h) == 0;
}

/* ================================================================ cripto */

static void test_crypto(void) {
    uint8_t pmk[32];
    /* IEEE 802.11-2016 J.4.2 */
    CHECK(xw_pmk_from_passphrase("password", (const uint8_t *)"IEEE", 4, pmk) == 0, "pmk");
    CHECK(eqhex(pmk, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"), "PMK password/IEEE");
    CHECK(xw_pmk_from_passphrase("ThisIsAPassword", (const uint8_t *)"ThisIsASSID", 11, pmk) == 0, "pmk2");
    CHECK(eqhex(pmk, 32, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"), "PMK ThisIsAPassword");
    CHECK(xw_pmk_from_passphrase("short", (const uint8_t *)"x", 1, pmk) != 0, "senha curta recusada");
    /* PRF (802.11 J.3.2, caso 1) */
    uint8_t k[20], out[64];
    memset(k, 0x0b, 20);
    xw_prf_sha1(k, 20, "prefix", (const uint8_t *)"Hi There", 8, out, 64);
    CHECK(eqhex(out, 64, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                         "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"), "PRF-512");
    /* RFC 3394 4.1 */
    uint8_t kek[16], pt[16], ct[24], back[16];
    unhex("000102030405060708090a0b0c0d0e0f", kek, 16);
    unhex("00112233445566778899aabbccddeeff", pt, 16);
    CHECK(xw_aes_wrap(kek, pt, 16, ct) == 0, "wrap");
    CHECK(eqhex(ct, 24, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"), "RFC3394 wrap");
    CHECK(xw_aes_unwrap(kek, ct, 24, back) == 0 && !memcmp(back, pt, 16), "RFC3394 unwrap");
    ct[3] ^= 1;
    CHECK(xw_aes_unwrap(kek, ct, 24, back) != 0, "unwrap adulterado recusado");
}

/* ============================================================ protocolo */

static size_t ie(uint8_t *p, uint8_t id, const void *d, size_t n) { p[0] = id; p[1] = (uint8_t)n; memcpy(p + 2, d, n); return 2 + n; }

/* um BSS no formato da resposta de scan legada */
static size_t bss_entry(uint8_t *p, const uint8_t bssid[6], int rssi, uint16_t cap, const char *ssid, int chan,
                        const uint8_t *rsn, size_t rsnl, int wpa1) {
    uint8_t *s = p + 2;
    size_t o = 0;
    memcpy(s, bssid, 6); o += 6;
    s[o++] = (uint8_t)(-rssi);
    memset(s + o, 0, 8); o += 8;
    mwx_put16(s + o, 100); o += 2;
    mwx_put16(s + o, cap); o += 2;
    o += ie(s + o, 0, ssid, strlen(ssid));
    static const uint8_t rates[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    o += ie(s + o, 1, rates, 8);
    uint8_t ch = (uint8_t)chan;
    o += ie(s + o, 3, &ch, 1);
    if (rsn) o += ie(s + o, 48, rsn, rsnl);
    if (wpa1) { static const uint8_t w[8] = { 0, 0x50, 0xf2, 1, 1, 0, 0, 0x50 }; o += ie(s + o, 221, w, 8); }
    mwx_put16(p, (uint16_t)o);
    return 2 + o;
}

static const uint8_t RSN_PSK_CCMP[] = { 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
static const uint8_t RSN_PSK_TKIPGRP[] = { 1, 0, 0, 0x0f, 0xac, 2, 2, 0, 0, 0x0f, 0xac, 4, 0, 0x0f, 0xac, 2, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
static const uint8_t RSN_SAE[] = { 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 8, 0xc0, 0 };
static const uint8_t RSN_MIXED_PSK_SAE[] = { 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 8, 0x80, 0 };
static const uint8_t RSN_ENT[] = { 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 1, 0, 0 };
static const uint8_t RSN_PSK_PMF_REQ[] = { 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0xc0, 0 };

static mwx_bss_t g_seen[16];
static int g_nseen;
static void seen_cb(void *u, const mwx_bss_t *b) { (void)u; if (g_nseen < 16) g_seen[g_nseen++] = *b; }

static void test_proto(void) {
    uint8_t b[2048];
    CHECK(mwx_build_probe(b) == 12 && eqhex(b, 12, "cefa0df0a900080001000000"), "probe");
    CHECK(mwx_build_mrvl(b) == 16 && !memcmp(b, "MRVL\x01", 5), "mrvl");
    uint8_t rom[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(mwx_probe_classify(rom, 8) == 1, "rom");
    uint8_t run[12];
    mwx_put32(run, MWX_USB_TYPE_CMD);
    CHECK(mwx_probe_classify(run, 12) == 0, "running");
    size_t n = mwx_build_get_hw_spec(b, 7);
    CHECK(n == 12 + 63 && mwx_get16(b + 4) == 3 && mwx_get16(b + 6) == 8 + 63 && mwx_get16(b + 8) == 7, "hw_spec");
    n = mwx_build_mac_control(b, 1, 0x13);
    CHECK(n == 16 && mwx_get32(b + 12) == 0x13, "mac_control");
    /* scan coringa 4 canais */
    uint8_t ch[4] = { 2, 3, 4, 5 };
    n = mwx_build_scan(b, 2, ch, 4, 40, NULL, 0);
    CHECK(b[12] == 3, "bss_mode any");
    CHECK(mwx_get16(b + 19) == 0x0112 && mwx_get16(b + 21) == 1 && b[23] == 32, "wildcard ssid tlv");
    CHECK(mwx_get16(b + 24) == 0x0101 && mwx_get16(b + 26) == 28 && b[29] == 2 && b[30] == 2 && mwx_get16(b + 31) == 40,
          "chanlist tlv");
    CHECK(mwx_get16(b + 56) == 0x0001 && mwx_get16(b + 58) == 12 && n == 60 + 12, "rates tlv (n=%zu)", n);
    CHECK(mwx_get16(b + 6) == n - 4, "scan size");
    /* scan específico */
    n = mwx_build_scan(b, 3, ch, 1, 40, "Casa", 4);
    CHECK(mwx_get16(b + 21) == 5 && b[23] == 0 && !memcmp(b + 24, "Casa", 4), "ssid específico");

    /* resposta de scan: classificação */
    uint8_t rsp[2048], *p = rsp + 3;
    const uint8_t bs[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
    size_t t = 0;
    t += bss_entry(p + t, bs, -40, 0x0411, "WPA2casa", 6, RSN_PSK_CCMP, sizeof(RSN_PSK_CCMP), 0);
    t += bss_entry(p + t, bs, -70, 0x0401, "Aberta", 1, NULL, 0, 0);
    t += bss_entry(p + t, bs, -60, 0x0411, "SAEonly", 11, RSN_SAE, sizeof(RSN_SAE), 0);
    t += bss_entry(p + t, bs, -61, 0x0411, "Misto", 11, RSN_MIXED_PSK_SAE, sizeof(RSN_MIXED_PSK_SAE), 0);
    t += bss_entry(p + t, bs, -62, 0x0411, "Empresa", 1, RSN_ENT, sizeof(RSN_ENT), 0);
    t += bss_entry(p + t, bs, -63, 0x0411, "Velha", 1, NULL, 0, 0);
    t += bss_entry(p + t, bs, -64, 0x0411, "WPA1", 1, NULL, 0, 1);
    t += bss_entry(p + t, bs, -65, 0x0411, "TKIPgrp", 1, RSN_PSK_TKIPGRP, sizeof(RSN_PSK_TKIPGRP), 0);
    t += bss_entry(p + t, bs, -66, 0x0411, "PMFreq", 1, RSN_PSK_PMF_REQ, sizeof(RSN_PSK_PMF_REQ), 0);
    mwx_put16(rsp, (uint16_t)t);
    rsp[2] = 9;
    g_nseen = 0;
    CHECK(mwx_parse_scan_rsp(rsp, 3 + t + 10, seen_cb, NULL) == 9, "9 BSS");
    CHECK(g_seen[0].sec == MWX_SEC_WPA2 && g_seen[0].group_cipher == 4 && g_seen[0].rssi == -40 && g_seen[0].channel == 6 &&
          !strcmp(g_seen[0].ssid, "WPA2casa"), "wpa2");
    CHECK(g_seen[1].sec == MWX_SEC_OPEN, "aberta");
    CHECK(g_seen[2].sec == MWX_SEC_WPA3, "sae");
    CHECK(g_seen[3].sec == MWX_SEC_WPA2, "transição WPA2/WPA3 = WPA2 (sem PMF obrigatório)");
    CHECK(g_seen[4].sec == MWX_SEC_ENTERPRISE, "802.1X");
    CHECK(g_seen[5].sec == MWX_SEC_WEP, "wep");
    CHECK(g_seen[6].sec == MWX_SEC_WPA1, "wpa1");
    CHECK(g_seen[7].sec == MWX_SEC_WPA2 && g_seen[7].group_cipher == 2, "grupo TKIP");
    CHECK(g_seen[8].sec == MWX_SEC_WPA3, "PMF obrigatório");
    /* scan truncado não explode */
    g_nseen = 0;
    mwx_parse_scan_rsp(rsp, 40, seen_cb, NULL);
    CHECK(g_nseen == 0, "truncado");

    /* associação */
    uint8_t rie[24];
    size_t rl = mwx_build_rsn_ie(rie, 4);
    CHECK(rl == 22 && rie[0] == 0x30 && rie[1] == 20, "rsn ie");
    n = mwx_build_assoc(b, 9, &g_seen[0], rie, rl);
    CHECK(!memcmp(b + 12, bs, 6) && mwx_get16(b + 18) == (0x0411 & 0xfff) && mwx_get16(b + 20) == 10, "assoc fixos");
    CHECK(mwx_get16(b + n - 24) == 0x30 && mwx_get16(b + n - 22) == 20, "assoc RSN TLV no fim");
    /* chaves */
    uint8_t key[32];
    memset(key, 0x5a, 32);
    n = mwx_build_key_v1(b, 1, MWX_KEY_AES, key, 16, 1);
    CHECK(n == 12 + 2 + 10 + 16 && mwx_get16(b + 14) == 0x0100 && mwx_get16(b + 16) == 22 && mwx_get16(b + 18) == 2 &&
          mwx_get16(b + 20) == 6 && mwx_get16(b + 22) == 16, "key v1 par");
    n = mwx_build_key_v1(b, 1, MWX_KEY_TKIP, key, 32, 0);
    CHECK(mwx_get16(b + 18) == 1 && mwx_get16(b + 20) == 5 && mwx_get16(b + 22) == 32, "key v1 grupo TKIP");
    static const uint8_t bc[6] = { 255, 255, 255, 255, 255, 255 };
    n = mwx_build_key_v2(b, 1, MWX_KEY_AES, key, 16, 0, bc, 2, NULL);
    CHECK(n == 12 + 2 + 4 + 52 && mwx_get16(b + 14) == 0x019c && b[24] == 2 && b[25] == 2 && mwx_get16(b + 36) == 16, "key v2");

    /* RxPD 802.3+SNAP -> Ethernet II */
    uint8_t rx[200];
    memset(rx, 0, sizeof(rx));
    uint8_t *pk = rx + 19 + 3;
    mwx_put16(rx + 2, 22 + 4);
    mwx_put16(rx + 4, 22);
    memcpy(pk, "\x01\x02\x03\x04\x05\x06\xa1\xa2\xa3\xa4\xa5\xa6\x00\x0c\xaa\xaa\x03\x00\x00\x00\x08\x00", 22);
    memcpy(pk + 22, "DATA", 4);
    uint8_t *f;
    size_t fl;
    int rs;
    CHECK(mwx_rx_to_eth(rx, 22 + 26, &f, &fl, &rs) == 0 && fl == 18 &&
          !memcmp(f, "\x01\x02\x03\x04\x05\x06\xa1\xa2\xa3\xa4\xa5\xa6\x08\x00" "DATA", 18), "snap -> eth2");
    mwx_put16(rx + 6, 0xe5);
    CHECK(mwx_rx_to_eth(rx, 22 + 26, &f, &fl, &rs) != 0, "gerência descartada");

    /* v0.21.6: firmware do Xbox marca Ethernet II (EAPOL) como RxPD tipo 2 */
    memset(rx, 0, sizeof(rx));
    mwx_put16(rx + 2, 14 + 4);
    mwx_put16(rx + 4, 26);
    mwx_put16(rx + 6, 2);
    memcpy(rx + 26, "\x01\x02\x03\x04\x05\x06\xa1\xa2\xa3\xa4\xa5\xa6\x88\x8e" "EAPL", 18);
    CHECK(mwx_rx_to_eth(rx, 26 + 18, &f, &fl, &rs) == 0 && fl == 18 && f[12] == 0x88 && f[13] == 0x8e,
          "RxPD tipo 2 (Ethernet II) aceito");
}

/* ========================================== firmware + AP WPA2 falsos */

typedef struct {
    int rom;                 /* começa na ROM */
    int booted;
    uint8_t mac[6];
    uint8_t q[64][2100];     /* fila do EP1 IN */
    size_t qn_len[64];
    uint32_t q_at[64];      /* v0.21.9: quando a resposta fica pronta (latência do firmware) */
    uint32_t resp_delay;
    int qh, qn;
    uint8_t dq[64][2100];    /* fila do EP2 IN */
    size_t dq_len[64];
    int dh, dn;
    /* rede */
    char ap_ssid[33];
    uint8_t ap_bssid[6];
    int ap_secure;
    uint8_t ap_pmk[32];
    uint8_t group_cipher;
    int assoc;
    /* autenticador */
    uint8_t anonce[32], snonce[32], ptk[48], gtk[32];
    int gtk_len, gtk_idx;
    uint64_t replay;
    int ap_state;            /* 0 nada, 1 mandou msg1, 2 mandou msg3, 3 pronto, 4 rekey enviado */
    int msg2_bad_mic;
    /* chaves instaladas no "firmware" */
    uint8_t inst_tk[16];
    uint8_t inst_gtk[32];
    int inst_tk_ok, inst_gtk_ok, inst_gtk_len;
    int key_v2, no_init_done;
    /* v0.21.4: AP de verdade repete a msg 1 e derruba com motivo 15; USB pode "engolir" o envio */
    uint32_t t_msg1;
    int msg1_tries, usb_drop_data, bad_msg3, tx_done_pending;
    int msg1_early, data_first;
    int scan_cmds, assoc_cmds, deauth_cmds;
    int last_scan_nchan, last_scan_chan0, mute_assoc;   /* v0.21.9 */
    int ps_on, ps_cmds, stats_on, getlog_cmds;          /* v0.21.10 */
    uint8_t last_data[1600];
    size_t last_data_len;
    int data_tx;
} fakefw_t;

static fakefw_t F;
static xw_core_t C;
static uint32_t g_now = 1000;
static int g_link_up = 0, g_link_events = 0;
static uint8_t g_rx_eth[1600];
static size_t g_rx_eth_len;
static int g_quiet = 1;

static void fq_cmd(const uint8_t *b, size_t n) {
    if (F.qn >= 64) return;
    int i = (F.qh + F.qn) % 64;
    memcpy(F.q[i], b, n);
    F.qn_len[i] = n;
    F.q_at[i] = g_now + F.resp_delay;
    F.qn++;
}
static void fq_data(const uint8_t *b, size_t n) {
    if (F.dn >= 64) return;
    int i = (F.dh + F.dn) % 64;
    memcpy(F.dq[i], b, n);
    F.dq_len[i] = n;
    F.dn++;
}

static void resp(uint16_t cmd, uint16_t seq, uint16_t result, const uint8_t *body, size_t bl) {
    uint8_t b[2100];
    mwx_put32(b, MWX_USB_TYPE_CMD);
    mwx_put16(b + 4, cmd | 0x8000);
    mwx_put16(b + 6, (uint16_t)(8 + bl));
    mwx_put16(b + 8, seq);
    mwx_put16(b + 10, result);
    if (bl) memcpy(b + 12, body, bl);
    fq_cmd(b, 12 + bl);
}
static void event(uint32_t id, uint16_t reason) {
    uint8_t b[16];
    mwx_put32(b, MWX_USB_TYPE_EVENT);
    mwx_put32(b + 4, id);
    mwx_put16(b + 8, reason);
    fq_cmd(b, 10);
}

/* AP -> STA: quadro Ethernet num RxPD com 802.3+SNAP (como o firmware entrega) */
static void ap_send_eth(const uint8_t *eth, size_t n) {
    uint8_t b[2100];
    memset(b, 0, 24);
    size_t off = 24;
    uint8_t *pk = b + off;
    memcpy(pk, eth, 12);
    mwx_put16(pk + 12, (uint16_t)(n - 14 + 8));
    memcpy(pk + 14, "\xaa\xaa\x03\x00\x00\x00", 6);
    pk[20] = eth[12];
    pk[21] = eth[13];
    memcpy(pk + 22, eth + 14, n - 14);
    mwx_put16(b + 2, (uint16_t)(n + 8));
    mwx_put16(b + 4, (uint16_t)off);
    b[12] = 40; b[13] = (uint8_t)-90;
    fq_data(b, off + n + 8);
}

static void be16w(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void ap_eapol(uint16_t info, const uint8_t *nonce, const uint8_t *data, size_t dlen, int mic) {
    uint8_t e[400];
    memset(e, 0, sizeof(e));
    memcpy(e, F.mac, 6);
    memcpy(e + 6, F.ap_bssid, 6);
    e[12] = 0x88; e[13] = 0x8e;
    uint8_t *k = e + 14;
    size_t total = XW_OFF_DATA + dlen;
    k[0] = 2; k[1] = 3;
    be16w(k + 2, (uint16_t)(total - 4));
    k[4] = 2;
    be16w(k + XW_OFF_INFO, info);
    be16w(k + XW_OFF_KLEN, 16);
    F.replay++;
    for (int i = 0; i < 8; i++) k[XW_OFF_REPLAY + i] = (uint8_t)(F.replay >> (56 - 8 * i));
    if (nonce) memcpy(k + XW_OFF_NONCE, nonce, 32);
    be16w(k + XW_OFF_DLEN, (uint16_t)dlen);
    if (dlen) memcpy(k + XW_OFF_DATA, data, dlen);
    if (mic) {
        uint8_t h[20];
        xw_hmac_sha1(F.ptk, 16, k, total, h);
        memcpy(k + XW_OFF_MIC, h, 16);
    }
    ap_send_eth(e, 14 + total);
}

static void ap_key_data(uint8_t *out, size_t *ol, int with_rsn) {
    uint8_t pl[128];
    size_t n = 0;
    if (with_rsn) {
        uint8_t rie[24];
        size_t rl = mwx_build_rsn_ie(rie, F.group_cipher);
        memcpy(pl, rie, rl);
        n += rl;
    }
    pl[n++] = 0xdd;
    pl[n++] = (uint8_t)(6 + F.gtk_len);
    pl[n++] = 0; pl[n++] = 0x0f; pl[n++] = 0xac; pl[n++] = 1;
    pl[n++] = (uint8_t)F.gtk_idx; pl[n++] = 0;
    memcpy(pl + n, F.gtk, (size_t)F.gtk_len);
    n += (size_t)F.gtk_len;
    if (n % 8 || n < 16) { pl[n++] = 0xdd; while (n % 8 || n < 16) pl[n++] = 0; }
    xw_aes_wrap(F.ptk + 16, pl, n, out);
    *ol = n + 8;
}

static void ap_start_4way(void) {
    for (int i = 0; i < 32; i++) F.anonce[i] = (uint8_t)(rand());
    ap_eapol(2 | XW_KI_PAIRWISE | XW_KI_ACK, F.anonce, NULL, 0, 0);
    F.ap_state = 1;
    F.t_msg1 = g_now;
    F.msg1_tries = 1;
}

/* AP esperando a msg 2: repete a msg 1 a cada 1 s, 3 vezes, depois DEAUTH 15 */
static void ap_timer(void) {
    if (F.ap_state != 1 || g_now - F.t_msg1 < 1000) return;
    F.t_msg1 = g_now;
    if (F.msg1_tries < 4) {
        F.msg1_tries++;
        ap_eapol(2 | XW_KI_PAIRWISE | XW_KI_ACK, F.anonce, NULL, 0, 0);
    } else {
        F.ap_state = 0;
        F.assoc = 0;
        event(MWX_EVT_DEAUTH, 15);
    }
}

static void ap_rekey(void) {
    F.gtk_idx = F.gtk_idx == 1 ? 2 : 1;
    for (int i = 0; i < F.gtk_len; i++) F.gtk[i] = (uint8_t)(0x70 + i);
    uint8_t kd[128];
    size_t kl;
    ap_key_data(kd, &kl, 0);
    ap_eapol(2 | XW_KI_ACK | XW_KI_MIC | XW_KI_SECURE | XW_KI_ENCDATA, NULL, kd, kl, 1);
    F.ap_state = 4;
}

/* STA -> AP */
static void ap_rx_eapol(const uint8_t *k, size_t n) {
    uint16_t info = (uint16_t)((k[XW_OFF_INFO] << 8) | k[XW_OFF_INFO + 1]);
    uint8_t tmp[400], h[20];
    if (F.ap_state == 1 && (info & XW_KI_PAIRWISE) && (info & XW_KI_MIC) && !(info & XW_KI_SECURE)) {
        memcpy(F.snonce, k + XW_OFF_NONCE, 32);
        xw_derive_ptk(F.ap_pmk, F.ap_bssid, F.mac, F.anonce, F.snonce, F.ptk);
        memcpy(tmp, k, n);
        memset(tmp + XW_OFF_MIC, 0, 16);
        xw_hmac_sha1(F.ptk, 16, tmp, n, h);
        if (memcmp(h, k + XW_OFF_MIC, 16)) { F.msg2_bad_mic++; return; }   /* AP fica calado (senha errada) */
        size_t dl = (size_t)((k[XW_OFF_DLEN] << 8) | k[XW_OFF_DLEN + 1]);
        CHECK(dl == 22 && k[XW_OFF_DATA] == 0x30, "msg2 leva o RSN IE");
        uint8_t kd[128];
        size_t kl;
        ap_key_data(kd, &kl, 1);
        if (F.bad_msg3) F.ptk[0] ^= 1;   /* MIC da msg 3 com KCK errada */
        ap_eapol(2 | XW_KI_PAIRWISE | XW_KI_ACK | XW_KI_MIC | XW_KI_INSTALL | XW_KI_SECURE | XW_KI_ENCDATA,
                 F.anonce, kd, kl, 1);
        if (F.bad_msg3) F.ptk[0] ^= 1;
        F.ap_state = 2;
        return;
    }
    if ((F.ap_state == 2 || F.ap_state == 4) && (info & XW_KI_MIC) && (info & XW_KI_SECURE)) {
        memcpy(tmp, k, n);
        memset(tmp + XW_OFF_MIC, 0, 16);
        xw_hmac_sha1(F.ptk, 16, tmp, n, h);
        CHECK(!memcmp(h, k + XW_OFF_MIC, 16), "MIC da msg %s", F.ap_state == 2 ? "4/4" : "grupo 2/2");
        CHECK(F.ap_state != 2 || (info & XW_KI_PAIRWISE), "msg4 é par");
        F.ap_state = 3;
    }
}

static int fw_send_cmd(void *u, const uint8_t *b, size_t n) {
    (void)u;
    if (!F.booted) {
        if (n == 12 && mwx_get32(b) == MWX_USB_TYPE_CMD && mwx_get16(b + 4) == MWX_CMD_FUNC_INIT) {
            if (F.rom) { uint8_t r[8] = { 1, 0, 0, 0, 0, 0, 0, 0 }; fq_cmd(r, 8); }
            else resp(MWX_CMD_FUNC_INIT, 1, 0, NULL, 0);
            return 0;
        }
        if (n == 16 && !memcmp(b, "MRVL", 4)) {
            uint8_t ack[8] = { 0, 0, 0, 0, 1, 0, 0, 0 };
            fq_cmd(ack, 8);
            if (!F.no_init_done) event(MWX_EVT_INIT_DONE, 0);
            F.booted = 1;
            F.rom = 0;
            return 0;
        }
        if (!F.rom) F.booted = 1;
        else return 0;
    }
    CHECK(mwx_get32(b) == MWX_USB_TYPE_CMD && mwx_get16(b + 6) == n - 4, "moldura do comando");
    uint16_t cmd = mwx_get16(b + 4), seq = mwx_get16(b + 8);
    const uint8_t *body = b + 12;
    switch (cmd) {
    case MWX_CMD_FUNC_INIT: resp(cmd, seq, 0, NULL, 0); break;
    case MWX_CMD_GET_HW_SPEC: {
        uint8_t hs[80];
        memset(hs, 0, sizeof(hs));
        memcpy(hs + 8, F.mac, 6);
        mwx_put32(hs + 18, 0x0e3c0102);
        size_t l = 63;
        if (F.key_v2) { mwx_put16(hs + 63, 0x01c7); mwx_put16(hs + 65, 4); mwx_put16(hs + 67, 1); hs[69] = 2; hs[70] = 0; l = 71; }
        resp(cmd, seq, 0, hs, l);
        break;
    }
    case MWX_CMD_MAC_CONTROL: resp(cmd, seq, 0, NULL, 0); break;
    case MWX_CMD_SCAN: {
        F.scan_cmds++;
        /* canais pedidos e SSID filtrado */
        size_t p = 7;
        int wild = 1;
        char want[33] = "";
        uint8_t chans[14];
        int nch = 0;
        while (p + 4 <= n - 12) {
            uint16_t t = mwx_get16(body + p), l = mwx_get16(body + p + 2);
            if (t == 0x0112) { wild = body[p + 4] != 0; memcpy(want, body + p + 5, (size_t)l - 1); want[l - 1] = 0; }
            if (t == 0x0101) for (int i = 0; i < l / 7; i++) chans[nch++] = body[p + 4 + i * 7 + 1];
            p += 4 + l;
        }
        F.last_scan_nchan = nch;
        F.last_scan_chan0 = nch ? chans[0] : 0;
        uint8_t r[1500], *q = r + 3;
        size_t t = 0;
        int sets = 0;
        for (int i = 0; i < nch; i++) {
            const uint8_t b2[6] = { 2, 2, 2, 2, 2, chans[i] };
            if (chans[i] == 6 && (wild || !strcmp(want, F.ap_ssid))) {
                t += bss_entry(q + t, F.ap_bssid, -48, F.ap_secure ? 0x0411 : 0x0401, F.ap_ssid, 6,
                               F.ap_secure ? (F.group_cipher == 2 ? RSN_PSK_TKIPGRP : RSN_PSK_CCMP) : NULL,
                               F.ap_secure ? (F.group_cipher == 2 ? sizeof(RSN_PSK_TKIPGRP) : sizeof(RSN_PSK_CCMP)) : 0, 0);
                sets++;
            }
            if (chans[i] == 6 && wild) {   /* outro AP do mesmo SSID, mais fraco */
                const uint8_t b3[6] = { 3, 3, 3, 3, 3, 3 };
                t += bss_entry(q + t, b3, -80, 0x0411, F.ap_ssid, 6, RSN_PSK_CCMP, sizeof(RSN_PSK_CCMP), 0);
                sets++;
            }
            if (chans[i] == 11 && wild) {
                t += bss_entry(q + t, b2, -71, 0x0411, "Vizinho WPA3", 11, RSN_SAE, sizeof(RSN_SAE), 0);
                t += bss_entry(q + t, b2, -55, 0x0401, "Cafe aberto", 11, NULL, 0, 0);
                t += bss_entry(q + t, b2, -90, 0x0401, "", 11, NULL, 0, 0);   /* oculta */
                sets += 3;
            }
        }
        mwx_put16(r, (uint16_t)t);
        r[2] = (uint8_t)sets;
        resp(cmd, seq, 0, r, 3 + t);
        break;
    }
    case MWX_CMD_ASSOCIATE: {
        F.assoc_cmds++;
        if (F.mute_assoc) break;   /* v0.21.9: firmware calado */
        int ok = !memcmp(body, F.ap_bssid, 6);
        /* RSN TLV presente sse a rede é segura */
        int has_rsn = 0;
        size_t p = 13;
        while (p + 4 <= n - 12) {
            uint16_t t = mwx_get16(body + p), l = mwx_get16(body + p + 2);
            if (t == 0x30) has_rsn = 1;
            p += 4 + l;
        }
        CHECK(has_rsn == F.ap_secure, "RSN TLV na associação = %d", F.ap_secure);
        uint8_t r[6];
        mwx_put16(r, 0x0411);
        mwx_put16(r + 2, ok ? 0 : 17);
        mwx_put16(r + 4, 0xc001);
        resp(cmd, seq, 0, r, 6);
        if (ok) {
            F.assoc = 1;
            if (F.ap_secure) ap_start_4way();
            if (F.msg1_early) F.data_first = 1;   /* a msg 1 sai pelo EP2 antes da resposta pelo EP1 */
        }
        break;
    }
    case MWX_CMD_KEY_MATERIAL: {
        uint16_t t = mwx_get16(body + 2);
        if (t == 0x0100) {
            uint16_t type = mwx_get16(body + 6), info = mwx_get16(body + 8), kl = mwx_get16(body + 10);
            if (info & 2) { CHECK(type == 2 && kl == 16, "TK AES"); memcpy(F.inst_tk, body + 12, 16); F.inst_tk_ok = 1; }
            else { memcpy(F.inst_gtk, body + 12, kl); F.inst_gtk_len = kl; F.inst_gtk_ok = 1; }
        } else if (t == 0x019c) {
            const uint8_t *q = body + 6;
            uint16_t info = mwx_get16(q + 8), kl = mwx_get16(q + 18);
            if (info & 2) { CHECK(!memcmp(q, F.ap_bssid, 6), "mac v2 = bssid"); memcpy(F.inst_tk, q + 20, 16); F.inst_tk_ok = 1; }
            else { CHECK(q[6] == F.gtk_idx, "índice da GTK v2"); memcpy(F.inst_gtk, q + 20, kl); F.inst_gtk_len = kl; F.inst_gtk_ok = 1; }
        } else {
            CHECK(0, "TLV de chave desconhecido 0x%04x", t);
        }
        resp(cmd, seq, 0, NULL, 0);
        break;
    }
    case MWX_CMD_PS_MODE_ENH: {
        F.ps_cmds++;
        uint16_t act = mwx_get16(body);
        uint8_t r[4];
        mwx_put16(r, act);
        if (act == MWX_PS_GET) { mwx_put16(r + 2, F.ps_on ? MWX_PS_BITMAP_STA : 0); resp(cmd, seq, 0, r, 4); }
        else if (act == MWX_PS_DIS_AUTO) { F.ps_on = 0; mwx_put16(r + 2, MWX_PS_BITMAP_STA); resp(cmd, seq, 0, r, 4); }
        else resp(cmd, seq, 1, r, 4);
        break;
    }
    case MWX_CMD_GET_LOG: {
        if (!F.stats_on) { resp(cmd, seq, 0xffff, NULL, 0); break; }
        F.getlog_cmds++;
        uint8_t r[76];
        for (int i = 0; i < 19; i++) mwx_put32(r + i * 4, (uint32_t)(100 * (i + 1)));
        resp(cmd, seq, 0, r, 76);
        break;
    }
    case MWX_CMD_RSSI_INFO: {
        if (!F.stats_on) { resp(cmd, seq, 0xffff, NULL, 0); break; }
        uint8_t r[26];
        memset(r, 0, sizeof(r));
        mwx_put16(r + 6, (uint16_t)-30); mwx_put16(r + 10, (uint16_t)-31); mwx_put16(r + 14, 60);
        mwx_put16(r + 18, (uint16_t)-92); mwx_put16(r + 20, (uint16_t)-29); mwx_put16(r + 24, (uint16_t)-28);
        resp(cmd, seq, 0, r, 26);
        break;
    }
    case MWX_CMD_TX_RATE_QUERY: {
        if (!F.stats_on) { resp(cmd, seq, 0xffff, NULL, 0); break; }
        uint8_t r[2] = { 11, 0 };
        resp(cmd, seq, 0, r, 2);
        break;
    }
    case MWX_CMD_DEAUTH:
        F.deauth_cmds++;
        F.assoc = 0;
        F.ap_state = 0;
        resp(cmd, seq, 0, NULL, 0);
        break;
    default:
        resp(cmd, seq, 0xffff, NULL, 0);
        break;
    }
    return 0;
}

static int fw_send_data(void *u, const uint8_t *b, size_t n) {
    (void)u;
    size_t off = mwx_get16(b + 4);
    CHECK(n >= 20 + 14 && (off == 20 || off == 24) && mwx_get16(b + 2) == n - off, "TxPD (offset %u)", (unsigned)off);
    CHECK(n % 64 != 0, "v0.21.8: transferência de dados nunca múltipla de 64 (sem ZLP): %u", (unsigned)n);
    for (size_t k = 20; k < off; k++) CHECK(b[k] == 0, "pad do TxPD zerado");
    const uint8_t *e = b + off;
    size_t el = n - off;
    F.data_tx++;
    if (F.usb_drop_data) return 0;   /* aceito na fila, nunca sai do USB */
    F.tx_done_pending++;
    if (e[12] == 0x88 && e[13] == 0x8e) {
        CHECK(!memcmp(e, F.ap_bssid, 6) && !memcmp(e + 6, F.mac, 6), "EAPOL endereçado ao AP");
        ap_rx_eapol(e + 14, el - 14);
        return 0;
    }
    memcpy(F.last_data, e, el);
    F.last_data_len = el;
    return 0;
}

static void ops_rx_eth(void *u, uint8_t *f, size_t n) { (void)u; memcpy(g_rx_eth, f, n); g_rx_eth_len = n; }
static void ops_link(void *u, int up) { (void)u; g_link_up = up; g_link_events++; }
static void ops_log(void *u, const char *l) {
    (void)u;
    if (!g_quiet) printf("  %s\n", l);
    CHECK(!strstr(l, "segredo123"), "senha no log!");
}
static int ops_rand(void *u, uint8_t *b, size_t n) { (void)u; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rand(); return 0; }

static const xw_ops_t OPS = { fw_send_cmd, fw_send_data, ops_rx_eth, ops_link, ops_log, ops_rand, NULL };

/* roda o "barramento" por ms milissegundos */
static void drain_data(void) {
    while (F.dn) {
        int k = F.dh;
        F.dh = (F.dh + 1) % 64;
        F.dn--;
        uint8_t tmp[2100];
        memcpy(tmp, F.dq[k], F.dq_len[k]);
        xw_on_data_in(&C, tmp, F.dq_len[k], g_now);
    }
}

/* v0.21.9: o glue colhe o USB com um "agora" mais novo que o do tick */
static uint32_t g_rx_ahead = 0;
static void run(int ms) {
    for (int i = 0; i < ms; i++) {
        g_now++;
        if (F.data_first) { F.data_first = 0; drain_data(); }
        while (F.qn && (int32_t)(g_now - F.q_at[F.qh]) >= 0) {
            int k = F.qh;
            F.qh = (F.qh + 1) % 64;
            F.qn--;
            uint8_t tmp[2100];
            memcpy(tmp, F.q[k], F.qn_len[k]);
            xw_on_cmd_in(&C, tmp, F.qn_len[k], g_now + g_rx_ahead);
        }
        while (F.dn) {
            int k = F.dh;
            F.dh = (F.dh + 1) % 64;
            F.dn--;
            uint8_t tmp[2100];
            memcpy(tmp, F.dq[k], F.dq_len[k]);
            xw_on_data_in(&C, tmp, F.dq_len[k], g_now + g_rx_ahead);
        }
        if (F.tx_done_pending) { xw_note_data_tx(&C, (unsigned)F.tx_done_pending); F.tx_done_pending = 0; }
        ap_timer();
        xw_tick(&C, g_now);
    }
}

static void fw_reset(int rom, int secure, uint8_t group, int key_v2) {
    memset(&F, 0, sizeof(F));
    F.rom = rom;
    memcpy(F.mac, "\x00\x22\x48\xaa\xbb\xcc", 6);
    memcpy(F.ap_bssid, "\x10\x20\x30\x40\x50\x66", 6);
    strcpy(F.ap_ssid, "Casa do Sérgio");
    F.ap_secure = secure;
    F.group_cipher = group;
    F.gtk_len = group == 2 ? 32 : 16;
    F.gtk_idx = 1;
    F.key_v2 = key_v2;
    for (int i = 0; i < 32; i++) F.gtk[i] = (uint8_t)(0x30 + i);
    xw_pmk_from_passphrase("segredo123", (const uint8_t *)F.ap_ssid, strlen(F.ap_ssid), F.ap_pmk);
    g_link_up = 0;
    g_link_events = 0;
    xw_init(&C, &OPS);
    xw_start(&C, g_now);
}

static void test_driver(int key_v2, uint8_t group) {
    printf("--- driver (ROM, WPA2, grupo %s, chave v%d)\n", group == 2 ? "TKIP" : "CCMP", key_v2 ? 2 : 1);
    fw_reset(1, 1, group, key_v2);
    run(300);
    CHECK(C.dev == XW_DEV_READY, "adaptador pronto (dev=%d)", C.dev);
    CHECK(!memcmp(C.mac, F.mac, 6) && C.key_v2 == key_v2, "MAC e API de chave");
    /* scan */
    xw_scan(&C);
    run(400);
    CHECK(C.nbss == 3, "3 redes (sem oculta, sem SSID repetido): %d", C.nbss);
    CHECK(C.nbss == 3 && !strcmp(C.bss[0].ssid, "Casa do Sérgio") && C.bss[0].rssi == -48, "mais forte primeiro");
    CHECK(C.nbss == 3 && C.bss[1].sec == MWX_SEC_OPEN && C.bss[2].sec == MWX_SEC_WPA3, "classificação na lista");
    /* senha errada (pedido do usuário): desiste */
    uint8_t pmk[32];
    xw_pmk_from_passphrase("errada9999", (const uint8_t *)F.ap_ssid, strlen(F.ap_ssid), pmk);
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), pmk, 1);
    run(9000);
    CHECK(F.msg2_bad_mic >= 1 && C.err == XW_E_AUTH && !C.want && !g_link_up, "senha errada = XW_E_AUTH (err=%d)", C.err);
    CHECK(C.hs_msg1 >= 2 && C.hs_msg1_after2 >= 1 && C.hs_tx_done >= 1, "prova: msg 1 repetida depois da msg 2 que saiu");
    /* msg 2 nunca sai do USB: NÃO é senha errada */
    F.usb_drop_data = 1;
    xw_pmk_from_passphrase("segredo123", (const uint8_t *)F.ap_ssid, strlen(F.ap_ssid), pmk);
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), pmk, 1);
    run(9000);
    CHECK(C.err == XW_E_HANDSHAKE && C.hs_tx_done == 0 && C.hs_msg2_q >= 1, "msg 2 presa no USB = XW_E_HANDSHAKE (err=%d)", C.err);
    CHECK(strstr(C.err_detail, "msg2 enviada") != NULL, "detalhe: %s", C.err_detail);
    F.usb_drop_data = 0;
    /* MIC da msg 3 errado: senha errada provada na hora */
    F.bad_msg3 = 1;
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), pmk, 1);
    run(3000);
    CHECK(C.err == XW_E_AUTH && C.hs_mic_bad == 1 && !g_link_up, "MIC da msg 3 = XW_E_AUTH (err=%d)", C.err);
    F.bad_msg3 = 0;
    /* senha certa */
    xw_pmk_from_passphrase("segredo123", (const uint8_t *)F.ap_ssid, strlen(F.ap_ssid), pmk);
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), pmk, 1);
    run(1500);
    CHECK(C.link == XW_LINK_UP && g_link_up, "conectado (link=%d err=%d)", C.link, C.err);
    CHECK(F.ap_state == 3, "AP recebeu a msg 4");
    CHECK(F.inst_tk_ok && !memcmp(F.inst_tk, F.ptk + 32, 16), "TK instalada = a do AP");
    uint8_t g[32];
    memcpy(g, F.gtk, 32);
    if (group == 2) { memcpy(g + 16, F.gtk + 24, 8); memcpy(g + 24, F.gtk + 16, 8); }
    CHECK(F.inst_gtk_ok && F.inst_gtk_len == F.gtk_len && !memcmp(F.inst_gtk, g, (size_t)F.gtk_len), "GTK instalada");
    CHECK(!memcmp(C.cur.bssid, F.ap_bssid, 6), "escolheu o BSS mais forte");
    /* dados: AP -> STA e STA -> AP */
    uint8_t e[60];
    memcpy(e, F.mac, 6); memcpy(e + 6, F.ap_bssid, 6); e[12] = 0x08; e[13] = 0x00;
    memset(e + 14, 0x45, 46);
    ap_send_eth(e, 60);
    run(2);
    CHECK(g_rx_eth_len == 60 && !memcmp(g_rx_eth, e, 60), "RX IPv4 chega inteiro na pilha");
    CHECK(xw_tx_eth(&C, e, 60) == 0 && F.last_data_len == 60 && !memcmp(F.last_data, e, 60), "TX");
    /* rekey de grupo */
    F.inst_gtk_ok = 0;
    ap_rekey();
    run(50);
    CHECK(F.ap_state == 3 && F.inst_gtk_ok && F.inst_gtk[0] == 0x70, "rekey de grupo instalou a GTK nova");
    /* queda: DEAUTH do AP -> reconecta sozinho */
    F.assoc = 0;
    F.ap_state = 0;
    event(MWX_EVT_DEAUTH, 7);
    run(10);
    CHECK(!g_link_up && C.err == XW_E_LOST && C.want, "caiu, quer reconectar");
    run(6000);
    CHECK(C.link == XW_LINK_UP && g_link_up, "reconectou sozinho (link=%d)", C.link);
    /* esquecer */
    /* msg 1 antes da resposta do ASSOCIATE (corrida EP1/EP2): guarda e processa */
    xw_disconnect(&C);
    run(50);
    F.msg1_early = 1;
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), pmk, 1);
    run(1500);
    CHECK(C.link == XW_LINK_UP && F.ap_state == 3, "msg 1 adiantada: conectou (link=%d)", C.link);
    F.msg1_early = 0;
    xw_disconnect(&C);
    run(50);
    CHECK(C.link == XW_LINK_IDLE && !g_link_up && F.deauth_cmds >= 1 && !C.want, "desconectou");
    int before = F.assoc_cmds;
    run(40000);
    CHECK(F.assoc_cmds == before, "não reconecta depois de esquecer");
}

/* v0.21.9: o console mostrou "comando 0x0012 sem resposta" logo de cara (o
 * tick com um relógio mais velho que o do RX via tempo negativo) e o rótulo
 * "o roteador recusou". Aqui: relógio do RX adiantado, tudo conecta; timeout
 * de verdade vira XW_E_FWTIMEOUT; reconexão rápida no canal; eventos contados. */
static void test_v0219(void) {
    printf("--- v0.21.9: relógio, timeout, reconexão rápida, eventos\n");
    fw_reset(0, 1, 4, 0);
    g_rx_ahead = 7;
    F.resp_delay = 3;   /* o firmware leva uns ms: o tick roda entre o envio e a resposta */
    run(300);
    CHECK(C.dev == XW_DEV_READY, "pronto com o RX adiantado");
    uint8_t pmk[32];
    xw_pmk_from_passphrase("segredo123", (const uint8_t *)F.ap_ssid, strlen(F.ap_ssid), pmk);
    int a0 = F.assoc_cmds;
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), pmk, 0);
    run(1500);
    CHECK(C.link == XW_LINK_UP && F.assoc_cmds == a0 + 1 && C.err == XW_E_NONE,
          "conectou de primeira com o relógio do RX adiantado (link=%d, ASSOCIATE x%d, err=%d)", C.link, F.assoc_cmds - a0, C.err);
    /* reconexão rápida: só o canal atual */
    int sc = F.scan_cmds;
    CHECK(xw_reconnect(&C) == 0, "reconnect aceito");
    run(1500);
    CHECK(C.link == XW_LINK_UP && F.scan_cmds == sc + 1 && F.last_scan_nchan == 1 && F.last_scan_chan0 == C.cur.channel,
          "reconexão rápida: 1 varredura de 1 canal (%d, canais %d, canal %d/%d)", F.scan_cmds - sc, F.last_scan_nchan,
          F.last_scan_chan0, C.cur.channel);
    /* evento desconhecido: contado e logado, não derruba nada */
    unsigned ev0 = C.ev_total;
    event(0x0017, 0);
    event(0x0017, 0);
    run(5);
    char es[120];
    xw_ev_summary(&C, es, sizeof(es));
    CHECK(C.ev_total == ev0 + 2 && strstr(es, "0x0017 x2") && C.link == XW_LINK_UP, "evento 0x0017 contado (%s)", es);
    /* firmware que não responde ao ASSOCIATE: tempo esgotado, não "recusou" */
    F.mute_assoc = 1;
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), pmk, 0);
    run(14000);
    CHECK(C.err == XW_E_FWTIMEOUT && C.want && strstr(C.err_detail, "ASSOCIATE"), "ASSOCIATE sem resposta = FWTIMEOUT (err=%d, %s)",
          C.err, C.err_detail);
    CHECK(strcmp(xw_err_text(XW_E_FWTIMEOUT), xw_err_text(XW_E_ASSOC)) != 0, "texto diferente de \"recusou\"");
    F.mute_assoc = 0;
    run(8000);
    CHECK(C.link == XW_LINK_UP, "voltou sozinho depois (link=%d)", C.link);
    g_rx_ahead = 0;
}

/* v0.21.10: PS desligado depois de conectar; estatísticas do firmware */
static void test_v02110(void) {
    printf("--- v0.21.10: economia de energia e estatísticas\n");
    fw_reset(0, 0, 0, 0);
    F.ps_on = 1;
    F.stats_on = 1;
    run(300);
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), NULL, 0);
    run(1000);
    CHECK(C.link == XW_LINK_UP, "conectou");
    CHECK(F.ps_cmds == 2 && C.ps_get_result == 0 && (C.ps_bitmap & MWX_PS_BITMAP_STA) && C.ps_dis_result == 0 && !F.ps_on,
          "PS lido ligado e desligado (cmds %d, get %d bitmap 0x%x, dis %d)", F.ps_cmds, C.ps_get_result, C.ps_bitmap,
          C.ps_dis_result);
    CHECK(xw_fw_query(&C) == 1, "consulta pedida");
    run(20);
    char t[300];
    xw_fwstats_text(&C, t, sizeof(t));
    CHECK(C.fws_n == 19 && C.fws[1] == 200 && C.rssi_ok && C.rssi_bcn_last == -29 && C.rate_ok && strstr(t, "taxa TX 48 Mb/s") &&
          strstr(t, "beacons 1800 perdidos 1900"), "estatísticas: %s", t);
    /* firmware sem os comandos: marca e não pede mais */
    F.stats_on = 0;
    xw_fw_query(&C);
    run(20);
    int g = F.getlog_cmds;
    CHECK(C.fwq_unsup == 7, "recusados marcados (0x%x)", C.fwq_unsup);
    CHECK(xw_fw_query(&C) == 0 && F.getlog_cmds == g, "não pede mais");
    xw_fwstats_text(&C, t, sizeof(t));
    CHECK(strstr(t, "recusado") != NULL, "texto: %s", t);
    CHECK(C.link == XW_LINK_UP, "link segue de pé");
}

static void test_driver_open(void) {
    printf("--- driver (firmware já rodando, rede aberta)\n");
    fw_reset(0, 0, 0, 0);
    run(300);
    CHECK(C.dev == XW_DEV_READY, "pronto sem MRVL");
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), NULL, 1);
    run(1000);
    CHECK(C.link == XW_LINK_UP, "aberta conectada");
    /* rede aberta pedindo PMK: recusa */
    uint8_t pmk[32] = { 1 };
    xw_connect(&C, F.ap_ssid, (int)strlen(F.ap_ssid), pmk, 0);
    run(1000);
    CHECK(C.err == XW_E_CHANGED && !C.want, "salva com senha, rede virou aberta");
    /* rede que não existe: tenta de novo */
    xw_connect(&C, "Nao existe", 10, NULL, 0);
    run(1000);
    CHECK(C.err == XW_E_NOTFOUND && C.want && C.link == XW_LINK_WAIT, "não achou: espera e tenta de novo");
    int sc = F.scan_cmds;
    run(5000);
    CHECK(F.scan_cmds > sc, "tentou de novo");
}

static void test_txpad(void) {
    int multiples = 0;
    for (size_t fl = 14; fl <= 1514; fl++) {
        size_t off = mwx_txpd_offset(fl);
        if ((off + fl) % 64 == 0) multiples++;
        if ((20 + fl) % 64 == 0) CHECK(off == 24, "quadro de %u: offset 24", (unsigned)fl);
        else CHECK(off == 20, "quadro de %u: offset 20", (unsigned)fl);
    }
    CHECK(multiples == 0, "nenhum tamanho de quadro gera transferência múltipla de 64 (%d)", multiples);
    uint8_t b[64];
    memset(b, 0xee, sizeof(b));
    mwx_build_txpd_off(b, 1004, 24);
    CHECK(mwx_get16(b + 2) == 1004 && mwx_get16(b + 4) == 24 && b[20] == 0 && b[23] == 0 && b[24] == 0xee, "TxPD de 24");
}

static void test_store(void) {
    xw_saved_t s, r;
    memset(&s, 0, sizeof(s));
    strcpy(s.ssid, "Café 2.4G");
    s.ssid_len = (int)strlen(s.ssid);
    s.secure = 1;
    for (int i = 0; i < 32; i++) s.pmk[i] = (uint8_t)(i * 7);
    char t[300];
    size_t n = xw_store_format(t, sizeof(t), &s);
    CHECK(n > 0 && !strstr(t, "Caf"), "WIFI.TXT sem SSID em claro");
    CHECK(xw_store_parse(t, n, &r) == 0 && r.secure && r.ssid_len == s.ssid_len && !strcmp(r.ssid, s.ssid) &&
          !memcmp(r.pmk, s.pmk, 32), "ida e volta");
    char crlf[300];
    size_t m = 0;
    for (size_t i = 0; i < n; i++) { if (t[i] == '\n') crlf[m++] = '\r'; crlf[m++] = t[i]; }
    CHECK(xw_store_parse(crlf, m, &r) == 0, "CRLF (editado no Windows)");
    s.secure = 0;
    n = xw_store_format(t, sizeof(t), &s);
    CHECK(xw_store_parse(t, n, &r) == 0 && !r.secure, "aberta");
    CHECK(xw_store_parse("XCWIFI1\nssid=zz\npmk=open\n", 25, &r) != 0, "hex inválido");
    CHECK(xw_store_parse("ssid=41\npmk=open\n", 18, &r) != 0, "sem cabeçalho");
}

/* ---- teclado de texto (beta/xc_keypad.h, xtk_*) ---- */
static void test_textpad(void) {
    xtk_t k;
    xtk_init(&k, 8, 63);
    /* todo ASCII imprimível existe em alguma página (espaço = ação) */
    int have[128] = { 0 };
    for (int pg = 0; pg < XTK_NPAGE; pg++)
        for (int r = 0; r < XTK_ROWS; r++)
            for (int c = 0; c < XTK_COLS; c++) { char ch = xtk_char_at(pg, r, c); if (ch) have[(int)ch] = 1; }
    have[' '] = 1;
    int missing = 0;
    for (int ch = 0x20; ch <= 0x7e; ch++) if (!have[ch]) { missing++; printf("  falta '%c'\n", ch); }
    CHECK(missing == 0, "todo ASCII imprimível digitável");
    for (int r = 0; r < XTK_ROWS; r++) CHECK(strlen("1234567890") == 10 && xtk_char_at(XTK_SYM, r, 9) != 0, "fileira cheia");
    /* digita "Ab1 x" pelo teclado: A (Shift), b volta pra minúscula sozinha */
    k.row = XTK_ROW_A1; k.col = XTK_A_SHIFT;
    CHECK(xtk_press(&k) == XTK_PAGE && k.page == XTK_UPPER, "shift");
    k.row = 2; k.col = 0;
    CHECK(xtk_press(&k) == XTK_TYPED && k.text[0] == 'A' && k.page == XTK_LOWER, "maiúscula e volta");
    k.row = 3; k.col = 4;
    xtk_press(&k);
    k.row = 0; k.col = 0;
    xtk_press(&k);
    k.row = XTK_ROW_A1; k.col = XTK_A_SPACE;
    xtk_press(&k);
    CHECK(!strcmp(k.text, "Ab1 ") && k.n == 4, "texto \"%s\"", k.text);
    k.row = XTK_ROW_A2; k.col = XTK_A_OK;
    CHECK(xtk_press(&k) == XTK_NONE, "Conectar com 4 caracteres recusado");
    for (int i = 0; i < 70; i++) xtk_type(&k, 'z');
    CHECK(k.n == 63 && xtk_ready(&k) && xtk_press(&k) == XTK_CONFIRM, "63 no máximo; Conectar");
    /* navegação: desce da coluna 9 pras ações e volta perto */
    xtk_clear(&k);
    k.row = 3; k.col = 9;
    xtk_nav(&k, 0, 1);
    CHECK(k.row == XTK_ROW_A1 && k.col == XTK_A_DEL, "col 9 -> Apagar");
    xtk_nav(&k, 0, 1);
    CHECK(k.row == XTK_ROW_A2 && k.col == XTK_A_OK, "Apagar -> Conectar");
    xtk_nav(&k, 0, 1);
    CHECK(k.row == XTK_ROW_A2, "para embaixo");
    xtk_nav(&k, -5, 0);
    CHECK(k.col == XTK_A_SHOW, "para na ponta");
    xtk_nav(&k, 0, -1);
    CHECK(k.row == XTK_ROW_A1 && k.col == XTK_A_SYM, "Mostrar -> #+= (col %d)", k.col);
    xtk_nav(&k, 0, -1);
    CHECK(k.row == 3 && k.col == 3, "#+= -> col 3 (%d)", k.col);
    xtk_type(&k, 'q');
    CHECK(xtk_toggle_show(&k) == XTK_SHOWN && k.show, "mostrar");
    xtk_wipe(&k);
    CHECK(k.n == 0 && k.text[0] == 0 && k.maxlen == 63 && k.minlen == 8 && !k.show, "wipe");
    CHECK(xtk_backspace(&k) == 0, "apagar vazio");
}

/* ---- v0.21.5: decisão de rede no boot (wifi/xw_boot.h) ---- */
static int boot_expected(const xwb_in_t *b) {
    /* escrito à parte da implementação, na ordem do pedido do produto */
    if (b->cable) return XWB_GO;                       /* 2. cabo com internet: como hoje */
    if (b->wifi_ok) return XWB_GO;                     /* 3. Wi-Fi conectou: segue o boot */
    if (b->skipped) return XWB_GO;                     /* "Seguir sem internet" */
    if (b->adapter == 0) return XWB_NOINTERNET;        /* 4. Fat sem cabo */
    if (b->adapter_failed) return XWB_NOINTERNET;
    if (b->adapter < 0) return XWB_FINDING;
    if (b->saved) {
        if (b->auto_gave_up) return XWB_PICK;          /* 3 -> 4: falhou, mostra a lista */
        return b->auto_ms >= XWB_AUTO_MS ? XWB_PICK : XWB_CONNECTING;
    }
    return XWB_PICK;                                   /* 4. sem rede salva */
}

static void test_boot(void) {
    static const unsigned ms[3] = { 0, XWB_AUTO_MS - 1, XWB_AUTO_MS };
    int n = 0, bad = 0;
    for (int cable = 0; cable < 2; cable++)
    for (int ad = -1; ad <= 1; ad++)
    for (int fail = 0; fail < 2; fail++)
    for (int saved = 0; saved < 2; saved++)
    for (int ok = 0; ok < 2; ok++)
    for (int gu = 0; gu < 2; gu++)
    for (int m = 0; m < 3; m++)
    for (int sk = 0; sk < 2; sk++) {
        xwb_in_t b = { cable, ad, fail, saved, ok, gu, ms[m], sk };
        int got = xwb_decide(&b), want = boot_expected(&b);
        n++;
        if (got != want) {
            if (bad++ < 5) printf("  boot: cabo %d adaptador %d falha %d salva %d ok %d desistiu %d ms %u pulou %d: %s, esperado %s\n",
                                  cable, ad, fail, saved, ok, gu, ms[m], sk, xwb_name(got), xwb_name(want));
        }
        /* regra 7: com cabo nunca espera o Wi-Fi */
        if (cable && got != XWB_GO) bad++;
    }
    CHECK(bad == 0 && n == 2 * 3 * 2 * 2 * 2 * 2 * 3 * 2, "tabela do boot: %d casos, %d errados", n, bad);

    /* roteiros do pedido */
    xwb_in_t b;
    /* cabo com internet: segue na hora, com ou sem adaptador/rede salva */
    memset(&b, 0, sizeof(b)); b.cable = 1; b.adapter = -1; b.saved = 1;
    CHECK(xwb_decide(&b) == XWB_GO, "cabo: segue sem esperar o Wi-Fi");
    /* sem cabo, rede salva: procurando -> conectando -> conectou -> segue */
    memset(&b, 0, sizeof(b)); b.adapter = -1; b.saved = 1;
    CHECK(xwb_decide(&b) == XWB_FINDING, "procurando o adaptador");
    b.adapter = 1; b.auto_ms = 4000;
    CHECK(xwb_decide(&b) == XWB_CONNECTING, "conectando na salva");
    b.wifi_ok = 1;
    CHECK(xwb_decide(&b) == XWB_GO, "conectou: segue");
    /* rede salva falhou (senha/segurança): lista; o usuário conecta noutra: segue */
    memset(&b, 0, sizeof(b)); b.adapter = 1; b.saved = 1; b.auto_ms = 9000; b.auto_gave_up = 1;
    CHECK(xwb_decide(&b) == XWB_PICK, "salva falhou: lista");
    b.wifi_ok = 1;
    CHECK(xwb_decide(&b) == XWB_GO, "conectou pela lista: segue");
    /* rede salva fora do alcance (o driver segue tentando): 30 s -> lista */
    memset(&b, 0, sizeof(b)); b.adapter = 1; b.saved = 1; b.auto_ms = XWB_AUTO_MS - 1;
    CHECK(xwb_decide(&b) == XWB_CONNECTING, "29,9 s: ainda tentando");
    b.auto_ms = XWB_AUTO_MS;
    CHECK(xwb_decide(&b) == XWB_PICK, "30 s: lista");
    /* sem rede salva: lista direto; B = segue sem internet */
    memset(&b, 0, sizeof(b)); b.adapter = 1;
    CHECK(xwb_decide(&b) == XWB_PICK, "sem rede salva: lista");
    b.skipped = 1;
    CHECK(xwb_decide(&b) == XWB_GO, "seguir sem internet");
    /* Fat sem cabo: sem internet; o cabo pega DHCP no "Tentar de novo": segue */
    memset(&b, 0, sizeof(b)); b.adapter = 0;
    CHECK(xwb_decide(&b) == XWB_NOINTERNET, "Fat sem cabo: sem internet");
    b.cable = 1;
    CHECK(xwb_decide(&b) == XWB_GO, "cabo depois: segue");
    /* adaptador que não ligou: sem internet (com o motivo na tela) */
    memset(&b, 0, sizeof(b)); b.adapter = 1; b.adapter_failed = 1; b.saved = 1;
    CHECK(xwb_decide(&b) == XWB_NOINTERNET, "adaptador falhou: sem internet");
}

/* ---- v0.21.7: vigia de travamento (xc_wdog.c) ---- */
extern int xwd_test_pir;
static uint64_t g_wd_now = 0;
static char g_wd_last[256];
static int g_wd_n = 0, g_wd_raw = -1;
uint64_t xwd_now_ms(void) { return g_wd_now; }
void xwd_report(const char *line, int raw_ok) {
    snprintf(g_wd_last, sizeof(g_wd_last), "%s", line);
    g_wd_n++;
    g_wd_raw = raw_ok;
}

/* v0.21.12: tripwire (de outra hw thread) */
static char g_trip_last[300];
static int g_trip_n = 0;
void xwd_trip_report(const char *line, int slot) {
    (void)slot;
    snprintf(g_trip_last, sizeof(g_trip_last), "%s", line);
    g_trip_n++;
}

static void test_tripwire(void) {
    g_wd_now = 500000;
    xwd_test_pir = 0; xwd_beat("menu");
    xwd_test_pir = 1; xwd_beat("rede: wifi"); xwd_crumb("wifi: colhendo o USB (RX + lwIP)");
    xwd_test_pir = 5;
    g_wd_now = 507000;
    xwd_tripwire();
    CHECK(g_trip_n == 0, "7 s: nada");
    g_wd_now = 508300;
    xwd_tripwire();
    CHECK(g_trip_n == 2 && strstr(g_trip_last, "TRAVOU: rede (hw1) parada há 8 s") &&
          strstr(g_trip_last, "passo wifi: colhendo o USB") && strstr(g_trip_last, "tripwire na hw5"),
          "as duas paradas reportadas pela hw5 (%d): %s", g_trip_n, g_trip_last);
    g_wd_now = 520000;
    xwd_tripwire();
    CHECK(g_trip_n == 2, "não repete");
    g_wd_now = 520100;
    xwd_tripwire();
    CHECK(g_trip_n == 2, "250 ms entre olhadas");
    /* pendrive preso */
    xwd_test_pir = 3; xwd_set_hw3_active(1); xwd_beat2("pendrive: gravando", "TOKEN.TXT");
    xwd_set_usb_holder(XWD_HW3);
    xwd_test_pir = 0; xwd_beat("menu");
    xwd_test_pir = 1; xwd_beat("rede: wifi");
    xwd_test_pir = 5;
    g_wd_now = 529000;
    xwd_tripwire();
    CHECK(g_trip_n >= 3 && strstr(g_trip_last, "USB do pendrive está com tarefa (hw3) há 8 s"), "USB preso: %s", g_trip_last);
    xwd_set_usb_holder(-1);
    xwd_set_hw3_active(0);
}

static void test_wdog(void) {
    g_wd_now = 1000;
    xwd_test_pir = 0; xwd_beat("menu");
    xwd_test_pir = 1; xwd_beat("rede: laço");
    xwd_check(XWD_NET, (1u << XWD_MAIN) | (1u << XWD_HW3));
    CHECK(g_wd_n == 0, "tudo andando: nada");
    g_wd_now = 6000;
    xwd_check(XWD_NET, (1u << XWD_MAIN) | (1u << XWD_HW3));
    CHECK(g_wd_n == 0, "5,0 s: ainda não");
    g_wd_now = 6200;
    xwd_check(XWD_NET, (1u << XWD_MAIN) | (1u << XWD_HW3));
    CHECK(g_wd_n == 1 && strstr(g_wd_last, "main (hw0) parada há 5 s em \"menu\"") && g_wd_raw == 1,
          "main parado: %s", g_wd_last);
    g_wd_now = 9000;
    xwd_check(XWD_NET, (1u << XWD_MAIN) | (1u << XWD_HW3));
    CHECK(g_wd_n == 1, "não repete");
    g_wd_now = 16500;
    xwd_check(XWD_NET, (1u << XWD_MAIN) | (1u << XWD_HW3));
    CHECK(g_wd_n == 2 && strstr(g_wd_last, "há 15 s"), "15 s: segundo aviso (%s)", g_wd_last);
    xwd_test_pir = 0; xwd_beat("menu");
    g_wd_now = 16600;
    xwd_check(XWD_NET, (1u << XWD_MAIN) | (1u << XWD_HW3));
    CHECK(g_wd_n == 3 && strstr(g_wd_last, "voltou depois de 15 s"), "voltou: %s", g_wd_last);
    /* hw 3 só conta com tarefa; segurando o pendrive, escrita direta só aos 15 s */
    xwd_test_pir = 3; xwd_beat2("pendrive: gravando", "COVERS/1A2B.JPG");
    xwd_set_usb_holder(XWD_HW3);
    g_wd_now = 30000;
    xwd_test_pir = 0; xwd_beat("menu");
    xwd_check(XWD_NET, 1u << XWD_HW3);
    CHECK(g_wd_n == 3, "hw 3 sem tarefa: não vigia");
    xwd_set_hw3_active(1);
    g_wd_now = 35100;
    xwd_check(XWD_NET, 1u << XWD_HW3);
    CHECK(g_wd_n == 4 && strstr(g_wd_last, "tarefa (hw3) parada há 5 s em \"pendrive: gravando COVERS/1A2B.JPG\"") &&
          strstr(g_wd_last, "USB do pendrive com: tarefa (hw3)") && g_wd_raw == 0,
          "hw3 presa no pendrive, sem escrita direta ainda: %s (raw %d)", g_wd_last, g_wd_raw);
    g_wd_now = 45200;
    xwd_check(XWD_NET, 1u << XWD_HW3);
    CHECK(g_wd_n == 5 && g_wd_raw == 1, "15 s: escrita direta mesmo segurando (raw %d)", g_wd_raw);
    xwd_set_hw3_active(0);
    xwd_set_usb_holder(-1);
    /* main vigia a hw1 */
    xwd_test_pir = 1; xwd_beat("rede: wifi");
    g_wd_now = 51000;
    xwd_check(XWD_MAIN, 1u << XWD_NET);
    CHECK(g_wd_n == 6 && strstr(g_wd_last, "rede (hw1) parada há 5 s em \"rede: wifi\"") && strstr(g_wd_last, "visto por main"),
          "hw1 parada: %s", g_wd_last);
    xwd_test_pir = 2; xwd_beat("decoder");   /* hw2: não é vigiada */
    CHECK(xwd_slot_of_pir() == -1, "hw2 fora");

    /* v0.21.8: main e rede param juntas; só a hw 3 (com tarefa) vê */
    int n0 = g_wd_n;
    g_wd_now = 100000;
    xwd_test_pir = 0; xwd_beat("menu");
    xwd_test_pir = 1; xwd_beat("rede: wifi");
    xwd_set_hw3_active(1);
    xwd_test_pir = 3;
    g_wd_now = 104000;
    xwd_observe();
    CHECK(g_wd_n == n0, "4 s: nada");
    g_wd_now = 105600;
    xwd_observe();
    CHECK(g_wd_n == n0 + 1 && strstr(g_wd_last, "main (hw0) e rede (hw1) paradas") && strstr(g_wd_last, "visto por tarefa (hw3)") &&
          g_wd_raw == 1, "as duas paradas: %s (raw %d)", g_wd_last, g_wd_raw);
    g_wd_now = 106200;
    xwd_observe();
    CHECK(g_wd_n == n0 + 1, "não repete");
    xwd_test_pir = 2;
    g_wd_now = 120000;
    xwd_observe();
    CHECK(g_wd_n == n0 + 1, "fora da hw 3 não observa");
    /* segurando o pendrive (rede presa nele): escrita direta só aos 15 s */
    xwd_set_usb_holder(XWD_NET);
    xwd_test_pir = 3;
    xwd_observe();
    CHECK(g_wd_n == n0 + 2 && strstr(g_wd_last, "USB do pendrive com: rede (hw1)") && g_wd_raw == 1,
          "15 s: segundo aviso com escrita direta: %s (raw %d)", g_wd_last, g_wd_raw);
    xwd_set_usb_holder(-1);
    xwd_test_pir = 0; xwd_beat("menu");
    xwd_test_pir = 3;
    g_wd_now = 121000;
    xwd_observe();
    CHECK(g_wd_n == n0 + 3 && strstr(g_wd_last, "voltaram"), "voltaram: %s", g_wd_last);
    /* sem tarefa na hw 3 (no jogo é do decoder): não observa */
    xwd_set_hw3_active(0);
    g_wd_now = 140000;
    xwd_observe();
    CHECK(g_wd_n == n0 + 3, "hw 3 sem tarefa: não observa");
    /* só uma parada: o par main/rede já cobre, o observador fica quieto */
    xwd_set_hw3_active(1);
    xwd_test_pir = 1; xwd_beat("rede: wifi");
    xwd_test_pir = 3;
    g_wd_now = 141000;
    xwd_observe();
    CHECK(g_wd_n == n0 + 3, "só o main parado: não é com o observador");
    xwd_set_hw3_active(0);
}

static void test_driver_dead(void) {
    printf("--- driver (ROM que nunca manda INIT_DONE)\n");
    fw_reset(1, 0, 0, 0);
    F.no_init_done = 1;
    run(12000);
    CHECK(C.dev == XW_DEV_FAILED && C.err == XW_E_DEVICE, "falha do adaptador (dev=%d)", C.dev);
    CHECK(xw_scan(&C) != 0, "scan recusado com o adaptador morto");
}

int main(int argc, char **argv) {
    if (argc == 7 && !strcmp(argv[1], "ptk")) {
        uint8_t pmk[32], aa[6], spa[6], an[32], sn[32], ptk[48];
        unhex(argv[2], pmk, 32); unhex(argv[3], aa, 6); unhex(argv[4], spa, 6);
        unhex(argv[5], an, 32); unhex(argv[6], sn, 32);
        xw_derive_ptk(pmk, aa, spa, an, sn, ptk);
        char h[200];
        hex(ptk, 48, h);
        printf("%s\n", h);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "pmk")) {
        uint8_t pmk[32];
        if (xw_pmk_from_passphrase(argv[2], (const uint8_t *)argv[3], strlen(argv[3]), pmk)) return 1;
        char h[80];
        hex(pmk, 32, h);
        printf("%s\n", h);
        return 0;
    }
    if (getenv("WIFI_VERBOSE")) g_quiet = 0;
    srand(1234);
    test_crypto();
    test_proto();
    test_store();
    test_txpad();
    test_v0219();
    test_v02110();
    test_textpad();
    test_boot();
    test_wdog();
    test_tripwire();
    test_driver(0, 4);
    test_driver(1, 4);
    test_driver(0, 2);
    test_driver_open();
    test_driver_dead();
    printf("wifi_unit: %d verificações, %d falhas\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
