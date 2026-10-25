#ifndef MOCK_LWIP_ERR_H
#define MOCK_LWIP_ERR_H
#include <stdint.h>
typedef int8_t   err_t;
typedef uint8_t  u8_t;
typedef uint16_t u16_t;
#define ERR_OK    0
#define ERR_MEM  -1
#define ERR_ABRT -10
#define ERR_RST  -11
#endif
