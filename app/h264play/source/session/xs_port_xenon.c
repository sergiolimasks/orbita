/* XCloud360 stage 3 — xs_port.h no Xbox. */
#ifdef XENON
#include "xs_port.h"
#include "../xc_lock.h"

/* v0.21.14: instrumentado (dono/lugar/espera pro tripwire) */
static xlk_t g_xs_lock = XLK_INIT("sessão (xs_lock)");

void xs_lock(void) { xlk_lock(&g_xs_lock, "xs_lock"); }

void xs_unlock(void) {
    __asm__ __volatile__("lwsync" ::: "memory");   /* escritas visíveis antes de soltar */
    xlk_unlock(&g_xs_lock);
}
#endif
