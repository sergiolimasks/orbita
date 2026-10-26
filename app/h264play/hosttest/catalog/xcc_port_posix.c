/* XCloud360 — xcc_port.h no host (pthread) pros testes do catálogo. */
#include "../../source/catalog/xcc_port.h"

#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_busy = 0;   /* "hw 3" ocupada */
static void          (*g_fn)(void);

void xccp_lock(void)   { pthread_mutex_lock(&g_mx); }
void xccp_unlock(void) { pthread_mutex_unlock(&g_mx); }
void xccp_sync(void)   { __sync_synchronize(); }

/* Testes simulam o login/sessão ocupando a hw. */
void xccp_test_set_busy(int b) { g_busy = b; }
int  xccp_slot_busy(void) { return g_busy; }

static void *tramp(void *a) {
    (void)a;
    g_fn();
    g_busy = 0;   /* como o libxenon zera o slot quando a função volta */
    return NULL;
}

int xccp_spawn(void (*fn)(void)) {
    if (g_busy) return -1;
    g_fn = fn;
    g_busy = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, tramp, NULL) != 0) { g_busy = 0; return -1; }
    pthread_detach(t);
    return 0;
}
