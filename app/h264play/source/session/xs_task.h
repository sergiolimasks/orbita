/*
 * XCloud360 stage 3 — thread da sessão xCloud no Xbox (como o login_task.c).
 *
 * Roda xsm_step() numa hardware thread própria com pilha de 256KB (TLS +
 * cJSON). NUNCA ao mesmo tempo que o login_task (os dois usam a hw 3 por
 * padrão e o xcn_cancelled() do https.c é o do login): o main só chama
 * xst_start() com !lt_running().
 *
 * v0.19.2 (decoder com 4 threads, hw 4,2,5,3): com cfg.net_live a thread
 * NÃO fica viva o jogo todo. Ela vai do AUTH até o answer publicado e
 * entrega as fases ao vivo pra thread de rede (xsm_net_step); só volta
 * (xst_resume) pro que gasta CPU/espera — re-login, handshake completo,
 * re-sinalização, DELETE — e sai de novo. Quem decide quando a hw 3 está
 * livre pra isso é o xc_native.c (aluguel da hw 3 com o decoder).
 */
#ifndef XC360_XS_TASK_H
#define XC360_XS_TASK_H
#include <stddef.h>
#include "xs_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XST_HW_THREAD
#define XST_HW_THREAD 3
#endif

/* Main: dispara a sessão (no-op/-1 se já roda). `refresh` NULL = TOKEN.TXT. */
int  xst_start(const char *title_id, const char *name, int f2p, int active_policy, const char *refresh);
/* Igual, com config completa (tempos, max_attempts, waittime...). */
int  xst_start_cfg(const xsm_cfg_t *cfg, const char *refresh);
int  xst_running(void);
/* v0.19.2: sobe a thread de novo numa máquina com owner = WANT_THREAD
 * (0 = ok; -1 = hw ocupada / dono errado). */
int  xst_resume(void);
/* XSM_OWN_* da máquina atual (DONE se nunca houve). */
int  xst_owner(void);
/* Pede o fim (DELETE da sessão); a thread sai sozinha logo depois. */
void xst_stop(void);
/* Linha "STATE\tCODE\ttexto" pro mm_handle_line; devolve o seq. */
unsigned xst_state_line(char *out, size_t cap);
/* Pro WebRTC (thread de rede) chamar xsm_rtc_*. Válido enquanto
 * xst_running(); NÃO use depois de um xst_start() novo sem reler a gen. */
xsm_t *xst_machine(void);

#ifdef __cplusplus
}
#endif
#endif
