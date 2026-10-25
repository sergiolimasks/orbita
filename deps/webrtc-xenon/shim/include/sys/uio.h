/* XCloud360 stage 4 — <sys/uio.h> (newlib on Xenon has none). usrsctp only
 * needs struct iovec; it never calls readv/writev. */
#ifndef XCW_SHIM_SYS_UIO_H
#define XCW_SHIM_SYS_UIO_H
#include <stddef.h>

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

#ifndef UIO_MAXIOV
#define UIO_MAXIOV 1024
#endif
#ifndef IOV_MAX
#define IOV_MAX UIO_MAXIOV
#endif

#endif
