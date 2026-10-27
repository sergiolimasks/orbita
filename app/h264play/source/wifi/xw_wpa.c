/* orbita v0.21.0 — WPA2-PSK no host. Ver xw_wpa.h. */
#include "xw_wpa.h"

#include <string.h>

#include <mbedtls/aes.h>
#include <mbedtls/md.h>

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

void xw_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *d, size_t n, uint8_t out[20]) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, klen, d, n, out);
}

/* PBKDF2-HMAC-SHA1, 4096 iterações, 2 blocos (32 bytes). O contexto HMAC é
 * reaproveitado (hmac_reset) pra não refazer o ipad/opad a cada iteração. */
int xw_pmk_from_passphrase(const char *pass, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]) {
    size_t pl = pass ? strlen(pass) : 0;
    if (pl < 8 || pl > 63 || ssid_len < 1 || ssid_len > 32) return -1;
    for (size_t i = 0; i < pl; i++)
        if ((unsigned char)pass[i] < 0x20 || (unsigned char)pass[i] > 0x7e) return -1;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1) != 0) {
        mbedtls_md_free(&ctx);
        return -1;
    }
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)pass, pl);
    uint8_t salt[36], u[20], t[20];
    memcpy(salt, ssid, ssid_len);
    for (uint32_t blk = 1; blk <= 2; blk++) {
        salt[ssid_len] = (uint8_t)(blk >> 24);
        salt[ssid_len + 1] = (uint8_t)(blk >> 16);
        salt[ssid_len + 2] = (uint8_t)(blk >> 8);
        salt[ssid_len + 3] = (uint8_t)blk;
        mbedtls_md_hmac_reset(&ctx);
        mbedtls_md_hmac_update(&ctx, salt, ssid_len + 4);
        mbedtls_md_hmac_finish(&ctx, u);
        memcpy(t, u, 20);
        for (int it = 1; it < 4096; it++) {
            mbedtls_md_hmac_reset(&ctx);
            mbedtls_md_hmac_update(&ctx, u, 20);
            mbedtls_md_hmac_finish(&ctx, u);
            for (int k = 0; k < 20; k++) t[k] ^= u[k];
        }
        memcpy(pmk + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
    }
    mbedtls_md_free(&ctx);
    wipe(u, sizeof(u));
    wipe(t, sizeof(t));
    return 0;
}

void xw_prf_sha1(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t dlen,
                 uint8_t *out, size_t out_len) {
    uint8_t in[128], h[20];
    size_t ll = strlen(label);
    if (ll + 1 + dlen + 1 > sizeof(in)) return;
    memcpy(in, label, ll);
    in[ll] = 0;
    memcpy(in + ll + 1, data, dlen);
    size_t n = ll + 1 + dlen;
    for (uint8_t i = 0; out_len > 0; i++) {
        in[n] = i;
        xw_hmac_sha1(key, key_len, in, n + 1, h);
        size_t c = out_len < 20 ? out_len : 20;
        memcpy(out, h, c);
        out += c;
        out_len -= c;
    }
    wipe(h, sizeof(h));
}

void xw_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
                   const uint8_t snonce[32], uint8_t ptk[48]) {
    uint8_t d[76];
    int a = memcmp(aa, spa, 6) < 0;
    memcpy(d, a ? aa : spa, 6);
    memcpy(d + 6, a ? spa : aa, 6);
    int b = memcmp(anonce, snonce, 32) < 0;
    memcpy(d + 12, b ? anonce : snonce, 32);
    memcpy(d + 44, b ? snonce : anonce, 32);
    xw_prf_sha1(pmk, 32, "Pairwise key expansion", d, sizeof(d), ptk, 48);
}

int xw_aes_unwrap(const uint8_t *kek, const uint8_t *cipher, size_t clen, uint8_t *plain) {
    if (clen < 24 || clen % 8) return -1;
    size_t n = clen / 8 - 1;
    uint8_t a[8], b[16];
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_dec(&ctx, kek, 128) != 0) { mbedtls_aes_free(&ctx); return -1; }
    memcpy(a, cipher, 8);
    memmove(plain, cipher + 8, clen - 8);
    for (int j = 5; j >= 0; j--) {
        for (size_t i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (uint64_t)j + i;
            memcpy(b, a, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(b + 8, plain + (i - 1) * 8, 8);
            mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, b, b);
            memcpy(a, b, 8);
            memcpy(plain + (i - 1) * 8, b + 8, 8);
        }
    }
    mbedtls_aes_free(&ctx);
    wipe(b, sizeof(b));
    for (int k = 0; k < 8; k++)
        if (a[k] != 0xa6) return -1;
    return 0;
}

int xw_aes_wrap(const uint8_t *kek, const uint8_t *plain, size_t plen, uint8_t *cipher) {
    if (plen < 16 || plen % 8) return -1;
    size_t n = plen / 8;
    uint8_t a[8], b[16];
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, kek, 128) != 0) { mbedtls_aes_free(&ctx); return -1; }
    memset(a, 0xa6, 8);
    memmove(cipher + 8, plain, plen);
    for (int j = 0; j <= 5; j++) {
        for (size_t i = 1; i <= n; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, cipher + i * 8, 8);
            mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, b, b);
            uint64_t t = (uint64_t)n * (uint64_t)j + i;
            memcpy(a, b, 8);
            for (int k = 0; k < 8; k++) a[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(cipher + i * 8, b + 8, 8);
        }
    }
    memcpy(cipher, a, 8);
    mbedtls_aes_free(&ctx);
    return 0;
}

/* ---------------------------------------------------------- supplicant */

void xw_sup_init(xw_sup_t *s, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                 const uint8_t *rsn_ie, size_t rsn_len, uint8_t group_cipher) {
    int (*r)(void *, uint8_t *, size_t) = s->rand;
    void *ru = s->rand_u;
    wipe(s, sizeof(*s));
    s->rand = r;
    s->rand_u = ru;
    memcpy(s->pmk, pmk, 32);
    memcpy(s->aa, aa, 6);
    memcpy(s->spa, spa, 6);
    if (rsn_len > sizeof(s->rsn_ie)) rsn_len = sizeof(s->rsn_ie);
    memcpy(s->rsn_ie, rsn_ie, rsn_len);
    s->rsn_ie_len = rsn_len;
    s->group_cipher = group_cipher;
}

void xw_sup_wipe(xw_sup_t *s) {
    int (*r)(void *, uint8_t *, size_t) = s->rand;
    void *ru = s->rand_u;
    wipe(s, sizeof(*s));
    s->rand = r;
    s->rand_u = ru;
}

static void mic_of(const uint8_t kck[16], const uint8_t *f, size_t n, uint8_t mic[16]) {
    uint8_t tmp[512], h[20];
    if (n > sizeof(tmp)) n = sizeof(tmp);
    memcpy(tmp, f, n);
    memset(tmp + XW_OFF_MIC, 0, 16);
    xw_hmac_sha1(kck, 16, tmp, n, h);
    memcpy(mic, h, 16);
}

static int mic_ok(const uint8_t kck[16], const uint8_t *f, size_t n) {
    uint8_t m[16];
    mic_of(kck, f, n, m);
    uint8_t d = 0;
    for (int i = 0; i < 16; i++) d |= (uint8_t)(m[i] ^ f[XW_OFF_MIC + i]);
    return d == 0;
}

/* monta uma resposta EAPOL-Key em out->reply com MIC */
static void build_reply(xw_sup_t *s, xw_sup_out_t *o, uint16_t info, const uint8_t replay[8],
                        const uint8_t *nonce, const uint8_t *data, size_t dlen) {
    uint8_t *r = o->reply;
    size_t total = XW_OFF_DATA + dlen;
    memset(r, 0, total);
    r[0] = 1;                       /* 802.1X-2001 (padrão do wpa_supplicant) */
    r[1] = 3;                       /* EAPOL-Key */
    put_be16(r + 2, (uint16_t)(total - XW_EAPOL_HDR));
    r[4] = 2;                       /* descritor RSN */
    put_be16(r + XW_OFF_INFO, info);
    put_be16(r + XW_OFF_KLEN, 0);
    memcpy(r + XW_OFF_REPLAY, replay, 8);
    if (nonce) memcpy(r + XW_OFF_NONCE, nonce, 32);
    put_be16(r + XW_OFF_DLEN, (uint16_t)dlen);
    if (dlen) memcpy(r + XW_OFF_DATA, data, dlen);
    mic_of(s->ptk, r, total, r + XW_OFF_MIC);
    o->reply_len = total;
}

/* KDEs dos dados (já decifrados): acha a GTK */
static int find_gtk(const uint8_t *d, size_t n, xw_sup_out_t *o, uint8_t group_cipher) {
    size_t p = 0;
    while (p + 2 <= n) {
        uint8_t id = d[p], l = d[p + 1];
        if (id == 0xdd && l == 0) break;          /* padding */
        if (p + 2 + l > n) break;
        const uint8_t *v = d + p + 2;
        if (id == 0xdd && l >= 6 && v[0] == 0x00 && v[1] == 0x0f && v[2] == 0xac && v[3] == 1) {
            size_t gl = (size_t)l - 6;
            size_t want = group_cipher == 2 ? 32 : 16;
            if (gl < want) return -1;
            o->gtk_idx = v[4] & 3;
            if (want == 32) {
                /* TKIP: troca as chaves de MIC Tx/Rx (como o wpa_supplicant) */
                memcpy(o->gtk, v + 6, 16);
                memcpy(o->gtk + 16, v + 6 + 24, 8);
                memcpy(o->gtk + 24, v + 6 + 16, 8);
            } else {
                memcpy(o->gtk, v + 6, 16);
            }
            o->gtk_len = want;
            return 0;
        }
        p += 2 + (size_t)l;
    }
    return -1;
}

static int replay_newer(const xw_sup_t *s, const uint8_t *r) {
    if (!s->have_replay) return 1;
    return memcmp(r, s->replay, 8) > 0;
}

static int unwrap_data(const xw_sup_t *s, const uint8_t *f, size_t dlen, uint8_t *plain, size_t *plen) {
    if (dlen < 24 || dlen % 8 || dlen > 256) return -1;
    if (xw_aes_unwrap(s->ptk + 16, f + XW_OFF_DATA, dlen, plain) != 0) return -1;
    *plen = dlen - 8;
    return 0;
}

int xw_sup_rx(xw_sup_t *s, const uint8_t *f, size_t n, xw_sup_out_t *o) {
    memset(o, 0, sizeof(*o));
    if (n < XW_OFF_DATA || f[1] != 3) return XW_SUP_IGNORED;
    size_t total = XW_EAPOL_HDR + be16(f + 2);
    if (total > n || total < XW_OFF_DATA || total > 512) return XW_SUP_ERR_FMT;
    if (f[4] != 2) return XW_SUP_ERR_FMT;                  /* só descritor RSN */
    uint16_t info = be16(f + XW_OFF_INFO);
    if ((info & XW_KI_VER_MASK) != 2) return XW_SUP_ERR_FMT;   /* HMAC-SHA1 + AES */
    size_t dlen = be16(f + XW_OFF_DLEN);
    if (XW_OFF_DATA + dlen > total) return XW_SUP_ERR_FMT;
    const uint8_t *replay = f + XW_OFF_REPLAY;
    uint16_t ver = info & XW_KI_VER_MASK;

    if ((info & XW_KI_PAIRWISE) && (info & XW_KI_ACK) && !(info & XW_KI_MIC)) {
        /* msg 1/4: ANonce; SNonce nova; PTK; responde msg 2/4 com o nosso RSN IE */
        memcpy(s->anonce, f + XW_OFF_NONCE, 32);
        if (!s->rand || s->rand(s->rand_u, s->snonce, 32) != 0) return XW_SUP_ERR_RAND;
        s->have_snonce = 1;
        xw_derive_ptk(s->pmk, s->aa, s->spa, s->anonce, s->snonce, s->ptk);
        s->ptk_ok = 0;
        build_reply(s, o, (uint16_t)(ver | XW_KI_PAIRWISE | XW_KI_MIC), replay, s->snonce, s->rsn_ie, s->rsn_ie_len);
        return XW_SUP_MSG2;
    }
    if ((info & XW_KI_PAIRWISE) && (info & XW_KI_ACK) && (info & XW_KI_MIC)) {
        /* msg 3/4 */
        if (!s->have_snonce || memcmp(s->anonce, f + XW_OFF_NONCE, 32) != 0) return XW_SUP_ERR_FMT;
        if (!mic_ok(s->ptk, f, total)) return XW_SUP_ERR_MIC;
        if (!replay_newer(s, replay)) return XW_SUP_ERR_REPLAY;
        if (!(info & XW_KI_ENCDATA)) return XW_SUP_ERR_FMT;
        uint8_t plain[256];
        size_t pl = 0;
        if (unwrap_data(s, f, dlen, plain, &pl) != 0) return XW_SUP_ERR_FMT;
        int g = find_gtk(plain, pl, o, s->group_cipher);
        wipe(plain, sizeof(plain));
        if (g != 0) return XW_SUP_ERR_FMT;
        memcpy(s->replay, replay, 8);
        s->have_replay = 1;
        memcpy(o->gtk_rsc, f + XW_OFF_RSC, 8);
        memcpy(o->tk, s->ptk + 32, 16);
        s->ptk_ok = 1;
        build_reply(s, o, (uint16_t)(ver | XW_KI_PAIRWISE | XW_KI_MIC | XW_KI_SECURE), replay, NULL, NULL, 0);
        return XW_SUP_MSG4;
    }
    if (!(info & XW_KI_PAIRWISE) && (info & XW_KI_ACK) && (info & XW_KI_MIC)) {
        /* grupo 1/2 (troca periódica da GTK) */
        if (!s->ptk_ok) return XW_SUP_ERR_FMT;
        if (!mic_ok(s->ptk, f, total)) return XW_SUP_ERR_MIC;
        if (!replay_newer(s, replay)) return XW_SUP_ERR_REPLAY;
        if (!(info & XW_KI_ENCDATA)) return XW_SUP_ERR_FMT;
        uint8_t plain[256];
        size_t pl = 0;
        if (unwrap_data(s, f, dlen, plain, &pl) != 0) return XW_SUP_ERR_FMT;
        int g = find_gtk(plain, pl, o, s->group_cipher);
        wipe(plain, sizeof(plain));
        if (g != 0) return XW_SUP_ERR_FMT;
        memcpy(s->replay, replay, 8);
        s->have_replay = 1;
        memcpy(o->gtk_rsc, f + XW_OFF_RSC, 8);
        build_reply(s, o, (uint16_t)(ver | XW_KI_MIC | XW_KI_SECURE), replay, NULL, NULL, 0);
        return XW_SUP_GROUP2;
    }
    return XW_SUP_IGNORED;
}
