/* XCloud360 v0.21.45 — vigia do buraco de memória (orbita.lds).
 *
 * Em alguns consoles (suspeita: orbita aberto com o sistema da Microsoft já
 * rodando, Bad Avatar/Aurora) algo grava pacotes de rede na faixa física
 * 0x1d7000-0x1df000, que era código do decoder: tela vermelha 0x700 ao abrir
 * um jogo. Agora os primeiros 4 MB (até 0x80400000) ficam vazios; aqui a gente
 * só confere se alguém grava lá e quando, pro log.
 *   xhw_early(): 1a coisa do main; anota o que já tinha no buraco e zera.
 *   xhw_step():  16 KB por chamada (menu e jogo); gravações novas são zeradas.
 *   xhw_line():  uma linha de log por gravação nova (até XHW_NEV, depois resumo). */
#ifndef XC_HOLEWATCH_H
#define XC_HOLEWATCH_H
#include <stddef.h>
#include <stdint.h>

#define XHW_NEV 12

typedef struct {
    uint32_t words, first, last, sample[4];
    uint64_t blocks;   /* bit i = bloco de 64 KB i do buraco teve gravação */
} xhw_scan_t;

typedef struct {
    uint32_t addr, word, ms;
} xhw_ev_t;

void xhw_early(void);
const xhw_scan_t *xhw_early_result(void);
uint32_t xhw_start(void);
uint32_t xhw_end(void);
void xhw_step(void);
int xhw_line(char *o, size_t n);
unsigned xhw_hits(void);

#endif
