/* XCloud360 stage 4 — <netdb.h> (Xenon shim): types only. */
#ifndef XCW_SHIM_NETDB_H
#define XCW_SHIM_NETDB_H
#include <sys/socket.h>
struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};
#endif
