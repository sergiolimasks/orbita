/* XCloud360 v0.17 — ver dns_msg.h. */
#include "dns_msg.h"
#include <string.h>

int dns_build_query(uint8_t *out, size_t cap, uint16_t id, const char *host) {
    size_t hl = strlen(host);
    if (hl == 0 || hl > 253 || cap < 12 + hl + 2 + 4) return -1;
    uint8_t *p = out;
    p[0] = (uint8_t)(id >> 8); p[1] = (uint8_t)id;
    p[2] = 0x01; p[3] = 0x00;            /* RD */
    p[4] = 0; p[5] = 1;                  /* QDCOUNT 1 */
    memset(p + 6, 0, 6);
    p += 12;
    const char *s = host;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (l == 0 || l > 63) return -1;
        *p++ = (uint8_t)l;
        memcpy(p, s, l);
        p += l;
        s += l;
        if (*s == '.') s++;
    }
    *p++ = 0;
    *p++ = 0; *p++ = 1;                  /* QTYPE A */
    *p++ = 0; *p++ = 1;                  /* QCLASS IN */
    return (int)(p - out);
}

/* Pula um nome (com ponteiros de compressão). -1 = inválido. */
static int skip_name(const uint8_t *p, size_t n, size_t *off) {
    size_t o = *off;
    for (int guard = 0; guard < 128; guard++) {
        if (o >= n) return -1;
        uint8_t l = p[o];
        if (l == 0) { *off = o + 1; return 0; }
        if ((l & 0xC0) == 0xC0) {
            if (o + 1 >= n) return -1;
            *off = o + 2;
            return 0;
        }
        if (l & 0xC0) return -1;
        o += 1 + l;
    }
    return -1;
}

int dns_parse_response(const uint8_t *p, size_t n, uint16_t id, uint32_t *ip, int *rcode) {
    if (n < 12) return -1;
    if (((p[0] << 8) | p[1]) != id) return -1;
    if (!(p[2] & 0x80)) return -1;       /* não é resposta */
    int rc = p[3] & 0x0F;
    if (rcode) *rcode = rc;
    unsigned qd = (unsigned)(p[4] << 8 | p[5]);
    unsigned an = (unsigned)(p[6] << 8 | p[7]);
    size_t off = 12;
    for (unsigned i = 0; i < qd; i++) {
        if (skip_name(p, n, &off) < 0 || off + 4 > n) return -1;
        off += 4;
    }
    /* Um recursivo devolve a cadeia CNAME inteira + os A do nome final: o
     * primeiro A serve. */
    for (unsigned i = 0; i < an; i++) {
        if (skip_name(p, n, &off) < 0 || off + 10 > n) return -1;
        unsigned type = (unsigned)(p[off] << 8 | p[off + 1]);
        unsigned cls  = (unsigned)(p[off + 2] << 8 | p[off + 3]);
        unsigned rdl  = (unsigned)(p[off + 8] << 8 | p[off + 9]);
        off += 10;
        if (off + rdl > n) return -1;
        if (type == 1 && cls == 1 && rdl == 4) {
            *ip = ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
                  ((uint32_t)p[off + 2] << 8) | p[off + 3];
            return 0;
        }
        off += rdl;
    }
    return 1;
}
