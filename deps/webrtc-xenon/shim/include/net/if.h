/* XCloud360 stage 4 — <net/if.h> (Xenon shim). Flag values follow Linux. */
#ifndef XCW_SHIM_NET_IF_H
#define XCW_SHIM_NET_IF_H
#include <sys/types.h>
#include <sys/socket.h>

#define IF_NAMESIZE 16
#ifndef IFNAMSIZ
#define IFNAMSIZ IF_NAMESIZE
#endif
#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_DEBUG       0x4
#define IFF_LOOPBACK    0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING     0x40
#define IFF_NOARP       0x80
#define IFF_PROMISC     0x100
#define IFF_ALLMULTI    0x200
#define IFF_MULTICAST   0x1000

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifru_addr;
        short ifru_flags;
        int   ifru_mtu;
        void *ifru_data;
    } ifr_ifru;
};
#define ifr_addr  ifr_ifru.ifru_addr
#define ifr_flags ifr_ifru.ifru_flags
#define ifr_mtu   ifr_ifru.ifru_mtu
#define ifr_data  ifr_ifru.ifru_data

#ifdef __cplusplus
extern "C" {
#endif
unsigned int if_nametoindex(const char *ifname);
char *if_indextoname(unsigned int ifindex, char *ifname);
#ifdef __cplusplus
}
#endif
#endif
