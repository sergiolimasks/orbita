/* XCloud360 stage 4 — <netinet/ip.h> (Xenon shim, BIG-endian bitfields).
 * usrsctp only uses the struct in code that is compiled out without INET,
 * but its headers reference the type. */
#ifndef XCW_SHIM_NETINET_IP_H
#define XCW_SHIM_NETINET_IP_H
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>

struct ip {
    uint8_t  ip_v : 4,   /* version (big-endian: high nibble first) */
             ip_hl : 4;  /* header length */
    uint8_t  ip_tos;
    uint16_t ip_len;
    uint16_t ip_id;
    uint16_t ip_off;
#define IP_RF 0x8000
#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_OFFMASK 0x1fff
    uint8_t  ip_ttl;
    uint8_t  ip_p;
    uint16_t ip_sum;
    struct in_addr ip_src, ip_dst;
};

#define IPVERSION    4
#define IP_MAXPACKET 65535
#define MAXTTL       255
#define IPDEFTTL     64
#define IPTOS_LOWDELAY 0x10
#endif
