/*
 * orbita v0.21.0 — WPA2-PSK no host (o firmware do 88W8786U do Xbox não tem
 * supplicant embutido utilizável: o Linux do Free60 usa o wpa_supplicant com
 * chaves instaladas por KEY_MATERIAL). C puro + mbedTLS (HMAC-SHA1, AES).
 *
 *   PMK  = PBKDF2-HMAC-SHA1(senha, SSID, 4096, 32)     (802.11 J.4)
 *   PTK  = PRF-384(PMK, "Pairwise key expansion", min/max(AA,SPA) || min/max(ANonce,SNonce))
 *          KCK = PTK[0:16], KEK = PTK[16:32], TK = PTK[32:48] (CCMP)
 *   MIC  = HMAC-SHA1(KCK, quadro EAPOL com MIC zerado)[0:16]   (descritor v2)
 *   dados chave da msg 3 / grupo: AES key unwrap (RFC 3394) com o KEK
 *
 * Handshake de 4 vias (msg 1..4) e de grupo (rekey, 2 mensagens). Só par CCMP
 * com AKM PSK (00-0F-AC:2), grupo CCMP ou TKIP. Nada aqui loga segredo.
 */
#ifndef XC360_XW_WPA_H
#define XC360_XW_WPA_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* senha 8..63 ASCII imprimível. 0 = ok. */
int  xw_pmk_from_passphrase(const char *pass, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]);
/* PRF do 802.11 (HMAC-SHA1), out_len qualquer. */
void xw_prf_sha1(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t dlen,
                 uint8_t *out, size_t out_len);
void xw_derive_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
                   const uint8_t snonce[32], uint8_t ptk[48]);
/* RFC 3394. clen múltiplo de 8 (>= 24). plain recebe clen-8. 0 = ok (IV confere). */
int  xw_aes_unwrap(const uint8_t *kek, const uint8_t *cipher, size_t clen, uint8_t *plain);
/* RFC 3394 wrap (o autenticador falso dos testes usa). */
int  xw_aes_wrap(const uint8_t *kek, const uint8_t *plain, size_t plen, uint8_t *cipher);
void xw_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *d, size_t n, uint8_t out[20]);

/* ---- EAPOL-Key (802.1X-2004) ---- */
#define XW_EAPOL_HDR      4
#define XW_KEY_FIXED      95       /* do tipo do descritor até o tamanho dos dados */
#define XW_KI_VER_MASK    0x0007
#define XW_KI_PAIRWISE    0x0008
#define XW_KI_INSTALL     0x0040
#define XW_KI_ACK         0x0080
#define XW_KI_MIC         0x0100
#define XW_KI_SECURE      0x0200
#define XW_KI_ERROR       0x0400
#define XW_KI_REQUEST     0x0800
#define XW_KI_ENCDATA     0x1000
/* offsets no quadro EAPOL (a partir do byte de versão) */
#define XW_OFF_INFO   5
#define XW_OFF_KLEN   7
#define XW_OFF_REPLAY 9
#define XW_OFF_NONCE  17
#define XW_OFF_IV     49
#define XW_OFF_RSC    65
#define XW_OFF_MIC    81
#define XW_OFF_DLEN   97
#define XW_OFF_DATA   99

typedef struct {
    uint8_t pmk[32];
    uint8_t aa[6], spa[6];
    uint8_t anonce[32], snonce[32];
    uint8_t ptk[48];
    uint8_t replay[8];
    int     have_replay, have_snonce, ptk_ok;
    uint8_t rsn_ie[64];
    size_t  rsn_ie_len;
    uint8_t group_cipher;      /* 2 TKIP, 4 CCMP */
    int (*rand)(void *u, uint8_t *b, size_t n);
    void   *rand_u;
} xw_sup_t;

enum {
    XW_SUP_IGNORED = 0,
    XW_SUP_MSG2,          /* reply = msg 2/4 */
    XW_SUP_MSG4,          /* reply = msg 4/4; instalar TK + GTK depois de enviar */
    XW_SUP_GROUP2,        /* reply = msg 2/2 do grupo; instalar GTK */
    XW_SUP_ERR_MIC = -1,  /* MIC errado (outro PMK = senha diferente?) */
    XW_SUP_ERR_FMT = -2,
    XW_SUP_ERR_REPLAY = -3,
    XW_SUP_ERR_RAND = -4,
};

typedef struct {
    uint8_t reply[160];
    size_t  reply_len;
    uint8_t tk[16];
    uint8_t gtk[32];
    size_t  gtk_len;
    int     gtk_idx;
    uint8_t gtk_rsc[8];
} xw_sup_out_t;

void xw_sup_init(xw_sup_t *s, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                 const uint8_t *rsn_ie, size_t rsn_len, uint8_t group_cipher);
/* eapol = carga Ethernet (tipo 0x888E). Devolve XW_SUP_*. */
int  xw_sup_rx(xw_sup_t *s, const uint8_t *eapol, size_t n, xw_sup_out_t *out);
/* zera tudo (PMK/PTK) */
void xw_sup_wipe(xw_sup_t *s);

#ifdef __cplusplus
}
#endif
#endif
