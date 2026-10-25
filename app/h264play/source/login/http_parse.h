/*
 * XCloud360 v0.17 — parser incremental de resposta HTTP/1.1 (status,
 * cabeçalhos, Content-Length, chunked, ou até o fim da conexão). C puro,
 * testado no host (hosttest/login). Sem gzip (pedimos identity).
 */
#ifndef XC360_HTTP_PARSE_H
#define XC360_HTTP_PARSE_H
#include <stddef.h>
#include <stdint.h>

#define HP_HDR_MAX 16384

enum { HP_OK_MORE = 0, HP_DONE = 1, HP_ERR = -1, HP_ERR_TOOBIG = -2 };

typedef struct {
    int      st;
    char     hdr[HP_HDR_MAX + 1];
    size_t   hdr_len;
    int      status;
    long     content_length;     /* -1 = não veio */
    int      chunked;
    int      head_only;          /* resposta sem corpo (204/304) */
    size_t   chunk_left;
    char     line[80];
    size_t   line_len;
    uint8_t *body;               /* sempre terminado em NUL (malloc) */
    size_t   body_len, body_cap, body_max;
    char     date[64];           /* header Date (vazio se não veio) */
    char     content_type[64];
} http_parser_t;

void hp_init(http_parser_t *h, size_t body_max);
/* Alimenta bytes. HP_DONE = resposta completa (bytes depois disso ignorados). */
int  hp_feed(http_parser_t *h, const uint8_t *p, size_t n);
/* A conexão fechou: HP_DONE se a resposta é válida assim (corpo até o fim). */
int  hp_eof(http_parser_t *h);
void hp_free(http_parser_t *h);
/* Data HTTP ("Sun, 06 Nov 1994 08:49:37 GMT") -> segundos Unix; -1 inválida. */
long long hp_parse_http_date(const char *s);

#endif
