/* orbita v0.21.1 — Wi-Fi interno em high speed no EHCI (nossos QHs na fila
 * assíncrona do controlador onde o adaptador está). Só a hw 1. Ver
 * xw_ehs_xenon.c e WIFI.md. */
#ifndef XC360_XW_EHS_XENON_H
#define XC360_XW_EHS_XENON_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int cin, cout, din, dout;              /* endereços (0 = não achou) */
    int cin_mps, cout_mps, din_mps, dout_mps;
    int cin_type;                          /* 2 bulk, 3 interrupt */
    int d3in, d3in_mps;                    /* v0.21.4: EP3 IN, se existir */
} xhs_eps_t;

typedef void (*xhs_rx_fn)(int ep, const uint8_t *b, size_t n, void *u);

/* 045e:0765 no EHCI (sem mexer em nada)? */
int  xhs_find(int *bus, int *port, int *addr);
/* QHs no anel, descritor, SET_CONFIGURATION, RX armados. 0 = ok; senão xhs_why(). */
int  xhs_open(xhs_eps_t *eps);
const char *xhs_why(void);
/* ep 1 = comando, 2 = dados. Só copia (vale dentro do xc_usb_hold). 0 = ok. */
int  xhs_send(int ep, const uint8_t *b, size_t n);
/* ativa envios, colhe RX (fn por pacote), religa o anel. 1 = fez algo. */
int  xhs_poll(xhs_rx_fn fn, void *u);
/* v0.21.7: allow_relink = 0 durante um xc_usb_hold com o pendrive no mesmo EHCI */
int  xhs_poll2(xhs_rx_fn fn, void *u, int allow_relink);
int  xhs_shared(void);
int  xhs_storage_begin(void); /* hw1: quiesce shared DMA before granting disk ownership */
void xhs_storage_end(void);   /* hw1: restore Wi-Fi after the disk owner releases it */
void xhs_stats(char *o, size_t n);
/* v0.21.4: transferências OUT concluídas (ep 1 ou 2) */
unsigned xhs_tx_done(int ep);
/* v0.21.8: diagnóstico/recuperação do RX. which: 1 EP1 IN, 2 EP1 OUT, 3 EP2 IN, 4 EP2 OUT, 5 EP3 IN */
unsigned xhs_rx_count(void);
int  xhs_npipes(void);
int  xhs_dump_pipe(int which, char *o, size_t n);
int  xhs_rx_rearm(int which);
/* v0.21.9 */
int  xhs_out_reset(int which);
/* v0.21.11: acerta um anel de entrada com o HC (sem mexer no overlay) se ele
 * estiver estacionado fora do lugar; >= 0 quadros entregues, -1 nada a fazer */
int  xhs_in_resync(int which, xhs_rx_fn fn, void *u);
unsigned xhs_resyncs(void);
unsigned xhs_resync_suspects(void);
/* v0.21.13: polls recusados por reentrada; polls que bateram no orçamento de 2 ms */
unsigned xhs_poll_reenters(void);
/* v0.21.17: trava do poll achada velha (e limpa); canários pisados; corpos do poll que terminaram */
unsigned xhs_guard_stale(void);
unsigned xhs_guard_trampled(void);
unsigned xhs_poll_runs(void);
#ifdef XHS_HOSTTEST
void xhs_test_stick_guard(void);
void xhs_test_trample_guard(void);
#endif
unsigned xhs_poll_budget_hits(void);
/* v0.21.15: última leitura da memória do HC (pro vigia) e leituras do overlay */
void xhs_last_access(char *o, size_t n);
/* v0.21.16: chaves do XC360/DIAG.TXT (lidas antes do open) */
extern int xhs_dma128, xhs_diag_overlay;
/* v0.21.19: pool estático dos descritores, buffers cacheados, EP3 IN (DIAG dmapool/bufcache/ep3) */
extern int xhs_dma_pool, xhs_buf_cached, xhs_diag_ep3;
int  xhs_dma_layout_ok(char *why, size_t n);
unsigned xhs_overlay_reads(void);
/* v0.21.20: portas do EHCI do Wi-Fi tratadas por nós (o tinyehci fica fora);
 * mudanças de PORTSC nos dois controladores vão pro log. A cada 10 ms. */
int  xhs_port_watch(void);
int  xhs_watch_claim(void);   /* v0.21.29: vigia sem o Wi-Fi aberto (cabo desde o boot) */
unsigned xhs_port_handoffs(void);
unsigned xhs_port_hs_blocked(void);
unsigned xhs_port_changes(void);
#ifdef XHS_HOSTTEST
void xhs_test_desync(int which);
#endif
void xhs_peek(char *o, size_t n);
int  xhs_check(char *why, size_t n);
void xhs_dump(char *o, size_t n);

#endif
