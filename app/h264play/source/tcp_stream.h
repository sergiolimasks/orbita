/*
 * XCloud360 — TCP live H.264 stream client (Aurora-safe).
 * Wire: [uint32_be size][annex-b AU bytes] repetido.
 * lwip RAW API do libxenon (LWIP_SOCKET=0, NO_SYS=1). v0.14: lwIP roda numa
 * thread de rede dedicada (stream_start_thread); o main só lê o ring.
 */
#ifndef XCLOUD360_TCP_STREAM_H
#define XCLOUD360_TCP_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int    stream_init(const char *server_ip, int server_port);
void   stream_poll(void);
size_t stream_read(uint8_t *dst, size_t n);
size_t stream_available(void);
size_t stream_dropped(void);
void   stream_reset(void);
/* v0.11.1: fecha o TCP atual + zera ring; próximo stream_poll reconecta.
 * Usado quando o framing corrompeu — força capture-rtp a mandar novo IDR. */
void   stream_force_reconnect(void);
/* v0.14: sobe a thread de rede na thread de hardware `hw_thread` (0 = ok).
 * Depois disso stream_poll() vira no-op e o lwIP é só dela. */
int    stream_start_thread(int hw_thread);
/* v0.14.4: função que a thread de rede roda entre polls (retorna 1 se fez
 * trabalho). Precisa ser setada antes de stream_start_thread. */
void   stream_set_idle_hook(int (*fn)(void));
/* v0.14: consumidor — 1 se a conexão mudou e o ring foi descartado; o
 * chamador deve esquecer qualquer AU parcialmente lido. */
int    stream_take_reset(void);

/* ---- v0.16 (menu) ---- */
/* Consumidor: descarta tudo que está no ring agora. */
void     stream_discard(void);
/* Quantas reconexões já foram feitas (stream_force_reconnect atendido). */
unsigned stream_reconnects_done(void);
/* Thread de rede: -1 se o vídeo não tem SYN pendente, senão há quantos ms. */
int      stream_connecting_ms(void);
/* 1 = não loga as tentativas de reconexão recusadas (menu esperando jogo). */
void     stream_set_quiet(int on);
/* 1 se stream_init passou do network_init (ring alocado). */
int      stream_net_up(void);
/* stage 5: carga da thread de rede desde a última chamada (us ocupados /
 * us de parede). Só um leitor (o STAT do main). */
void     stream_net_load(uint64_t *busy_us, uint64_t *wall_us);

#ifdef XCLOUD360_NATIVE
/* ---- stage 5 (make STANDALONE=1): vídeo do WebRTC no próprio Xbox ---- */
#define STREAM_INJECT_MAX (512 * 1024)   /* = AU_BUF_CAP do main.c */
/* Main: 1 = o ring passa a ser alimentado por stream_inject_au (o cliente
 * TCP :9000 é fechado e para de tentar); 0 = volta pro PC. A thread de rede
 * aplica no próximo passo e o consumidor vê um stream_take_reset(). */
void     stream_set_inject(int on);
int      stream_inject_active(void);
/* Thread de rede: um AU inteiro ([u32 BE n][AU]) ou nada. 0 = ok, -1 = sem
 * espaço / modo errado / consumidor ainda descartando. */
int      stream_inject_au(const uint8_t *au, size_t n);
uint32_t stream_inject_full(void);
/* Função extra que a thread de rede roda no fim de cada net_step (o WebRTC). */
void     stream_set_net_extra(void (*fn)(void));
/* Um net_step completo (sem a função extra se já estiver dentro dela):
 * bomba do xcw_idle() (xcw_net_set_pump). Só a thread de rede. */
void     stream_net_pump(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
