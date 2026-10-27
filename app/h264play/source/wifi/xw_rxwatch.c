/* orbita v0.21.8 — detector de RX parado. Ver xw_rxwatch.h. */
#include "xw_rxwatch.h"

#include <string.h>

void xrw_init(xrw_t *w, uint32_t now) {
    memset(w, 0, sizeof(*w));
    w->t_rx = now;
}

int xrw_step(xrw_t *w, uint32_t now, int link, uint32_t rx, uint32_t tx) {
    if (!w->armed || !link) {
        /* sem link (ou primeira vez): só acompanha */
        w->armed = link;
        w->last_rx = rx;
        w->last_tx = tx;
        w->t_rx = now;
        w->tx_since_rx = 0;
        return 0;
    }
    if (rx != w->last_rx) {
        w->last_rx = rx;
        w->t_rx = now;
        w->tx_since_rx = 0;
    }
    if (tx != w->last_tx) {
        /* o prazo conta do 1º envio sem resposta, não do último RX: depois de
         * um tempo quieto, o primeiro SYN não pode disparar na hora */
        if (w->tx_since_rx == 0) w->t_tx0 = now;
        w->tx_since_rx += tx - w->last_tx;
        w->last_tx = tx;
    }
    if (w->tx_since_rx == 0 || now - w->t_tx0 < XRW_STALL_MS) return 0;
    if (w->fires && now - w->t_last_fire < XRW_GAP_MS) return 0;
    w->fires++;
    w->t_last_fire = now;
    w->t_rx = now;          /* dá tempo pro rearme fazer efeito */
    w->tx_since_rx = 0;
    return 1;
}
