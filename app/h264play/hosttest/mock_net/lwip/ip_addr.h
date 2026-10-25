#ifndef MOCK_LWIP_IP_ADDR_H
#define MOCK_LWIP_IP_ADDR_H
#include <stdint.h>
typedef struct { uint32_t addr; } ip_addr_t;
int ipaddr_aton(const char *cp, ip_addr_t *addr);
#endif
