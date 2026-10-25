/* XCloud360 stage 4 — force-included (-include xcw_shim.h) into every WebRTC
 * library object on bare-metal Xenon. */
#ifndef XCW_SHIM_H
#define XCW_SHIM_H
#include <errno.h>
#include <sys/types.h>
#include <stdint.h>
/* newlib has no ERESTART; usrsctp uses it as an internal pseudo-errno. */
#ifndef ERESTART
#define ERESTART 700
#endif
#ifndef EPROCLIM
#define EPROCLIM 701
#endif
#ifndef EUSERS
#define EUSERS 702
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#define XCW_BAREMETAL 1

/* NOTE: xenon-gcc predefines __linux__/unix (newlib "linux-like" triple), so
 * usrsctp/libpeer compile their Linux branches; the shim headers satisfy them. */
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
int nanosleep(const struct timespec *req, struct timespec *rem); /* xcw_pthread_extra.c */
#ifdef __cplusplus
}
#endif
#endif
