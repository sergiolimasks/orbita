/*
 * orbita v0.21.0 — driver do Wi-Fi interno (Marvell 88W8786U), máquina de
 * estados em C puro: boot do firmware (ROM -> "MRVL"), init, scan,
 * associação, WPA2 (xw_wpa.h), chaves no firmware, quadros de dados.
 * Não sabe nada de USB nem de lwIP: tudo passa pelos xw_ops_t (no Xbox,
 * xw_xenon.c; no host, o firmware falso de hosttest/wifi). Uma thread só
 * (no Xbox, a de rede, hw 1). Tempo em ms (relógio do chamador).
 */
#ifndef XC360_XW_CORE_H
#define XC360_XW_CORE_H
#include <stddef.h>
#include <stdint.h>

#include "mwx_proto.h"
#include "xw_wpa.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* EP1 OUT (comando já com o prefixo USB). 0 = aceito (copiado). */
    int  (*send_cmd)(void *u, const uint8_t *b, size_t n);
    /* EP2 OUT ([TxPD][Ethernet]). 0 = aceito (copiado), -1 = sem espaço. */
    int  (*send_data)(void *u, const uint8_t *b, size_t n);
    /* quadro Ethernet II recebido com o link de pé (pode alterar f) */
    void (*rx_eth)(void *u, uint8_t *f, size_t n);
    /* link de dados subiu (1) / caiu (0) */
    void (*link)(void *u, int up);
    void (*log)(void *u, const char *line);
    int  (*rand)(void *u, uint8_t *b, size_t n);
    void  *u;
} xw_ops_t;

enum { XW_DEV_OFF = 0, XW_DEV_PROBE, XW_DEV_BOOT, XW_DEV_INIT, XW_DEV_READY, XW_DEV_FAILED };
enum { XW_LINK_IDLE = 0, XW_LINK_SCAN, XW_LINK_ASSOC, XW_LINK_AUTH, XW_LINK_KEYS, XW_LINK_UP, XW_LINK_WAIT };
enum {
    XW_E_NONE = 0,
    XW_E_NOTFOUND,      /* rede salva/escolhida não apareceu */
    XW_E_UNSUPPORTED,   /* WPA3, corporativa, WEP, WPA... */
    XW_E_NEEDPASS,      /* rede passou a pedir senha */
    XW_E_CHANGED,       /* rede salva com senha agora está aberta */
    XW_E_ASSOC,         /* o roteador recusou (err_code = status 802.11) */
    XW_E_AUTH,          /* handshake WPA2 não fechou: senha errada? */
    XW_E_LOST,          /* conexão caiu */
    XW_E_DEVICE,        /* firmware não respondeu / recusou comando */
    XW_E_HANDSHAKE,     /* v0.21.4: WPA2 não fechou SEM prova de senha errada (err_detail diz onde) */
    XW_E_FWTIMEOUT,     /* v0.21.9: o firmware não respondeu a um comando a tempo (não é recusa do roteador) */
};

#define XW_MAX_BSS 32
#define XW_QLEN    8

typedef struct {
    uint8_t  b[400];
    uint16_t n, cmd;
    uint32_t tmo;
} xw_qcmd_t;

typedef struct {
    const xw_ops_t *ops;
    int      dev, link, err, err_code;
    uint32_t now, t_dev, t_link;
    int      probe_tries;
    /* firmware */
    uint8_t  mac[6];
    uint32_t fw_release;
    int      key_v2;
    /* fila de comandos (um por vez no firmware) */
    xw_qcmd_t q[XW_QLEN];
    int      qh, qn;
    int      busy;
    uint16_t busy_cmd, seq;
    uint32_t t_sent, busy_tmo;
    uint8_t  cbuf[MWX_CMD_MAX];
    /* scan: job atual (0 = nenhum, 1 = do usuário, 2 = da conexão) */
    int      job, job_left;
    int      want_user_scan, want_connect;
    mwx_bss_t tmp[XW_MAX_BSS];
    int      ntmp;
    mwx_bss_t bss[XW_MAX_BSS];   /* última lista do usuário (sem repetir SSID, por sinal) */
    int      nbss;
    unsigned scan_seq;
    /* alvo */
    int      want;               /* manter conectado (reconecta sozinho) */
    char     tssid[MWX_SSID_MAX + 1];
    int      tssid_len;
    uint8_t  tpmk[32];
    int      tsecure;
    int      user;               /* 1 = pedido agora pelo usuário (erro de senha não insiste) */
    int      retry, auth_fails;
    uint32_t t_retry;
    mwx_bss_t cur;
    uint8_t  rsn_ie[24];
    size_t   rsn_len;
    xw_sup_t sup;
    /* chaves pendentes (depois da msg 4) */
    xw_sup_out_t keys;
    int      keys_pending, keys_left;
    uint32_t t_keys;
    /* números */
    uint32_t rx_frames, tx_frames, rx_drop, tx_drop, eapol_rx;
    uint32_t tx_padded;              /* v0.21.8: quadros com TxPD de 24 (evitando ZLP) */
    /* v0.21.9: eventos do firmware (todos contados por id; os 3 primeiros de
     * cada id logados) e o tempo de resposta dos comandos */
    uint32_t ev_total;
    uint16_t ev_id[16];
    uint32_t ev_cnt[16];
    int      ev_n;
    uint32_t cmd_total, cmd_slow, cmd_late;
    uint32_t cmd_worst_ms;
    uint16_t cmd_worst;
    int      now_set;
    uint8_t  avoid_bssid[6];  /* v0.21.29: ponto de acesso que não deu DHCP: outro da mesma rede primeiro */
    int      avoid_on;
    int      fast_chan;       /* v0.21.9: próxima busca só neste canal (reconexão rápida) */
    /* v0.21.10: economia de energia (desligada depois de conectar) e
     * estatísticas do firmware (GET_LOG / RSSI_INFO / TX_RATE_QUERY) */
    int      ps_get_result, ps_dis_result;   /* -1 = não respondeu ainda */
    uint16_t ps_bitmap;                      /* do GET */
    unsigned fwq_unsup;                      /* 1 GET_LOG, 2 RSSI_INFO, 4 TX_RATE: firmware recusou, não pede mais */
    unsigned fws_seq;                        /* respostas de GET_LOG recebidas */
    uint32_t fws[19];                        /* GET_LOG: campos do mwifiex na ordem (ver xw_fwstats_text) */
    int      fws_n;
    int16_t  rssi_data_last, rssi_data_avg, rssi_bcn_last, rssi_bcn_avg, nf_bcn_last, snr_bcn_last;
    int      rssi_ok;
    int      tx_rate_idx, tx_rate_ht, rate_ok;
    /* v0.21.4: passos do handshake da tentativa atual (o "senha incorreta" só
     * com prova: MIC da msg 3, ou o AP repetindo a msg 1 / derrubando com
     * motivo 14/15 DEPOIS de a nossa msg 2 sair pelo USB) */
    unsigned hs_msg1, hs_msg2_q, hs_tx_done, hs_msg1_after2, hs_msg3, hs_mic_bad, hs_msg4_q, hs_fmt_bad;
    unsigned rx_log, assoc_status;
    char     err_detail[140];
    uint8_t  pend_eapol[300];   /* v0.21.4: EAPOL que chegou antes da resposta do ASSOCIATE */
    size_t   pend_len;
    int      rssi;
    uint8_t  txbuf[MWX_TXPD_LEN + MWX_TXPD_MAXPAD + 1600];
    char     lb[200];
} xw_core_t;

void xw_init(xw_core_t *c, const xw_ops_t *ops);
/* o adaptador apareceu e os canos estão abertos: começa o probe */
void xw_start(xw_core_t *c, uint32_t now);
void xw_on_cmd_in(xw_core_t *c, const uint8_t *b, size_t n, uint32_t now);
void xw_on_data_in(xw_core_t *c, uint8_t *b, size_t n, uint32_t now);
void xw_tick(xw_core_t *c, uint32_t now);

/* pedidos (só a thread dona) */
int  xw_scan(xw_core_t *c);
/* pmk NULL = rede aberta. user = 1 se veio da tela agora. */
int  xw_connect(xw_core_t *c, const char *ssid, int ssid_len, const uint8_t *pmk, int user);
void xw_disconnect(xw_core_t *c);
/* v0.21.29: a próxima conexão evita o ponto de acesso atual se houver outro com o mesmo nome */
void xw_avoid_current(xw_core_t *c);
/* v0.21.9: reassocia à rede atual, procurando só no canal dela (recuperação) */
int  xw_reconnect(xw_core_t *c);
/* v0.21.10: pede as estatísticas ao firmware (só com o link de pé e a fila
 * vazia). 1 = pediu. */
int  xw_fw_query(xw_core_t *c);
/* texto curto das últimas estatísticas */
void xw_fwstats_text(const xw_core_t *c, char *o, size_t n);
/* v0.21.4: n quadros de dados saíram pelo USB (o glue conta as conclusões do EP2 OUT) */
void xw_note_data_tx(xw_core_t *c, unsigned n);
/* quadro Ethernet II pra rede (só com o link de pé). 0 = foi pra fila. */
int  xw_tx_eth(xw_core_t *c, const uint8_t *f, size_t n);

const char *xw_err_text(int err);
/* v0.21.9: resumo dos eventos ("0x0003x1 0x0017x4") */
void xw_ev_summary(const xw_core_t *c, char *o, size_t n);
extern int xw_tx_nopad_test;   /* testes do host */

#ifdef __cplusplus
}
#endif
#endif
