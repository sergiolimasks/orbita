/* Host-test mock (ctrl_test.c): just enough of lwIP's raw TCP API for
 * source/ctrl_conn.c. The test drives the callbacks by hand. */
#ifndef MOCK_LWIP_TCP_H
#define MOCK_LWIP_TCP_H
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

struct tcp_pcb;
typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
typedef void  (*tcp_err_fn)(void *arg, err_t err);
typedef err_t (*tcp_connected_fn)(void *arg, struct tcp_pcb *pcb, err_t err);

struct tcp_pcb {
    u16_t snd_buf;
    int   flags;
    void *arg;
    tcp_recv_fn recv;
    tcp_err_fn  errf;
    tcp_connected_fn connected;
    int   freed;
};

#define TF_NODELAY           0x40
#define TCP_WRITE_FLAG_COPY  0x01
#define tcp_sndbuf(pcb)          ((pcb)->snd_buf)
#define tcp_nagle_disable(pcb)   ((pcb)->flags |= TF_NODELAY)

struct tcp_pcb *tcp_new(void);
void  tcp_arg(struct tcp_pcb *pcb, void *arg);
void  tcp_err(struct tcp_pcb *pcb, tcp_err_fn f);
void  tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn f);
err_t tcp_connect(struct tcp_pcb *pcb, ip_addr_t *ip, u16_t port, tcp_connected_fn f);
err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t len, u8_t flags);
err_t tcp_output(struct tcp_pcb *pcb);
void  tcp_recved(struct tcp_pcb *pcb, u16_t len);
err_t tcp_close(struct tcp_pcb *pcb);
void  tcp_abort(struct tcp_pcb *pcb);
#endif
