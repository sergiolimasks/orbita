/* XCloud360 stage 4 — <ifaddrs.h> (Xenon shim). getifaddrs() always fails:
 * nothing in the AF_CONN-only usrsctp / xcw-ported libpeer enumerates
 * interfaces (the host address comes from xcw_local_ipv4). */
#ifndef XCW_SHIM_IFADDRS_H
#define XCW_SHIM_IFADDRS_H
#include <sys/socket.h>
struct ifaddrs {
    struct ifaddrs  *ifa_next;
    char            *ifa_name;
    unsigned int     ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    struct sockaddr *ifa_dstaddr;
    void            *ifa_data;
};
#ifdef __cplusplus
extern "C" {
#endif
int  getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);
#ifdef __cplusplus
}
#endif
#endif
