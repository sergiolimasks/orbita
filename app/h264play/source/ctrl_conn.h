/*
 * XCloud360 — conexão de controle com o launcher do PC (v0.16, make MENU=1).
 *
 * TCP PC_IP:9002, linhas de texto UTF-8 terminadas em "\n", campos por TAB.
 *   Xbox -> PC: HELLO\t<versão> (automático a cada conexão), LIST,
 *               PLAY\t<TITLEID>, STOP, PING (automático a cada 2s)
 *   PC -> Xbox: STATE\t<CODE>\t<texto>, LOGIN\t<url>\t<código>,
 *               GAME\t<TITLEID>\t<nome>\t<0|1> ... END, PONG
 *
 * THREADS: o lwIP só é tocado pela thread de rede (ctrl_step é chamado de
 * dentro do net_step do tcp_stream.c). O main só usa ctrl_send_line /
 * ctrl_recv_line / ctrl_connected / ctrl_conn_gen, que mexem em buffers
 * protegidos por lock() do libxenon — mesmo esquema da mailbox do input_udp.
 */
#ifndef XCLOUD360_CTRL_CONN_H
#define XCLOUD360_CTRL_CONN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guarda IP/porta/versão. Chamar antes de stream_start_thread; a primeira
 * conexão sai no próximo net_step. 0 = ok. */
int      ctrl_init(const char *server_ip, int server_port, const char *version);

/* Só a thread de rede (net_step): (re)conexão a cada 1s, flush da fila de
 * envio, PING a cada 2s, derruba a conexão se o PC ficar 7s mudo. */
void     ctrl_step(void);
/* Só a thread de rede: -1 se não há SYN do controle pendente, senão há
 * quantos ms (o tcp_stream.c evita dois SYN ao mesmo tempo). */
int      ctrl_connecting_ms(void);

/* Main: enfileira uma linha (sem o "\n"). -1 se desconectado ou fila cheia
 * — linhas não ficam esperando reconexão (a fila é zerada a cada conexão). */
int      ctrl_send_line(const char *line);
/* Main: copia a próxima linha completa recebida (sem "\r\n", truncada em
 * n-1). 1 = copiou, 0 = nada completo ainda. */
int      ctrl_recv_line(char *dst, size_t n);
int      ctrl_connected(void);
/* Incrementa a cada conexão nova (o main reenvia LIST quando muda). */
unsigned ctrl_conn_gen(void);

#ifdef __cplusplus
}
#endif

#endif
