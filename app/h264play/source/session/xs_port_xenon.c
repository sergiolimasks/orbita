/* XCloud360 stage 3 — xs_port.h no Xbox. */
#ifdef XENON
#include "xs_port.h"
#include <ppc/atomic.h>

static unsigned int g_xs_lock = 0;

void xs_lock(void) { lock(&g_xs_lock); }

void xs_unlock(void) {
    __asm__ __volatile__("lwsync" ::: "memory");   /* escritas visíveis antes de soltar */
    unlock(&g_xs_lock);
}
#endif
