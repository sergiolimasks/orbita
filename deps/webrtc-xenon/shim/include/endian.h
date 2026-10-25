/* XCloud360 stage 4 — <endian.h> (Xenon shim). libpeer rtp.h/rtcp.h pick
 * their RTP/RTCP header bitfield order from __BYTE_ORDER. */
#ifndef XCW_SHIM_ENDIAN_H
#define XCW_SHIM_ENDIAN_H
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412
#define __BYTE_ORDER    __BIG_ENDIAN
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER
#endif
#endif
