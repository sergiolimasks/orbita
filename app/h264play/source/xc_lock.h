/*
 * orbita v0.21.14 — locks instrumentados (spinlock com dono, lugar e espera).
 *
 * Os travamentos da v0.21.13 pararam o main e a hw1 no mesmo instante, sem
 * laço vivo e sem estouro de pilha: cara de deadlock. Os locks do app que o
 * main e a hw1 tomam no jogo passam por aqui:
 *   - quem segura (hw thread), desde quando e de onde (arquivo:linha);
 *   - quem espera, por qual lock e de onde (pra o tripwire dizer
 *     "a hw1 espera X, que a hw0 segura desde Y em Z");
 *   - ordem: cada par (segurando A, pega B) fica registrado; se alguém fizer
 *     B -> A depois, conta como ordem invertida (ABBA) e guarda os nomes.
 * try = no máximo `spins` tentativas, pra quem não pode esperar (log da hw1).
 *
 * ORDEM GLOBAL (de fora pra dentro; nunca pegar um de cima segurando um de
 * baixo): sessão (xs_lock) > net_pipe rx/tx > wifi status > [folhas: WIFILOG,
 * UART hw1, log beta]. As folhas não pegam nenhum outro lock.
 */
#ifndef XC360_XC_LOCK_H
#define XC360_XC_LOCK_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile unsigned int w;
    volatile int          owner;      /* hw thread (PIR) ou -1 */
    const char *volatile  site;       /* de onde pegou */
    volatile uint64_t     t_ms;       /* desde quando */
    const char           *name;
    volatile int          id;         /* 0 = ainda não registrado */
} xlk_t;

#define XLK_INIT(nm) { 0, -1, 0, 0, nm, 0 }
#define XLK_STR2(x) #x
#define XLK_STR(x) XLK_STR2(x)
#define XLK_HERE __FILE__ ":" XLK_STR(__LINE__)

void xlk_lock(xlk_t *l, const char *site);
/* 1 = pegou; 0 = desistiu depois de `spins` tentativas */
int  xlk_try(xlk_t *l, const char *site, int spins);
void xlk_unlock(xlk_t *l);
void xlk_set_name(xlk_t *l, const char *name);

/* relatório (só leitura; serve de qualquer thread, sem lock) */
void xlk_waiting_text(int pir, char *o, size_t n);   /* o que essa hw thread espera e quem segura */
void xlk_held_text(char *o, size_t n, unsigned min_ms);   /* locks seguros há >= min_ms */
unsigned xlk_inversions(void);
const char *xlk_inversion_text(void);
unsigned xlk_try_fails(void);

/* plataforma */
int      xlk_pir(void);
uint64_t xlk_now_ms(void);

#ifdef __cplusplus
}
#endif
#endif
