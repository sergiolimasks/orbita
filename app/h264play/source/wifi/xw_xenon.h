/*
 * orbita v0.21.0 — Wi-Fi interno do Xbox 360 S/E no app (libxenon). Ver WIFI.md.
 *
 * Donos:
 *   - xw_sys_init: main, ANTES da thread de rede (lê XC360/WIFI.TXT, semeia o DRBG).
 *   - xw_sys_step: SÓ a thread de rede (hw 1), logo depois do usb_do_poll e
 *     fora do xc_usb_hold: USB (OHCI), driver (xw_core), netif "wl" do lwIP, DHCP.
 *   - o resto (status/pedidos): qualquer thread (mailbox com lock).
 * O Ethernet não muda: o Wi-Fi só liga se houver adaptador E rede salva, e só
 * vira a rota padrão quando o cabo não pegou DHCP no boot.
 */
#ifndef XC360_XW_XENON_H
#define XC360_XW_XENON_H
#include <stdint.h>

#include "xw_core.h"
#include "xw_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    ssid[MWX_SSID_MAX + 1];
    int     ssid_len;
    int     rssi;
    int     sec;       /* MWX_SEC_* */
    int     chan;
} xw_net_t;

enum { XW_PRESENT_UNKNOWN = -1, XW_PRESENT_NO = 0, XW_PRESENT_YES = 1 };

typedef struct {
    unsigned seq;
    int      present;          /* XW_PRESENT_* */
    int      dev, link, err, err_code;
    int      want;             /* tentando ficar conectado */
    int      dhcp_ok;
    uint32_t ip;               /* a<<24|b<<16|c<<8|d */
    int      is_default;       /* o tráfego do app sai pelo Wi-Fi */
    int      cable;            /* o cabo pegou DHCP no boot */
    char     ssid[MWX_SSID_MAX + 1];   /* alvo/conectado */
    int      rssi;
    int      saved;
    int      saved_secure;
    char     saved_ssid[MWX_SSID_MAX + 1];
    /* v0.21.1: etapa enquanto liga, motivo da falha (aparecem na tela) */
    char     stage[48];
    unsigned stage_ms;
    char     why[320];   /* v0.21.43: + portas */
    int      transport;        /* 0 nenhum, 1 OHCI full speed, 2 EHCI high speed */
    unsigned rx_rearms;        /* v0.21.8: recuperações do detector de RX parado */
    unsigned rx_reassoc;       /* v0.21.8: reassociações por RX parado */
    unsigned fw_resets;        /* v0.21.9: reinícios do firmware */
    unsigned rx_recovered;     /* v0.21.9: recuperações que trouxeram o RX de volta */
    char     err_detail[140];  /* v0.21.4: onde o handshake/associação parou */
    int      scanning;
    unsigned scan_seq;
    int      nnets;
    xw_net_t nets[XW_MAX_BSS];
} xw_status_t;

/* v0.21.10: o main avisa que há sessão/jogo (abrindo ou jogando). Com ela:
 * estatísticas do Wi-Fi a cada 5 s, todas as linhas do app no anel do
 * WIFILOG e o anel vai pro XC360/NETLOG.TXT direto no disco pela hw1
 * (xw_logfile_raw_step, no net_hook). */
void xw_sys_set_session(int on);
extern int xw_diag_lite;     /* v0.21.25: DIAG.TXT diag=lite (estatísticas a cada 30 s no jogo) */
extern int xw_diag_netlog;   /* v0.21.16: DIAG.TXT netlog=0 */
int  xw_sys_session(void);
/* só a hw1, fora de um xc_usb_hold: grava um setor do anel se houver e já
 * deu a hora. 1 = gravou. */
int  xw_logfile_raw_step(void);
void xw_sys_init(int cable);
void xw_sys_cable_up(void);   /* v0.21.29: cabo pegou DHCP depois do boot e virou a rota */
void xw_sys_step(void);
/* v0.21.7: o mesmo passo durante um xc_usb_hold (sem tocar no tinyehci) */
void xw_sys_step_held(void);
void xw_usb_poll_no_storage(void);

void xw_sys_status(xw_status_t *o);
int  xw_sys_scan(void);
/* pmk NULL = aberta. A rede é salva (WIFI.TXT, pelo main) quando conectar. */
int  xw_sys_connect(const char *ssid, int ssid_len, const uint8_t *pmk);
/* conecta na rede salva (A num item "Salva") */
int  xw_sys_connect_saved(void);
/* desconecta e apaga WIFI.TXT (main: usa o pendrive) */
int  xw_sys_forget(void);
/* main, a cada volta: grava WIFI.TXT se uma conexão nova deu certo */
void xw_sys_main_poll(void);
/* main: 1 enquanto vale esperar o Wi-Fi no boot (rede salva, sem cabo) */
int  xw_sys_boot_wait(void);

/* v0.21.5: XC360/WIFILOG.TXT. _line: qualquer thread (o np_log_line chama pras
 * linhas [wifi]); _flush: só quem pode usar o pendrive agora (main com a hw 3
 * livre, ou o worker das capas). 0 = ok. */
void xw_logfile_boot(const char *version);
void xw_logfile_line(const char *text);
int  xw_logfile_pending(void);
int  xw_logfile_flush(void);

#ifdef __cplusplus
}
#endif
#endif
