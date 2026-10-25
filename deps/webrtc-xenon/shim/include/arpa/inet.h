/* XCloud360 stage 4 — <arpa/inet.h> for bare-metal Xenon. The functions are
 * implemented in deps/webrtc-xenon/shim/xcw_inet.c under xcw_ names (lwIP
 * 1.4.1 has inet_* MACROS in lwip/inet.h; distinct symbol names keep the two
 * worlds from ever colliding). */
#ifndef XCW_SHIM_ARPA_INET_H
#define XCW_SHIM_ARPA_INET_H
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif
int         xcw_inet_pton(int af, const char *src, void *dst);
const char *xcw_inet_ntop(int af, const void *src, char *dst, socklen_t size);
in_addr_t   xcw_inet_addr(const char *cp);
int         xcw_inet_aton(const char *cp, struct in_addr *inp);
char       *xcw_inet_ntoa(struct in_addr in);
#ifdef __cplusplus
}
#endif

#define inet_pton xcw_inet_pton
#define inet_ntop xcw_inet_ntop
#define inet_addr xcw_inet_addr
#define inet_aton xcw_inet_aton
#define inet_ntoa xcw_inet_ntoa

#endif
