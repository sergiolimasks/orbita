/* XCloud360 v0.21.37 — guarda do código (.text).
 *
 * As telas vermelhas do jogo (0x700/DSI no decoder) caem em instruções que
 * não podem falhar ("mr", "lwz" com registrador válido): o código na RAM foi
 * alterado depois do boot (o mesmo fenômeno da página 0x3b000 da v0.21.29).
 * No boot guarda uma cópia do .text (~3,7 MB) e a soma de cada bloco de
 * 64 KB; quem chama xtg_step() confere um bloco por vez e, se ele mudou,
 * restaura da cópia (com o cache de instruções invalidado) e anota onde. */
#ifndef XC_TEXTGUARD_H
#define XC_TEXTGUARD_H
#include <stdint.h>

int  xtg_init(void);              /* 0 = ligado; no boot, antes das threads do jogo */
void xtg_step(void);              /* confere 1 bloco (no máximo 1 a cada XTG_STEP_MS); qualquer thread */
int  xtg_check_addr(uint32_t a);  /* 1 = o word em a difere da cópia (relatório de exceção) */

/* o que achou (o main loga quando xtg_bad muda) */
extern volatile unsigned xtg_bad, xtg_fixed, xtg_sweeps;
extern volatile uint32_t xtg_last_addr, xtg_last_now, xtg_last_want, xtg_last_words;
/* v0.21.39: diagnóstico. observe = só anota (aceita o valor novo, não restaura);
 * custo de cada passo em us; anel das últimas mudanças (endereço, antes, depois) */
extern volatile int xtg_observe;
extern volatile uint32_t xtg_us_max, xtg_steps;
extern volatile uint64_t xtg_us_sum;
#define XTG_NEV 16
typedef struct { uint32_t addr, before, after; } xtg_ev_t;
extern xtg_ev_t xtg_ev[XTG_NEV];
extern volatile unsigned xtg_nev;   /* total já anotado (o anel guarda os últimos XTG_NEV) */

#endif
