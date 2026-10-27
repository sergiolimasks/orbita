/*
 * orbita v0.21.0 — Wi-Fi interno do Xbox 360 S/E (Marvell 88W8786U, USB
 * 045e:0765): protocolo do firmware, C puro (sem libxenon), testado no host
 * (hosttest/wifi). Ver app/h264play/WIFI.md.
 *
 * Derivado do driver mwifiex do Linux (drivers/net/wireless/marvell/mwifiex,
 * GPL-2.0, Copyright 2011-2020 NXP) e do patch Free60Project/linux-xbox360-wifi
 * (boot "MRVL" da ROM): layouts de comando/resposta/evento, TxPD/RxPD, TLVs
 * de scan/associação/chaves. Ver THIRD_PARTY.md.
 *
 * Moldura USB (o adaptador roda o firmware da própria flash):
 *   EP1 OUT: [u32 LE 0xF00DFACE][comando]           (comandos)
 *   EP1 IN : [u32 LE 0xF00DFACE][resposta]  ou  [u32 LE 0xBEEFFACE][u32 evento][corpo]
 *   EP2 OUT: [TxPD 20 B][quadro Ethernet II]          (dados, sem prefixo)
 *   EP2 IN : [RxPD][802.3 + LLC/SNAP ou Ethernet II]  (dados, sem prefixo)
 * Comando: [u16 id][u16 tamanho (cabeçalho incluso)][u16 seq][u16 resultado][corpo],
 * tudo little-endian. Resposta: id | 0x8000, resultado 0 = ok.
 */
#ifndef XC360_MWX_PROTO_H
#define XC360_MWX_PROTO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MWX_VID 0x045e
#define MWX_PID 0x0765

#define MWX_USB_TYPE_CMD   0xF00DFACEu
#define MWX_USB_TYPE_EVENT 0xBEEFFACEu
#define MWX_EP_CMD  1
#define MWX_EP_DATA 2

#define MWX_CMD_GET_HW_SPEC   0x0003
#define MWX_CMD_SCAN          0x0006
#define MWX_CMD_ASSOCIATE     0x0012
#define MWX_CMD_DEAUTH        0x0024
#define MWX_CMD_MAC_CONTROL   0x0028
#define MWX_CMD_KEY_MATERIAL  0x005e
#define MWX_CMD_FUNC_INIT     0x00a9
#define MWX_CMD_FUNC_SHUTDOWN 0x00aa   /* v0.21.9: reinício do firmware (mwifiex_shutdown_drv) */
/* v0.21.10: economia de energia e estatísticas (fw.h do mwifiex) */
#define MWX_CMD_GET_LOG       0x000b   /* host_cmd_ds_802_11_get_log */
#define MWX_CMD_TX_RATE_QUERY 0x007f   /* host_cmd_ds_tx_rate_query */
#define MWX_CMD_RSSI_INFO     0x00a4   /* host_cmd_ds_802_11_rssi_info */
#define MWX_CMD_PS_MODE_ENH   0x00e4   /* host_cmd_ds_802_11_ps_mode_enh */
#define MWX_PS_GET            0x0000
#define MWX_PS_DIS_AUTO       0x00fe
#define MWX_PS_BITMAP_STA     0x0010
#define MWX_CMD_RESP          0x8000

#define MWX_EVT_LINK_LOST     0x03
#define MWX_EVT_INIT_DONE     0x07
#define MWX_EVT_DEAUTH        0x08
#define MWX_EVT_DISASSOC      0x09

#define MWX_MAC_RX_ON   0x0001
#define MWX_MAC_TX_ON   0x0002
#define MWX_MAC_ETH2    0x0010

#define MWX_PREFIX   4              /* tipo USB */
#define MWX_HDR      8              /* cabeçalho do comando */
#define MWX_BODY     (MWX_PREFIX + MWX_HDR)
#define MWX_TXPD_LEN 20
#define MWX_RXPD_MIN 19
#define MWX_CMD_MAX  1024           /* maior comando que montamos */
#define MWX_RX_CMD_BUF  2048
#define MWX_RX_DATA_BUF 4096
#define MWX_HW_SPEC_LEN 63

/* tipo de chave do firmware */
enum { MWX_KEY_TKIP = 1, MWX_KEY_AES = 2 };

/* ---- segurança de uma rede (classificação do scan) ---- */
enum {
    MWX_SEC_OPEN = 0,
    MWX_SEC_WPA2,        /* WPA2-PSK, par CCMP: suportado */
    MWX_SEC_WEP,         /* não suportado */
    MWX_SEC_WPA1,        /* WPA (TKIP) sem RSN: não suportado */
    MWX_SEC_WPA3,        /* só SAE / PSK-SHA256 / PMF obrigatório: não suportado */
    MWX_SEC_ENTERPRISE,  /* 802.1X: não suportado */
    MWX_SEC_OTHER,       /* RSN sem CCMP no par etc.: não suportado */
};

#define MWX_SSID_MAX 32
#define MWX_RATES_MAX 14
typedef struct {
    uint8_t  bssid[6];
    int      rssi;            /* dBm (negativo) */
    uint16_t cap;
    uint16_t beacon_period;
    uint8_t  channel;
    uint8_t  ssid_len;
    char     ssid[MWX_SSID_MAX + 1];
    uint8_t  rates[MWX_RATES_MAX];
    uint8_t  nrates;
    uint8_t  sec;             /* MWX_SEC_* */
    uint8_t  group_cipher;    /* 2 = TKIP, 4 = CCMP (sufixo da suíte 00-0F-AC) */
} mwx_bss_t;

/* little-endian */
static inline void mwx_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void mwx_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t mwx_get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t mwx_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- boot (patch Free60 linux-xbox360-wifi) ---- */
/* FUNC_INIT com moldura: a ROM responde 8 bytes (1ª palavra 1), o firmware
 * rodando responde com 0xF00DFACE (ou manda um evento). 12 bytes. */
size_t mwx_build_probe(uint8_t *b);
/* 16 bytes "MRVL" + 1: a ROM sobe o firmware da flash e manda INIT_DONE. */
size_t mwx_build_mrvl(uint8_t *b);
/* resposta ao probe: 0 = firmware rodando, 1 = ROM, -1 = não sei */
int    mwx_probe_classify(const uint8_t *b, size_t n);

/* ---- comandos: montam em b (>= MWX_CMD_MAX) com o prefixo USB, devolvem o tamanho ---- */
size_t mwx_build_func_init(uint8_t *b, uint16_t seq);
size_t mwx_build_func_shutdown(uint8_t *b, uint16_t seq);
/* v0.21.10: comando genérico (corpo copiado) */
size_t mwx_build_cmd(uint8_t *b, uint16_t seq, uint16_t cmd, const void *body, size_t n);
size_t mwx_build_get_hw_spec(uint8_t *b, uint16_t seq);
size_t mwx_build_mac_control(uint8_t *b, uint16_t seq, uint32_t action);
/* chans[n] (2,4 GHz); ssid NULL/len 0 = coringa. active_ms por canal. */
size_t mwx_build_scan(uint8_t *b, uint16_t seq, const uint8_t *chans, int n, int active_ms,
                      const char *ssid, int ssid_len);
/* rsn_ie = IE completo (0x30 len ...) ou NULL (rede aberta) */
size_t mwx_build_assoc(uint8_t *b, uint16_t seq, const mwx_bss_t *bss, const uint8_t *rsn_ie, size_t rsn_len);
size_t mwx_build_deauth(uint8_t *b, uint16_t seq, const uint8_t bssid[6], uint16_t reason);
/* chave v1 (firmware antigo, sem TLV de API) */
size_t mwx_build_key_v1(uint8_t *b, uint16_t seq, int type, const uint8_t *key, size_t klen, int unicast);
/* chave v2 (API de chave 2.x, do TLV do GET_HW_SPEC). mac = BSSID (par) ou ff..ff (grupo) */
size_t mwx_build_key_v2(uint8_t *b, uint16_t seq, int type, const uint8_t *key, size_t klen, int unicast,
                        const uint8_t mac[6], int key_idx, const uint8_t rsc[6]);

/* ---- respostas ---- */
typedef struct {
    uint16_t cmd;      /* sem o 0x8000 */
    uint16_t seq;
    uint16_t result;
    const uint8_t *body;
    size_t   body_len;
} mwx_resp_t;
/* Pacote do EP1 IN. Devolve 1 = resposta, 2 = evento (*evt), 0 = outro/lixo. */
int mwx_parse_cmd_ep(const uint8_t *b, size_t n, mwx_resp_t *r, uint32_t *evt,
                     const uint8_t **evt_body, size_t *evt_len);

typedef struct {
    uint8_t  mac[6];
    uint16_t region;
    uint32_t fw_release;
    uint32_t fw_cap;
    int      key_api_major, key_api_minor;   /* 0 = sem TLV (v1) */
} mwx_hw_spec_t;
int mwx_parse_hw_spec(const uint8_t *body, size_t n, mwx_hw_spec_t *o);

/* Resposta do SCAN: chama cb pra cada BSS (já classificado). Devolve o nº. */
int mwx_parse_scan_rsp(const uint8_t *body, size_t n, void (*cb)(void *u, const mwx_bss_t *b), void *u);
/* Classifica a partir dos IEs (exposto pros testes). */
void mwx_parse_ies(mwx_bss_t *o, const uint8_t *ie, size_t n);

/* ASSOCIATE: status IEEE 802.11 (0 = ok); cap 0xFFFF + status = falha interna do firmware */
int mwx_parse_assoc_rsp(const uint8_t *body, size_t n, uint16_t *status, uint16_t *cap, uint16_t *aid);

/* ---- dados ---- */
/* TxPD em b (20 bytes) pra um quadro Ethernet de flen bytes logo depois. */
void mwx_build_txpd(uint8_t *b, size_t flen);
/* v0.21.8: TxPD com o quadro em `off` (20..83; bytes entre o TxPD e o quadro
 * zerados), como o pad de alinhamento do mwifiex. Ver mwx_txpd_offset. */
void mwx_build_txpd_off(uint8_t *b, size_t flen, size_t off);
/* offset que deixa a transferência USB (off + flen) FORA de múltiplo de 64:
 * assim nunca sai um ZLP no EP2 OUT (o firmware da flash do 88W8786U trata o
 * pacote vazio como uma transferência nova e embaralha o TX dali em diante). */
size_t mwx_txpd_offset(size_t flen);
#define MWX_TXPD_MAXPAD 4
/* RxPD -> quadro Ethernet II (converte 802.3+SNAP no lugar, b é alterado).
 * 0 = ok (*f, *flen), -1 = descartar. */
int  mwx_rx_to_eth(uint8_t *b, size_t n, uint8_t **f, size_t *flen, int *snr);

/* nossas taxas b/g (sem o bit de básica) */
extern const uint8_t mwx_rates_bg[12];

/* IE RSN que mandamos (associação e msg 2/4): CCMP par, PSK, grupo do AP. */
size_t mwx_build_rsn_ie(uint8_t *ie, uint8_t group_cipher);

const char *mwx_sec_name(int sec);

#ifdef __cplusplus
}
#endif
#endif
