/* orbita v0.21.0 — protocolo do firmware Marvell (derivado do mwifiex do
 * Linux, GPL-2.0; ver mwx_proto.h e THIRD_PARTY.md). */
#include "mwx_proto.h"

#include <string.h>

const uint8_t mwx_rates_bg[12] = { 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };

#define TLV_SSID        0x0000
#define TLV_RATES       0x0001
#define TLV_DS          0x0003
#define TLV_CF          0x0004
#define TLV_KEY         0x0100
#define TLV_CHANLIST    0x0101
#define TLV_WILDSSID    0x0112
#define TLV_AUTH_TYPE   0x011f
#define TLV_API_REV     0x01c7
#define TLV_KEY_V2      0x019c
#define CAPINFO_MASK    ((uint16_t)~((1u << 15) | (1u << 14) | (1u << 12) | (1u << 11) | (1u << 9)))

/* ------------------------------------------------------------- boot */

size_t mwx_build_probe(uint8_t *b) {
    mwx_put32(b, MWX_USB_TYPE_CMD);
    mwx_put32(b + 4, MWX_CMD_FUNC_INIT | (8u << 16));
    mwx_put32(b + 8, 1);
    return 12;
}

size_t mwx_build_mrvl(uint8_t *b) {
    memset(b, 0, 16);
    memcpy(b, "MRVL", 4);
    b[4] = 1;
    return 16;
}

int mwx_probe_classify(const uint8_t *b, size_t n) {
    if (n < 4) return -1;
    uint32_t t = mwx_get32(b);
    if (t == MWX_USB_TYPE_CMD || t == MWX_USB_TYPE_EVENT) return 0;
    if (n == 8 && t == 1) return 1;
    return -1;
}

/* ---------------------------------------------------------- comandos */

static size_t cmd_begin(uint8_t *b, uint16_t cmd, uint16_t seq) {
    memset(b, 0, MWX_BODY);
    mwx_put32(b, MWX_USB_TYPE_CMD);
    mwx_put16(b + 4, cmd);
    mwx_put16(b + 8, seq);
    return MWX_BODY;
}

static size_t cmd_end(uint8_t *b, size_t total) {
    mwx_put16(b + 6, (uint16_t)(total - MWX_PREFIX));
    return total;
}

static size_t tlv(uint8_t *p, uint16_t type, const void *v, size_t n) {
    mwx_put16(p, type);
    mwx_put16(p + 2, (uint16_t)n);
    if (n && v) memcpy(p + 4, v, n);
    else if (n) memset(p + 4, 0, n);
    return 4 + n;
}

size_t mwx_build_func_init(uint8_t *b, uint16_t seq) {
    return cmd_end(b, cmd_begin(b, MWX_CMD_FUNC_INIT, seq));
}

size_t mwx_build_cmd(uint8_t *b, uint16_t seq, uint16_t cmd, const void *body, size_t n) {
    size_t o = cmd_begin(b, cmd, seq);
    if (n > MWX_CMD_MAX - o) n = MWX_CMD_MAX - o;
    if (n) {
        if (body) memcpy(b + o, body, n);
        else memset(b + o, 0, n);
    }
    return cmd_end(b, o + n);
}

size_t mwx_build_func_shutdown(uint8_t *b, uint16_t seq) {
    return cmd_end(b, cmd_begin(b, MWX_CMD_FUNC_SHUTDOWN, seq));
}

size_t mwx_build_get_hw_spec(uint8_t *b, uint16_t seq) {
    size_t o = cmd_begin(b, MWX_CMD_GET_HW_SPEC, seq);
    memset(b + o, 0, MWX_HW_SPEC_LEN);
    return cmd_end(b, o + MWX_HW_SPEC_LEN);
}

size_t mwx_build_mac_control(uint8_t *b, uint16_t seq, uint32_t action) {
    size_t o = cmd_begin(b, MWX_CMD_MAC_CONTROL, seq);
    mwx_put32(b + o, action);
    return cmd_end(b, o + 4);
}

size_t mwx_build_scan(uint8_t *b, uint16_t seq, const uint8_t *chans, int n, int active_ms,
                      const char *ssid, int ssid_len) {
    size_t o = cmd_begin(b, MWX_CMD_SCAN, seq);
    if (n > 14) n = 14;
    if (ssid_len < 0 || !ssid) ssid_len = 0;
    if (ssid_len > MWX_SSID_MAX) ssid_len = MWX_SSID_MAX;
    b[o++] = 3;                     /* HostCmd_BSS_MODE_ANY */
    memset(b + o, 0, 6); o += 6;    /* sem filtro de BSSID */
    /* SSID: max_ssid_length 0 = procura exatamente este; 32 = coringa */
    uint8_t ws[1 + MWX_SSID_MAX];
    ws[0] = ssid_len ? 0 : MWX_SSID_MAX;
    if (ssid_len) memcpy(ws + 1, ssid, (size_t)ssid_len);
    o += tlv(b + o, TLV_WILDSSID, ws, 1 + (size_t)ssid_len);
    /* canais: rádio BG, ativo, sem filtro de canal (DISABLE_CHAN_FILT) */
    mwx_put16(b + o, TLV_CHANLIST);
    mwx_put16(b + o + 2, (uint16_t)(7 * n));
    o += 4;
    for (int i = 0; i < n; i++) {
        b[o] = 0;
        b[o + 1] = chans[i];
        b[o + 2] = 0x02;
        mwx_put16(b + o + 3, (uint16_t)active_ms);
        mwx_put16(b + o + 5, (uint16_t)active_ms);
        o += 7;
    }
    o += tlv(b + o, TLV_RATES, mwx_rates_bg, sizeof(mwx_rates_bg));
    return cmd_end(b, o);
}

size_t mwx_build_rsn_ie(uint8_t *ie, uint8_t group_cipher) {
    static const uint8_t tmpl[22] = {
        0x30, 20, 0x01, 0x00,
        0x00, 0x0f, 0xac, 0x04,          /* grupo (trocado abaixo) */
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,
        0x00, 0x00 };
    memcpy(ie, tmpl, sizeof(tmpl));
    ie[7] = group_cipher ? group_cipher : 4;
    return sizeof(tmpl);
}

size_t mwx_build_assoc(uint8_t *b, uint16_t seq, const mwx_bss_t *bss, const uint8_t *rsn_ie, size_t rsn_len) {
    size_t o = cmd_begin(b, MWX_CMD_ASSOCIATE, seq);
    memcpy(b + o, bss->bssid, 6); o += 6;
    mwx_put16(b + o, (uint16_t)(bss->cap & CAPINFO_MASK)); o += 2;
    mwx_put16(b + o, 10); o += 2;                      /* listen interval */
    mwx_put16(b + o, bss->beacon_period); o += 2;
    b[o++] = 0;                                        /* dtim */
    o += tlv(b + o, TLV_SSID, bss->ssid, bss->ssid_len);
    o += tlv(b + o, TLV_DS, &bss->channel, 1);
    o += tlv(b + o, TLV_CF, NULL, 6);
    /* taxas em comum (com o bit de básica do AP) */
    uint8_t r[MWX_RATES_MAX];
    int nr = 0;
    for (int i = 0; i < bss->nrates && nr < MWX_RATES_MAX; i++)
        for (int j = 0; j < 12; j++)
            if ((bss->rates[i] & 0x7f) == mwx_rates_bg[j]) { r[nr++] = bss->rates[i]; break; }
    if (nr == 0) { memcpy(r, mwx_rates_bg, 12); nr = 12; }
    o += tlv(b + o, TLV_RATES, r, (size_t)nr);
    uint8_t auth[2] = { 0, 0 };                        /* open system */
    o += tlv(b + o, TLV_AUTH_TYPE, auth, 2);
    if (rsn_ie && rsn_len >= 2) o += tlv(b + o, rsn_ie[0], rsn_ie + 2, rsn_ie[1]);
    return cmd_end(b, o);
}

size_t mwx_build_deauth(uint8_t *b, uint16_t seq, const uint8_t bssid[6], uint16_t reason) {
    size_t o = cmd_begin(b, MWX_CMD_DEAUTH, seq);
    memcpy(b + o, bssid, 6);
    mwx_put16(b + o + 6, reason);
    return cmd_end(b, o + 8);
}

size_t mwx_build_key_v1(uint8_t *b, uint16_t seq, int type, const uint8_t *key, size_t klen, int unicast) {
    size_t o = cmd_begin(b, MWX_CMD_KEY_MATERIAL, seq);
    mwx_put16(b + o, 1); o += 2;                       /* SET */
    mwx_put16(b + o, TLV_KEY);
    mwx_put16(b + o + 2, (uint16_t)(klen + 6));
    mwx_put16(b + o + 4, (uint16_t)type);
    mwx_put16(b + o + 6, (uint16_t)(0x0004 | (unicast ? 0x0002 : 0x0001)));   /* ENABLED | UNICAST/MCAST */
    mwx_put16(b + o + 8, (uint16_t)klen);
    memcpy(b + o + 10, key, klen);
    return cmd_end(b, o + 10 + klen);
}

size_t mwx_build_key_v2(uint8_t *b, uint16_t seq, int type, const uint8_t *key, size_t klen, int unicast,
                        const uint8_t mac[6], int key_idx, const uint8_t rsc[6]) {
    size_t o = cmd_begin(b, MWX_CMD_KEY_MATERIAL, seq);
    mwx_put16(b + o, 1); o += 2;
    const size_t plen = 10 + 42;                       /* fixo + {pn[8], key_len, key[32]} */
    mwx_put16(b + o, TLV_KEY_V2);
    mwx_put16(b + o + 2, (uint16_t)plen);
    uint8_t *p = b + o + 4;
    memset(p, 0, plen);
    memcpy(p, mac, 6);
    p[6] = (uint8_t)(key_idx & 0x0f);
    p[7] = (uint8_t)type;
    uint16_t info = 0x0004 | 0x0008 | (unicast ? (0x0002 | 0x0010 | 0x0020) : (0x0001 | 0x0020));
    mwx_put16(p + 8, info);
    if (rsc) memcpy(p + 10, rsc, 6);
    mwx_put16(p + 18, (uint16_t)klen);
    memcpy(p + 20, key, klen > 32 ? 32 : klen);
    return cmd_end(b, o + 4 + plen);
}

/* ---------------------------------------------------------- respostas */

int mwx_parse_cmd_ep(const uint8_t *b, size_t n, mwx_resp_t *r, uint32_t *evt,
                     const uint8_t **evt_body, size_t *evt_len) {
    if (n < 8) return 0;
    uint32_t t = mwx_get32(b);
    if (t == MWX_USB_TYPE_CMD) {
        if (n < MWX_BODY) return 0;
        uint16_t cmd = mwx_get16(b + 4);
        if (!(cmd & MWX_CMD_RESP)) return 0;
        size_t size = mwx_get16(b + 6);
        if (size < MWX_HDR) return 0;
        if (size > n - MWX_PREFIX) size = n - MWX_PREFIX;
        r->cmd = (uint16_t)(cmd & 0x7fff);
        r->seq = mwx_get16(b + 8);
        r->result = mwx_get16(b + 10);
        r->body = b + MWX_BODY;
        r->body_len = size - MWX_HDR;
        return 1;
    }
    if (t == MWX_USB_TYPE_EVENT) {
        *evt = mwx_get32(b + 4);
        *evt_body = b + 8;
        *evt_len = n - 8;
        return 2;
    }
    return 0;
}

int mwx_parse_hw_spec(const uint8_t *body, size_t n, mwx_hw_spec_t *o) {
    memset(o, 0, sizeof(*o));
    if (n < 38) return -1;
    memcpy(o->mac, body + 8, 6);
    o->region = mwx_get16(body + 14);
    o->fw_release = mwx_get32(body + 18);
    o->fw_cap = mwx_get32(body + 34);
    size_t p = MWX_HW_SPEC_LEN;
    while (p + 4 <= n) {
        uint16_t t = mwx_get16(body + p), l = mwx_get16(body + p + 2);
        if (p + 4 + l > n) break;
        if (t == TLV_API_REV && l >= 4 && mwx_get16(body + p + 4) == 1) {
            o->key_api_major = body[p + 6];
            o->key_api_minor = body[p + 7];
        }
        p += 4 + l;
    }
    return 0;
}

static int has_suite(const uint8_t *s, int cnt, size_t avail, uint8_t type) {
    for (int i = 0; i < cnt && (size_t)(i + 1) * 4 <= avail; i++)
        if (s[i * 4] == 0x00 && s[i * 4 + 1] == 0x0f && s[i * 4 + 2] == 0xac && s[i * 4 + 3] == type) return 1;
    return 0;
}

static void classify_rsn(mwx_bss_t *o, const uint8_t *d, size_t n) {
    /* padrões do 802.11 quando o IE é curto */
    uint8_t group = 4;
    int pair_ccmp = 1, akm_psk = 0, akm_psk256 = 0, akm_sae = 0, akm_1x = 1, mfpr = 0;
    size_t p = 2;                       /* versão */
    if (n >= p + 4) {
        if (d[p] == 0x00 && d[p + 1] == 0x0f && d[p + 2] == 0xac) group = d[p + 3];
        else group = 0xff;
        p += 4;
    }
    if (n >= p + 2) {
        int c = mwx_get16(d + p);
        p += 2;
        pair_ccmp = has_suite(d + p, c, n - p, 4);
        p += (size_t)c * 4;
        if (n >= p + 2) {
            int a = mwx_get16(d + p);
            p += 2;
            if (p <= n) {
                akm_1x = has_suite(d + p, a, n - p, 1) || has_suite(d + p, a, n - p, 3) ||
                         has_suite(d + p, a, n - p, 5);
                akm_psk = has_suite(d + p, a, n - p, 2);
                akm_psk256 = has_suite(d + p, a, n - p, 6);
                akm_sae = has_suite(d + p, a, n - p, 8);
            }
            p += (size_t)a * 4;
            if (n >= p + 2) mfpr = (mwx_get16(d + p) & 0x0040) != 0;
        }
    }
    o->group_cipher = group;
    if (akm_psk && pair_ccmp && !mfpr && (group == 2 || group == 4)) o->sec = MWX_SEC_WPA2;
    else if (akm_sae || akm_psk256 || (akm_psk && mfpr)) o->sec = MWX_SEC_WPA3;
    else if (akm_1x) o->sec = MWX_SEC_ENTERPRISE;
    else o->sec = MWX_SEC_OTHER;
}

void mwx_parse_ies(mwx_bss_t *o, const uint8_t *ie, size_t n) {
    int rsn = 0, wpa1 = 0;
    size_t p = 0;
    o->nrates = 0;
    while (p + 2 <= n) {
        uint8_t id = ie[p], l = ie[p + 1];
        if (p + 2 + l > n) break;
        const uint8_t *d = ie + p + 2;
        switch (id) {
        case 0:
            if (l <= MWX_SSID_MAX) {
                memcpy(o->ssid, d, l);
                o->ssid[l] = 0;
                o->ssid_len = l;
            }
            break;
        case 1: case 50:
            for (int i = 0; i < l && o->nrates < MWX_RATES_MAX; i++) o->rates[o->nrates++] = d[i];
            break;
        case 3:
            if (l >= 1) o->channel = d[0];
            break;
        case 48:
            rsn = 1;
            classify_rsn(o, d, l);
            break;
        case 221:
            if (l >= 4 && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xf2 && d[3] == 0x01) wpa1 = 1;
            break;
        default:
            break;
        }
        p += 2 + (size_t)l;
    }
    if (!rsn) {
        o->group_cipher = 0;
        o->sec = wpa1 ? MWX_SEC_WPA1 : (o->cap & 0x0010) ? MWX_SEC_WEP : MWX_SEC_OPEN;
    }
}

int mwx_parse_scan_rsp(const uint8_t *body, size_t n, void (*cb)(void *u, const mwx_bss_t *b), void *u) {
    if (n < 3) return 0;
    size_t total = mwx_get16(body);
    int sets = body[2];
    const uint8_t *p = body + 3;
    if (total > n - 3) total = n - 3;
    int found = 0;
    for (int i = 0; i < sets && total >= 2; i++) {
        size_t bl = mwx_get16(p);
        p += 2; total -= 2;
        if (bl == 0 || bl > total) break;
        if (bl >= 6 + 1 + 12) {
            mwx_bss_t b;
            memset(&b, 0, sizeof(b));
            memcpy(b.bssid, p, 6);
            b.rssi = -(int)p[6];
            b.beacon_period = mwx_get16(p + 7 + 8);
            b.cap = mwx_get16(p + 7 + 10);
            mwx_parse_ies(&b, p + 19, bl - 19);
            if (cb) cb(u, &b);
            found++;
        }
        p += bl; total -= bl;
    }
    return found;
}

int mwx_parse_assoc_rsp(const uint8_t *body, size_t n, uint16_t *status, uint16_t *cap, uint16_t *aid) {
    if (n < 6) return -1;
    *cap = mwx_get16(body);
    *status = mwx_get16(body + 2);
    *aid = (uint16_t)(mwx_get16(body + 4) & 0x3fff);
    return 0;
}

/* ---------------------------------------------------------------- dados */

size_t mwx_txpd_offset(size_t flen) {
    return ((MWX_TXPD_LEN + flen) % 64) == 0 ? MWX_TXPD_LEN + MWX_TXPD_MAXPAD : MWX_TXPD_LEN;
}

void mwx_build_txpd_off(uint8_t *b, size_t flen, size_t off) {
    memset(b, 0, off);
    mwx_put16(b + 2, (uint16_t)flen);
    mwx_put16(b + 4, (uint16_t)off);
}

void mwx_build_txpd(uint8_t *b, size_t flen) {
    memset(b, 0, MWX_TXPD_LEN);
    mwx_put16(b + 2, (uint16_t)flen);
    mwx_put16(b + 4, MWX_TXPD_LEN);
}

int mwx_rx_to_eth(uint8_t *b, size_t n, uint8_t **f, size_t *flen, int *rssi) {
    if (n < MWX_RXPD_MIN) return -1;
    uint16_t len = mwx_get16(b + 2), off = mwx_get16(b + 4), type = mwx_get16(b + 6);
    if (rssi) *rssi = (int)(int8_t)b[12] + (int)(int8_t)b[13];
    /* v0.21.6: o firmware da flash do 88W8786U (43.12.135.p3) marca dado
     * Ethernet II como tipo 2 (log do console: EAPOL 0x888e com RxPD tipo
     * 0x0002); o mwifiex usa 0. Os dois são dado; o resto (gerência, AMSDU,
     * BAR...) não pedimos. */
    if (type != 0 && type != 2) return -1;
    if ((size_t)off + len > n || len < 14) return -1;
    uint8_t *pkt = b + off;
    static const uint8_t rfc1042[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    static const uint8_t bridge[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8 };
    if (len >= 22 && (!memcmp(pkt + 14, bridge, 6) ||
                      (!memcmp(pkt + 14, rfc1042, 6) &&
                       !(pkt[20] == 0x80 && pkt[21] == 0xf3) && !(pkt[20] == 0x81 && pkt[21] == 0x37)))) {
        uint8_t da[6], sa[6];
        memcpy(da, pkt, 6);
        memcpy(sa, pkt + 6, 6);
        memcpy(pkt + 8, da, 6);
        memcpy(pkt + 14, sa, 6);
        *f = pkt + 8;
        *flen = (size_t)len - 8;
    } else {
        *f = pkt;
        *flen = len;
    }
    return 0;
}

const char *mwx_sec_name(int sec) {
    switch (sec) {
    case MWX_SEC_OPEN: return "aberta";
    case MWX_SEC_WPA2: return "WPA2";
    case MWX_SEC_WEP: return "WEP";
    case MWX_SEC_WPA1: return "WPA";
    case MWX_SEC_WPA3: return "WPA3";
    case MWX_SEC_ENTERPRISE: return "corporativa";
    default: return "outra";
    }
}
