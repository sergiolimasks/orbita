/* host-test stand-in for libxenon drivers/nocfe/cfe.h */
#ifndef MOCK_CFE_H
#define MOCK_CFE_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <input/input.h>

typedef struct queue_s { struct queue_s *q_next, *q_prev; } queue_t;
typedef unsigned int physaddr_t;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

void *mock_kmalloc(size_t n, int align);
void mock_kfree(void *p);
#define KMALLOC(size, align) mock_kmalloc((size), (align))
#define KFREE(ptr) mock_kfree(ptr)
#endif
