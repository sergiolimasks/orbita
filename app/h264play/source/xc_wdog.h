/*
 * orbita v0.21.7 — vigia de travamento por thread de hardware.
 *
 * Cada thread que importa marca um "batimento" com um rótulo curto do que
 * está fazendo (xwd_beat). Outra thread confere (xwd_check): a de rede (hw 1)
 * vigia o main (hw 0) e a hw 3 (login/capas/sessão/atualização/chave); o
 * main vigia a hw 1. Parou por mais de XWD_STALL_MS: uma linha "[wdog]" com a
 * thread, há quanto tempo, o último rótulo e quem segura o USB do pendrive.
 * A linha vai pro log normal, pro XC360/WIFILOG.TXT e, por escrita direta de
 * setor (sem FatFs e sem xc_usb_hold), pro XC360/HANG.TXT pré-alocado.
 * Voltou a andar: "[wdog] ... voltou". Nada aqui bloqueia.
 */
#ifndef XC360_XC_WDOG_H
#define XC360_XC_WDOG_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { XWD_MAIN = 0, XWD_NET, XWD_HW3, XWD_N };
#define XWD_STALL_MS 5000u

/* thread atual (pelo PIR): hw0 = main, hw1 = rede, hw3 = tarefas. Outras: ignora. */
void xwd_beat(const char *label);
/* rótulo com um complemento (copiado), ex. ("pendrive: save", "TOKEN.TXT") */
void xwd_beat2(const char *label, const char *arg);
void xwd_beat_slot(int slot, const char *label);
/* v0.21.9: sub-passo (sem mexer no tempo do batimento): o aviso de parada diz
 * "em <rótulo> / passo <sub-passo>". Só ponteiro pra string estática. */
void xwd_crumb(const char *step);
/* a hw 3 só é vigiada enquanto roda uma tarefa (o main/xenon diz) */
void xwd_set_hw3_active(int on);
/* quem segura o USB do pendrive agora (slot) ou -1 */
void xwd_set_usb_holder(int slot);
/* confere os slots de mask (1<<XWD_*); self = quem confere. Reporta (log + HANG.TXT).
 * hw1 confere main e hw3; o main confere a hw1 (conjuntos separados). */
void xwd_check(int self, unsigned mask);
int  xwd_usb_holder(void);
/* v0.21.10: ms desde o último batimento do slot (0 = nunca bateu) */
uint64_t xwd_age_ms(int slot);
/* v0.21.12: tripwire de travamento DURO (main E/OU rede paradas). Chamado de
 * uma hw thread que não é nenhuma das duas: a tarefa do vigia na hw 5 (menu
 * e começo da sessão) e as esperas ociosas das threads do decoder no jogo
 * (xenon-pthread). No máximo 1x por 250 ms, sem lock: um TAS próprio. Parada
 * >= 8 s: xwd_trip_report (plataforma: setor do HANG.TXT se o USB não está
 * com a thread parada + texto na tela + UART; nunca o xc_log, que tem locks). */
void xwd_tripwire(void);
void xwd_trip_report(const char *line, int slot);
/* rótulo e sub-passo atuais do slot (strings estáticas; NULL se nunca bateu) */
const char *xwd_label(int slot);
const char *xwd_crumb_of(int slot);
/* v0.21.8: terceiro observador. Chamado de laços da hw 3 (worker das capas,
 * esperas do net_pipe); só faz algo na hw 3 com tarefa ativa, no máximo a
 * cada 500 ms. Main E rede paradas ao mesmo tempo (ninguém mais vê): uma
 * linha "[wdog] main (hw0) e rede (hw1) paradas" com escrita direta no
 * HANG.TXT primeiro. Não vale no jogo (aí a hw 3 é do decoder). */
void xwd_observe(void);
/* o mesmo, sem o filtro de PIR/tempo (testes e quem já sabe que é a hw 3) */
void xwd_check_both(int self);
/* PIR -> slot (ou -1) */
int  xwd_slot_of_pir(void);

/* plataforma (Xbox: main.c/xc_port_xenon.c; nos testes, falsos) */
uint64_t xwd_now_ms(void);
void     xwd_report(const char *line, int raw_ok);

#ifdef __cplusplus
}
#endif
#endif
