/*
 * XCloud360 v0.17 — thread do login no Xbox (make XBOX_LOGIN=1).
 *
 * O fluxo (login_flow.c: TLS, OAuth, XSTS, catálogo) roda na hardware
 * thread 3 com pilha própria de 256KB (o handshake TLS + cJSON precisam de
 * pilha folgada; a thread de rede tem só 64KB). No menu as threads 2-5 estão
 * livres (o decoder só cria as dele no PLAY); o main não deixa dar PLAY nem
 * abrir o teste offline enquanto esta thread roda, pra ela não roubar um
 * slot de hw thread do decoder.
 *
 * Comunicação com o main: estado com lock() (lt_snapshot) e a lista de jogos
 * entregue uma vez (lt_take_games). Rede só pelo net_pipe; USB só com
 * xc_usb_hold (main.c).
 */
#ifndef XC360_LOGIN_TASK_H
#define XC360_LOGIN_TASK_H
#include "xbox_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LT_HW_THREAD 3

enum { LT_IDLE = 0, LT_RUNNING, LT_DONE, LT_FAILED };

typedef struct {
    int      phase;
    unsigned seq;           /* muda a cada atualização */
    unsigned runs;          /* quantas vezes terminou (DONE/FAILED) */
    char     status[120];
    char     uri[128];      /* não vazio = mostrar a tela de login */
    char     code[32];
    char     err[160];
    int      ngames;
} lt_snap_t;

/* Main: dispara (no-op se já roda). 0 = ok. */
int  lt_start(void);
int  lt_running(void);
void lt_cancel(void);
void lt_clear_cancel(void);   /* stage 5: before the session thread starts */
void lt_snapshot(lt_snap_t *s);
/* Main: pega a lista do último DONE (malloc; o chamador dá free). */
int  lt_take_games(xa_title_t **games, int *n);

/* main.c: segura o USB pra thread do login (a de rede para de chamar
 * usb_do_poll). on=1 bloqueia até a rede confirmar. */
int  xc_usb_hold(int on);   /* hold: 0 = USB é nosso, -1 = rede não soltou */
/* xc_port_xenon.c */
void xc_time_log_rtc(void);

#ifdef __cplusplus
}
#endif
#endif
