/* orbita v0.21.8 — lwIP 1.4.1 do libxenon compilado no host (hosttest/wifi/stress). */
#ifndef __ARCH_CC_H__
#define __ARCH_CC_H__
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t  u8_t;
typedef int8_t   s8_t;
typedef uint16_t u16_t;
typedef int16_t  s16_t;
typedef uint32_t u32_t;
typedef int32_t  s32_t;
typedef uintptr_t mem_ptr_t;
#define U32_F "u"
#define U16_F "hu"
#define S32_F "d"
#define S16_F "hd"
#define X32_F "x"
#define X16_F "hx"
#define SZT_F "zu"
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "lwIP ASSERT %s:%d: %s\n", __FILE__, __LINE__, x); abort(); } while (0)
#endif
