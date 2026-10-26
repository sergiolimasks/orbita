/*
 * XCloud360 v0.17 — o que o cliente HTTPS / login precisa da plataforma.
 * Duas implementações:
 *   Xbox: net_pipe.c (TCP/DNS pela thread de rede) + xc_port_xenon.c
 *   host: hosttest/login/port_posix.c (sockets POSIX), pros testes.
 * Tudo aqui é chamado pela thread do login/sessão (nunca pela thread de rede),
 * menos as funções não-bloqueantes marcadas abaixo.
 */
#ifndef XC360_PORT_H
#define XC360_PORT_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Códigos de erro do transporte (negativos). */
#define XCN_ETIMEOUT  (-1)
#define XCN_ECLOSED   (-2)
#define XCN_EDNS      (-3)
#define XCN_ECONN     (-4)
#define XCN_EIO       (-5)
#define XCN_ECANCEL   (-6)
#define XCN_ENOSLOT   (-7)
#define XCN_EAGAIN    (-8)   /* v0.19.2 não-bloqueante: ainda nada */

/* host -> IPv4 "a.b.c.d" como (a<<24)|..|d. 0 = ok. */
int      xcn_resolve(const char *host, uint32_t *ip, int timeout_ms);
/* Abre TCP. *h = handle. 0 = ok. */
int      xcn_connect(uint32_t ip, int port, int timeout_ms, int *h);
/* Manda tudo (bloqueia até `timeout_ms`). Retorna n ou erro < 0. */
int      xcn_send(int h, const uint8_t *b, size_t n, int timeout_ms);
/* >0 bytes lidos, 0 = o outro lado fechou, <0 erro (XCN_ETIMEOUT...). */
int      xcn_recv(int h, uint8_t *b, size_t n, int timeout_ms);
void     xcn_close(int h);
uint64_t xcn_now_ms(void);
uint64_t xcn_now_us(void);
void     xcn_sleep_ms(int ms);
/* v0.19.2 — versões NÃO-bloqueantes, pra a thread de rede (hw 1) dirigir a
 * conexão keep-alive do GSSV durante o jogo sem nunca esperar (no Xbox é a
 * própria hw 1 que serve os canos: uma espera ali travaria tudo).
 *   connect_start: reserva um cano e pede o SYN (0 = ok); connect_poll:
 *   0 = aberto, XCN_EAGAIN = ainda não, < 0 = falhou (cano já liberado).
 *   try_send: copia o que couber (>= 0 bytes, 0 = cheio). try_recv: > 0
 *   bytes, 0 = o outro lado fechou, XCN_EAGAIN = nada ainda, < 0 erro.
 *   close_nb: pede o fechamento e volta na hora. */
int      xcn_connect_start(uint32_t ip, int port, int *h);
int      xcn_connect_poll(int h);
int      xcn_try_send(int h, const uint8_t *b, size_t n);
int      xcn_try_recv(int h, uint8_t *b, size_t n);
void     xcn_close_nb(int h);
/* 1 se o usuário cancelou (o login aborta no próximo ponto de espera). */
int      xcn_cancelled(void);
/* Gancho extra pro xcn_cancelled (NULL = nenhum): xc_catalog o usa pra
 * interromper o pedido em voo quando o main manda parar a thread das capas.
 * Xbox: login_task.c; host: port_posix.c. */
void     xcn_set_cancel_hook(int (*fn)(void));

/* Log de diagnóstico: UART + buffer do upload beta (xc_logup.h; v0.20.1,
 * antes UDP :9003 pro PC). NUNCA passar tokens (o filtro é só a 2a linha). */
void     xc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Hora Unix atual, ou -1 se ainda não sabemos. */
long long xc_time_now(void);
/* Acerta a hora (fonte: header Date de uma resposta autenticada). */
void     xc_time_set(long long unix_now, const char *source);
/* Piso: hora da build (nenhuma hora válida é anterior). */
long long xc_time_floor(void);

/* Arquivo pequeno no pendrive (nome 8.3, ex. "TOKEN.TXT"). Retorna o
 * tamanho lido (>= 0) ou -1. save: 0 = ok. remove: 0 = ok. */
int      xc_store_load(const char *name, void *buf, size_t cap);
int      xc_store_save(const char *name, const void *buf, size_t n);
int      xc_store_remove(const char *name);

/* Material de personalização pro DRBG (fusíveis, MAC, relógio...). */
size_t   xc_seed_material(uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
