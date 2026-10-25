/* host-test stand-in for libxenon nocfe/cfe.h, for compiling the REAL ohci.c.
 * Memory comes from an arena mapped twice (at A and A|0x80000000), so
 * libxenon's PHYSADDR/UNCADDR address games work as on the Xbox. */
#ifndef OHCITEST_CFE_H
#define OHCITEST_CFE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lib_queue.h"

typedef unsigned int physaddr_t;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define PHYSADDR(x)  ((x) & ~0x80000000UL)
#define KERNADDR(x)  ((x) | 0x80000000UL)
#define UNCADDR(x)   ((x) | 0x80000000UL)

void *mock_kmalloc(size_t n, int align);
void mock_kfree(void *p);
#define KMALLOC(size, align) mock_kmalloc((size), (align))
#define KFREE(ptr) mock_kfree(ptr)

uint32_t read32(uint64_t addr);
void write32(uint64_t addr, uint32_t v);
void memdcbst(void *a, int l);
void memdcbf(void *a, int l);
#define console_log(fmt, x...) printf(fmt "\n", x)
#endif
