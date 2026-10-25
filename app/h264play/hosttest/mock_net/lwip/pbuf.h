#ifndef MOCK_LWIP_PBUF_H
#define MOCK_LWIP_PBUF_H
#include "lwip/err.h"
struct pbuf {
    struct pbuf *next;
    void        *payload;
    u16_t        tot_len;
    u16_t        len;
};
u8_t  pbuf_free(struct pbuf *p);
u16_t pbuf_copy_partial(struct pbuf *p, void *dataptr, u16_t len, u16_t offset);
#endif
