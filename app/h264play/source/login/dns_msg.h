/*
 * XCloud360 v0.17 — mensagens DNS (RFC 1035) mínimas: consulta A e leitura
 * da resposta. C puro (sem lwIP) pra rodar igual no Xbox e no teste do host.
 */
#ifndef XC360_DNS_MSG_H
#define XC360_DNS_MSG_H
#include <stddef.h>
#include <stdint.h>

/* Monta a consulta A recursiva de `host` em `out`. Retorna o tamanho ou -1. */
int dns_build_query(uint8_t *out, size_t cap, uint16_t id, const char *host);
/* Lê a resposta: 0 = achou um registro A (em *ip, "a.b.c.d" ->
 * (a<<24)|(b<<16)|(c<<8)|d), 1 = resposta válida sem A (NXDOMAIN etc., *rcode),
 * -1 = pacote inválido / id errado. */
int dns_parse_response(const uint8_t *p, size_t n, uint16_t id, uint32_t *ip, int *rcode);

#endif
