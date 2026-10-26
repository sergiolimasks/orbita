/*
 * XCloud360 v0.17 — "canos" TCP genéricos + resolvedor DNS + log UDP, todos
 * servidos pela THREAD DE REDE (np_step é chamado do net_step do
 * tcp_stream.c). Nenhuma outra thread toca o lwIP.
 *
 * Do outro lado (a thread do login, hw 3) a API é a de xc_port.h:
 * xcn_resolve / xcn_connect / xcn_send / xcn_recv / xcn_close, que só mexem
 * em mailboxes e buffers com lock() + lwsync:
 *   - conexão: o consumidor pede (slot FREE -> REQ); a rede faz tcp_new +
 *     tcp_connect (nunca com outro SYN pendente de outro cano,
 *     regra do bug da v0.8.2) e responde OPEN/ERR;
 *   - RX: 256KB por cano (rede escreve, consumidor lê); cheio = ERR_MEM no
 *     on_recv e o lwIP reentrega depois;
 *   - TX: 32KB (consumidor escreve, rede manda com tcp_write a cada passo);
 *   - fechar: o consumidor marca close_req, a rede faz tcp_abort e libera.
 * DNS: servidores do DHCP (opção 6, lida de dhcp_rx_options_val — o lwIP do
 * libxenon tem LWIP_DNS=0 mas decodifica a opção), depois o gateway,
 * 1.1.1.1 e 8.8.8.8. Log: np_log_line -> xlu_line (v0.20.1: upload dos
 * testadores beta, xc_logup.h; o UDP :9003 pro PC saiu, era o último
 * resquício do modo PC).
 */
#ifndef XC360_NET_PIPE_H
#define XC360_NET_PIPE_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Main, ANTES de stream_start_thread (cria o pcb UDP do DNS; o lwIP ainda
 * é do main nesse momento). 0 = ok. */
int  np_init(void);
/* Só a thread de rede. */
void np_step(void);
/* Só a thread de rede: -1 se nenhum cano tem SYN pendente, senão há quantos ms. */
int  np_connecting_ms(void);
/* Qualquer thread: uma linha de log (sem "\n") pro buffer do xc_logup. */
void np_log_line(const char *text);

#ifdef __cplusplus
}
#endif
#endif
