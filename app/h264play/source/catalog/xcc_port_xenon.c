/* XCloud360 — xcc_port.h no Xbox (libxenon). */
#ifdef XENON
#include "xcc_port.h"
#include "xc_catalog.h"

#include <stdint.h>
#include <ppc/atomic.h>
#include <xenon_soc/xenon_power.h>

/* Pilha própria (o handshake TLS + cJSON + stb_image pedem folga; mesma
 * conta da thread do login). A hw 3 só roda um de nós por vez. */
static uint8_t       g_stack[256 * 1024] __attribute__((aligned(128)));
static unsigned int  g_lock = 0;

void xccp_lock(void)   { lock(&g_lock); }
void xccp_unlock(void) { unlock(&g_lock); }
void xccp_sync(void)   { __asm__ __volatile__("lwsync" ::: "memory"); }

int xccp_slot_busy(void) { return xenon_is_thread_task_running(XCC_HW_THREAD) ? 1 : 0; }

int xccp_spawn(void (*fn)(void)) {
    if (xccp_slot_busy()) return -1;
    return xenon_run_thread_task(XCC_HW_THREAD, g_stack + sizeof(g_stack) - 0x100, (void *)fn) == 0 ? 0 : -1;
}
#endif
