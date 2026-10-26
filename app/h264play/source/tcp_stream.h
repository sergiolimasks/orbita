/*
 * XCloud360 — rede do Xbox + ring de vídeo (AUs) pro decoder.
 *
 * v0.14: lwIP roda numa thread de rede dedicada (stream_start_thread); o
 * main só lê o ring. Wire do ring: [uint32_be size][annex-b AU] repetido.
 * v0.20.0: o modo PC saiu. Não há mais cliente TCP :9000 (vídeo vindo do
 * capture-rtp do PC): o ring é alimentado só pelo WebRTC do próprio Xbox
 * (stream_inject_au, na thread de rede). O nome do arquivo ficou.
 */
#ifndef XCLOUD360_TCP_STREAM_H
#define XCLOUD360_TCP_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* network_init (Aurora-safe: pula se o netif já tem IP) + ring. 0 = ok. */
int    stream_init(void);
/* 1 se stream_init passou do network_init (ring alocado). */
int    stream_net_up(void);
/* Sobe a thread de rede na thread de hardware `hw_thread` (0 = ok). Depois
 * disso o lwIP é só dela. */
int    stream_start_thread(int hw_thread);
/* v0.14.4: função que a thread de rede roda entre polls (retorna 1 se fez
 * trabalho). Precisa ser setada antes de stream_start_thread. */
void   stream_set_idle_hook(int (*fn)(void));

/* ---- consumidor (main) ---- */
size_t stream_read(uint8_t *dst, size_t n);
size_t stream_available(void);
/* Descarta tudo que está no ring agora (o produtor publica AUs inteiros,
 * então o ring continua alinhado num cabeçalho). */
void   stream_discard(void);

/* stage 5: carga da thread de rede desde a última chamada (us ocupados /
 * us de parede). Só um leitor (o STAT do main). */
void   stream_net_load(uint64_t *busy_us, uint64_t *wall_us);

/* ---- stage 5: vídeo do WebRTC no próprio Xbox ---- */
#define STREAM_INJECT_MAX (512 * 1024)   /* = AU_BUF_CAP do main.c */
/* Thread de rede: um AU inteiro ([u32 BE n][AU], publicado de uma vez) ou
 * nada. 0 = ok, -1 = sem espaço / tamanho inválido. */
int      stream_inject_au(const uint8_t *au, size_t n);
uint32_t stream_inject_full(void);
/* Função extra que a thread de rede roda no fim de cada net_step (o WebRTC). */
void     stream_set_net_extra(void (*fn)(void));
/* Um net_step completo (sem a função extra se já estiver dentro dela):
 * bomba do xcw_idle() (xcw_net_set_pump). Só a thread de rede. */
void     stream_net_pump(void);

#ifdef __cplusplus
}
#endif

#endif
