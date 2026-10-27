/*
 * orbita v0.21.8 — detector de RX parado no Wi-Fi (C puro, testado no host).
 *
 * Link de dados de pé, nenhum quadro recebido há XRW_STALL_MS e, nesse tempo,
 * nós continuamos mandando (TX concluiu no USB): a outra ponta está
 * retransmitindo pra um receptor que não ouve. O glue então loga os tokens
 * dos QH/qTD e os índices dos anéis, reconstrói o anel do EP2 IN e conta a
 * recuperação. No máximo uma a cada XRW_GAP_MS.
 */
#ifndef XC360_XW_RXWATCH_H
#define XC360_XW_RXWATCH_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XRW_STALL_MS 3000u
#define XRW_GAP_MS   10000u

typedef struct {
    uint32_t last_rx, last_tx;      /* contadores vistos */
    uint32_t t_rx;                  /* quando o RX andou pela última vez */
    uint32_t tx_since_rx;           /* TX concluídos desde o último RX */
    uint32_t t_tx0;                 /* quando saiu o 1º desses TX */
    uint32_t t_last_fire;
    unsigned fires;
    int      armed;
} xrw_t;

void xrw_init(xrw_t *w, uint32_t now);
/* 1 = RX parado agora (o chamador loga e rearma). link = dados de pé. */
int  xrw_step(xrw_t *w, uint32_t now, int link, uint32_t rx_count, uint32_t tx_count);

#ifdef __cplusplus
}
#endif
#endif
