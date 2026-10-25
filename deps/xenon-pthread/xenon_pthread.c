/*
 * XCloud360 — pthread mínimo pra libxenon, suficiente pro frame threading
 * do libavcodec 0.10 (mutex, cond, create, join; atributos sempre NULL).
 *
 * Tipos vêm do <pthread.h> do newlib (pthread_mutex_t / pthread_cond_t são
 * uint32), newlib não implementa nada — as funções ficam aqui.
 *
 * Diferenças vs ~/xmplayer/mplayer/libxenon_miss/xenon_pthread.c:
 *  - threads de hardware escolhidas explicitamente (k_slots): o worker de
 *    convert do h264play mora na thread 2 e não pode ser atropelado;
 *  - pilha de 256KB por thread (xmplayer usava 64KB — curto pro H.264);
 *  - cond com contador de geração (signal/broadcast acordam de verdade;
 *    no xmplayer cond_wait era só unlock+lock, um spin cego);
 *  - espera em spin com db16cyc pra ceder ciclos ao SMT irmão do núcleo.
 *
 * Mapa de threads de hardware (3 núcleos × 2 SMT, PIR 0..5):
 *   núcleo 0: 0 = main (rede/input/dispatch)   1 = decode
 *   núcleo 1: 2 = worker convert YUV->RGB      3 = decode
 *   núcleo 2: 4 = decode                       5 = decode
 * k_slots em ordem de preferência: espalha por núcleo antes de usar o SMT.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

#include <ppc/atomic.h>
#include <xenon_soc/xenon_power.h>

#define XPT_STACK_SIZE (256 * 1024)

#ifdef XCLOUD360_GPU
/* v0.15 (make GPU=1): YUV->RGB vai pra GPU, então não existe worker de
 * convert na thread 2 nem metade de baixo na thread 1. Mapa:
 *   núcleo 0: 0 = main (decode dispatch + upload/draw Xe_*)  1 = rede (lwIP)
 *   núcleo 1: 2 = decode                                      3 = decode
 *   núcleo 2: 4 = decode                                      5 = decode
 * Ordem 4,2,5,3 e só então 1 (irmã do main; normalmente ocupada pela rede,
 * livre no modo embedded). Se o init da GPU falhar, o main volta pro
 * caminho de CPU: o worker ocupa a 2 antes do decoder abrir e é pulado. */
/* v0.15.2: 4,2,5,3 (era 4,5,2,3): espalha por núcleo antes do SMT — com
 * DECODE_THREADS=2 o 4,5 caía inteiro no núcleo 2. */
static const int k_slots[] = { 4, 2, 5, 3, 1 };
#else
static const int k_slots[] = { 4, 1, 5, 3 };
#endif
#define XPT_NSLOTS ((int)(sizeof(k_slots) / sizeof(k_slots[0])))

typedef struct {
    void *(*fn)(void *);
    void *arg;
    void *ret;
} xpt_task_t;

static xpt_task_t g_task[6] __attribute__((aligned(128)));
static uint8_t    g_stack[6][XPT_STACK_SIZE] __attribute__((aligned(128)));

static inline unsigned int cur_pir(void) {
    unsigned int pir;
    __asm__ __volatile__("mfspr %0, 1023" : "=r"(pir));
    return pir;
}

static inline void spin_pause(void) {
    __asm__ __volatile__("db16cyc" ::: "memory");
}

static void xpt_runner(void) {
    unsigned int id = cur_pir();
    __asm__ __volatile__("lwsync" ::: "memory");
    g_task[id].ret = g_task[id].fn(g_task[id].arg);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;
    for (int i = 0; i < XPT_NSLOTS; i++) {
        int id = k_slots[i];
        if (xenon_is_thread_task_running(id)) continue;
        g_task[id].fn  = start_routine;
        g_task[id].arg = arg;
        g_task[id].ret = NULL;
        __asm__ __volatile__("sync" ::: "memory");
        if (xenon_run_thread_task(id, g_stack[id] + XPT_STACK_SIZE - 0x100,
                                  (void *)xpt_runner) == 0) {
            *thread = (pthread_t)id;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_join(pthread_t thread, void **value_ptr) {
    while (xenon_is_thread_task_running((int)thread)) spin_pause();
    __asm__ __volatile__("lwsync" ::: "memory");
    if (value_ptr) *value_ptr = g_task[thread].ret;
    return 0;
}

/* ---- mutex: spinlock do libxenon (lwarx/stwcx., já faz db16cyc) ---- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    *m = 0;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m)    { lock((unsigned int *)m); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m)  { unlock((unsigned int *)m); return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (*(volatile pthread_mutex_t *)m) return EBUSY;
    lock((unsigned int *)m);
    return 0;
}

/* ---- cond: contador de geração ----
 * wait lê a geração com o mutex na mão, solta, espera ela mudar, retoma.
 * O libavcodec sempre sinaliza segurando o mesmo mutex e sempre reconfere o
 * predicado em loop, então acordar a mais (broadcast em vez de signal) é ok. */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    *c = 0;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_signal(pthread_cond_t *c)    { atomic_inc((unsigned int *)c); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { atomic_inc((unsigned int *)c); return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    unsigned int gen = *(volatile pthread_cond_t *)c;
    pthread_mutex_unlock(m);
    while (*(volatile pthread_cond_t *)c == gen) spin_pause();
    pthread_mutex_lock(m);
    return 0;
}
