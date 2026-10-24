/* Force-included compat header for building BSD/POSIX networking (usrsctp,
 * libpeer) on Xenon (devkitxenon/newlib + libxenon).
 * Injected with: -include xenon_missing.h
 *
 * Modeled on green-nx/deps/shim/include/switch_missing.h -- same problem,
 * different target. */
#ifndef _XENON_SHIM_MISSING_H_
#define _XENON_SHIM_MISSING_H_

#include <sys/types.h>
#include <stdint.h>

/* newlib on xenon: no <sys/socket.h> or <arpa/inet.h> in general (LWIP_SOCKET=0
 * means libxenon does NOT expose the sockets layer). We build usrsctp WITHOUT
 * touching real sockets -- our sctp <-> DTLS bridge is in-memory. So provide
 * the minimum types usrsctp needs to compile, but do NOT pretend socket() etc.
 * exist -- the patch below stubs the reference APIs. */

#ifndef AF_INET
#define AF_INET  2
#endif
#ifndef AF_INET6
#define AF_INET6 10
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif
#ifndef INADDR_ANY
#define INADDR_ANY ((uint32_t)0)
#endif

/* usrsctp expects sa_len byte on BSD-style newlib -- keep struct sockaddr
 * layout aligned. libxenon's newlib is BSD-derived. Matches usrsctp patch. */
#ifndef HAVE_SA_LEN
#define HAVE_SA_LEN 1
#endif

/* htons / htonl / ntohs / ntohl: lwip provides them but only when we include
 * lwip/inet.h. usrsctp includes <netinet/in.h> which does NOT exist on xenon,
 * so declare them here. Xenon is BIG-ENDIAN so htons/htonl are identity. */
#ifndef htons
static inline uint16_t htons(uint16_t x) { return x; }
static inline uint16_t ntohs(uint16_t x) { return x; }
static inline uint32_t htonl(uint32_t x) { return x; }
static inline uint32_t ntohl(uint32_t x) { return x; }
#endif

/* CMSG_ALIGN: newlib lacks it. Only referenced by usrsctp for msg control
 * layout math (dead code on our port since we do NOT use recvmsg). */
#ifndef CMSG_ALIGN
#define CMSG_ALIGN(n) (((n) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#endif

/* newlib: no ERESTART. usrsctp uses it as pseudo-errno for interrupted
 * waits (dead code on our port too -- no blocking recv). Any distinct
 * value not colliding with real errnos works. */
#include <errno.h>
#ifndef ERESTART
#define ERESTART 700
#endif

#ifndef UIO_MAXIOV
#define UIO_MAXIOV 1024
#endif
#ifndef IOV_MAX
#define IOV_MAX UIO_MAXIOV
#endif

/* struct iovec: usrsctp scatter/gather. newlib xenon might have it in
 * <sys/uio.h> (BSD-style) -- guard. */
#ifndef _SYS_UIO_H_
struct iovec {
    void*  iov_base;
    size_t iov_len;
};
#endif

/* struct sockaddr: skeleton. usrsctp needs the layout to match sa_len byte
 * pattern; real socket() calls are stubbed at link time. */
#ifndef _SYS_SOCKET_H
struct sockaddr {
    uint8_t  sa_len;
    uint8_t  sa_family;
    char     sa_data[14];
};
typedef uint32_t socklen_t;
#endif

/* struct sockaddr_in / sockaddr_in6: match Linux/BSD ABI. libpeer address.c
 * copies these around by memcpy. sa_len byte required for HAVE_SA_LEN. */
#ifndef _NETINET_IN_H
struct in_addr { uint32_t s_addr; };
struct sockaddr_in {
    uint8_t         sin_len;
    uint8_t         sin_family;
    uint16_t        sin_port;
    struct in_addr  sin_addr;
    char            sin_zero[8];
};
struct in6_addr { uint8_t s6_addr[16]; };
struct sockaddr_in6 {
    uint8_t         sin6_len;
    uint8_t         sin6_family;
    uint16_t        sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};
extern const struct in6_addr in6addr_any;
#endif

/* ip_mreq / ipv6_mreq: libpeer socket.c multicast. Compile stubs only. */
#ifndef _NETINET_IN_H
struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};
#endif

#endif /* _XENON_SHIM_MISSING_H_ */
