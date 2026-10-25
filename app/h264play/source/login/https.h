/*
 * XCloud360 v0.17 — cliente HTTPS mínimo (mbedTLS 3.4, TLS 1.2) sobre o
 * transporte de xc_port.h.
 *   - https_request: uma conexão por pedido (Connection: close) — login,
 *     catálogo, passport.
 *   - https_conn_* (v0.19.2): conexão PERSISTENTE (HTTP/1.1 keep-alive) pro
 *     host da região do GSSV: /state, /sdp, /ice, /keepalive e DELETE
 *     reaproveitam o mesmo TLS, então um pedido em regime custa só a cifra
 *     dos registros (AES-GCM de ~4 KB), não um handshake ECDHE (~75 ms de
 *     CPU no Xenon). Reconexão tenta retomar a sessão TLS (session ID); o
 *     GSSV de hoje NÃO retoma (sondado em 2026-10-25: "New" sempre), então
 *     na prática reconectar = handshake completo.
 *     Duas formas de usar a MESMA conexão (um dono por vez):
 *       bloqueante  https_conn_request  (thread da sessão, hw 3)
 *       não-bloq.   https_conn_start + https_conn_poll  (thread de rede,
 *                   hw 1: nunca espera; um handshake COMPLETO ali é proibido
 *                   — devolve HTTPS_ENEEDCPU e quem chamou passa o serviço
 *                   pra uma thread que pode gastar CPU).
 *
 * Certificados: cadeia verificada SEMPRE contra as raízes embutidas
 * (ca_roots.c) + nome do host (SAN/CN). Hora: enquanto não sabemos a hora
 * (o RTC do Xbox não é confiável), o 1º pedido aceita só as flags de
 * validade (EXPIRED/FUTURE) no handshake; a resposta traz o header Date —
 * vindo do servidor já autenticado pela assinatura — que acerta o relógio,
 * e aí a validade de toda a cadeia do servidor é conferida contra essa hora.
 * Se falhar, o pedido falha. Daí em diante a verificação é a normal.
 */
#ifndef XC360_HTTPS_H
#define XC360_HTTPS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  code;          /* HTTPS_E* ou status HTTP ruim */
    int  tls;           /* código mbedTLS (negativo) quando houver */
    char msg[160];      /* curto, pt-BR, pra tela: "TLS -0x2700 em xsts" */
} xc_err_t;

enum {
    HTTPS_OK = 0, HTTPS_EINIT = -100, HTTPS_EDNS, HTTPS_ETCP, HTTPS_ETLS,
    HTTPS_ECERT, HTTPS_ETIMEOUT, HTTPS_EPROTO, HTTPS_ETOOBIG, HTTPS_ECANCEL,
    HTTPS_ETIME, HTTPS_EHTTP,
    HTTPS_ENEEDCPU,     /* v0.19.2, só no modo não-bloqueante: precisa de DNS
                           ou de um handshake TLS completo -> thread de CPU */
    HTTPS_EBUSY         /* já há um pedido em voo nesta conexão */
};
#define HTTPS_PENDING 1  /* https_conn_poll: ainda não terminou */

typedef struct {
    int    status;
    char  *body;        /* terminado em NUL; https_free */
    size_t len;
} https_resp_t;

/* Uma vez: entropia + DRBG + raízes. 0 = ok. */
int  https_init(xc_err_t *e);
/* `headers`: linhas extras "Nome: valor\r\n" (ou NULL). `step` nomeia o
 * passo nas mensagens de erro ("token", "xsts"...). Retorna 0 se houve
 * resposta HTTP (qualquer status, em r->status), < 0 se não. */
int  https_request(const char *step, const char *method, const char *host,
                   const char *path, const char *headers, const char *body,
                   size_t body_len, size_t max_body, https_resp_t *r, xc_err_t *e);
void https_free(https_resp_t *r);
/* Gera bytes aleatórios do DRBG (seed nova pro pendrive). 0 = ok. */
int  https_random(unsigned char *out, size_t n);
/* Teste do host: só DNS + TCP + handshake + verificação (sem HTTP). */
int  https_probe(const char *host, xc_err_t *e);

/* ------------------------------------------- v0.19.2: keep-alive */
typedef struct https_conn https_conn_t;

typedef struct {
    unsigned full;            /* handshakes completos (ECDHE + cadeia) */
    unsigned resumed;         /* handshakes abreviados (sessão retomada) */
    unsigned requests;        /* respostas recebidas */
    unsigned reused;          /* ... numa conexão que já tinha servido outro */
    unsigned retries;         /* conexão reaproveitada morta, pedido refeito */
    unsigned needcpu;         /* não-bloqueante desistiu (HTTPS_ENEEDCPU) */
    unsigned last_hs_cpu_us;  /* CPU do último handshake (sem esperas de rede) */
    unsigned max_hs_cpu_us;
    unsigned last_req_cpu_us; /* CPU do último pedido (cifra + HTTP, sem rede) */
    unsigned max_req_cpu_us;
    uint64_t req_cpu_us;      /* soma, pros pedidos */
    unsigned nb_requests;     /* pedidos feitos pelo caminho não-bloqueante */
    unsigned max_nb_poll_us;  /* maior chamada de https_conn_poll */
} https_conn_stats_t;

https_conn_t *https_conn_new(const char *host);
/* Fecha (close_notify; espera o cano ser liberado — NÃO use na hw 1). */
void https_conn_free(https_conn_t *c);
const char *https_conn_host(const https_conn_t *c);
/* 1 = TLS aberto e pronto pra outro pedido. */
int  https_conn_is_open(const https_conn_t *c);
/* 1 = há um pedido não-bloqueante em voo. */
int  https_conn_busy(const https_conn_t *c);
/* Bloqueante, mesma semântica de https_request, reaproveitando a conexão
 * (reabre — retomando a sessão TLS se o servidor deixar — quando ela caiu;
 * um pedido numa conexão reaproveitada que morre sem nenhum byte de
 * resposta é refeito uma vez numa conexão nova). */
int  https_conn_request(https_conn_t *c, const char *step, const char *method,
                        const char *path, const char *headers, const char *body,
                        size_t body_len, size_t max_body, https_resp_t *r, xc_err_t *e);
/* Não-bloqueante (thread de rede). start copia tudo; poll avança o que der
 * sem esperar e devolve HTTPS_PENDING, 0 (resposta em r) ou erro < 0.
 * HTTPS_ENEEDCPU = a conexão caiu e reabrir exige DNS ou handshake completo
 * (a conexão é derrubada; a próxima https_conn_request reabre). */
int  https_conn_start(https_conn_t *c, const char *step, const char *method,
                      const char *path, const char *headers, const char *body,
                      size_t body_len, size_t max_body, xc_err_t *e);
int  https_conn_poll(https_conn_t *c, https_resp_t *r, xc_err_t *e);
/* Não-bloqueante: abandona o pedido em voo e derruba o TCP (sem esperar). */
void https_conn_abort(https_conn_t *c);
void https_conn_get_stats(const https_conn_t *c, https_conn_stats_t *s);

#ifdef __cplusplus
}
#endif
#endif
