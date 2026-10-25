/* XCloud360 stage 4 — <netinet/udp.h> (Xenon shim). */
#ifndef XCW_SHIM_NETINET_UDP_H
#define XCW_SHIM_NETINET_UDP_H
#include <stdint.h>
struct udphdr {
    uint16_t uh_sport;
    uint16_t uh_dport;
    uint16_t uh_ulen;
    uint16_t uh_sum;
};
#endif
