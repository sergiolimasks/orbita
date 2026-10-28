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

/* XCloud360 v0.21.26:
 *  - xpt_park_new: pthreads criadas com isso ligado são "estacionadas" (nunca
 *    rodam, não ocupam hw thread). O app liga em volta do xn_init: o
 *    usrsctp_init_nothreads ainda cria a thread do iterator do SCTP, que
 *    ficava pra sempre na hw 4 esperando trabalho que nunca vem, e o decoder
 *    acabava nas hw 2,5,3 (a 3 sem o aluguel, o que travava a volta pra 4).
 *  - xpt_set_slots: o decoder diz em quais hw threads as threads de frame
 *    podem ficar (4,2,5 ou 4,2,5,3 com a hw 3 alugada).
 *  - xpt_last_ids: onde as últimas pthreads foram parar (log do app). */
volatile int xpt_park_new = 0;
volatile int xpt_parked = 0;
#define XPT_PARKED ((pthread_t)0x50a4)
static const int *volatile g_slots_ovr = 0;
static volatile int g_nslots_ovr = 0;
volatile int xpt_last_ids[8];
volatile int xpt_n_last = 0;
void xpt_set_slots(const int *ids, int n) {
    g_nslots_ovr = ids ? n : 0;
    g_slots_ovr = ids;
    __asm__ __volatile__("sync" ::: "memory");
}
void xpt_track_reset(void) { xpt_n_last = 0; }

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;
    if (xpt_park_new) {
        *thread = XPT_PARKED;
        xpt_parked++;
        return 0;
    }
    const int *slots = g_slots_ovr ? g_slots_ovr : k_slots;
    int nslots = g_slots_ovr ? g_nslots_ovr : XPT_NSLOTS;
    for (int i = 0; i < nslots; i++) {
        int id = slots[i];
        if (id < 1 || id > 5) continue;
        if (xenon_is_thread_task_running(id)) continue;
        g_task[id].fn  = start_routine;
        g_task[id].arg = arg;
        g_task[id].ret = NULL;
        __asm__ __volatile__("sync" ::: "memory");
        if (xenon_run_thread_task(id, g_stack[id] + XPT_STACK_SIZE - 0x100,
                                  (void *)xpt_runner) == 0) {
            *thread = (pthread_t)id;
            if (xpt_n_last < 8) xpt_last_ids[xpt_n_last++] = id;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_join(pthread_t thread, void **value_ptr) {
    if (thread == XPT_PARKED) { if (value_ptr) *value_ptr = NULL; return 0; }   /* nunca rodou */
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
/* XCloud360 v0.21.12: chamado de vez em quando nas esperas OCIOSAS (a thread
 * não tem trabalho: nada a mais no decode); o app põe o tripwire do vigia. */
void (*volatile xpt_idle_hook)(void) = 0;
/* XCloud360 v0.21.25: tempo ocioso por hw thread (espera em cond) e SMT baixa
 * enquanto espera: o irmão do núcleo (outra thread do decoder, ou o main)
 * fica com o núcleo (DIAG.TXT spinprio=med desliga). */
volatile uint64_t xpt_idle_tb[8];
volatile int xpt_spin_low = 0;
static inline uint64_t xpt_tb(void) {
    uint32_t hi, lo, hi2;
    do {
        __asm__ __volatile__("mftbu %0" : "=r"(hi));
        __asm__ __volatile__("mftb %0" : "=r"(lo));
        __asm__ __volatile__("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

/* XCloud360 v0.21.35: quem espera onde. xpt_site = linha do pthread.c do
 * libavcodec que vai esperar (o chamador põe antes); xpt_wait_* = a espera em
 * curso por hw thread (tb de início, 0 = não espera). Só leitura pro vigia.
 * xpt_dec_abort: o app manda soltar as esperas de progresso do decoder
 * (deadlock do frame threading); xpt_abort_gen acorda quem já está esperando
 * (acordar a mais é permitido: todo cond_wait do libavcodec está num laço
 * que reconfere o predicado). */
volatile int xpt_site[8];
volatile int xpt_wait_site[8];
volatile uint64_t xpt_wait_tb[8];
volatile int xpt_dec_abort = 0;
volatile unsigned int xpt_abort_gen = 0;
volatile unsigned int xpt_dec_released = 0;

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    unsigned int gen = *(volatile pthread_cond_t *)c;
    unsigned int agen = xpt_abort_gen;
    int me = cur_pir() & 7;
    pthread_mutex_unlock(m);
    unsigned int spins = 0;
    uint64_t t0 = xpt_tb();
    xpt_wait_site[me] = xpt_site[me];
    xpt_wait_tb[me] = t0;
    int low = xpt_spin_low;
    if (low) __asm__ __volatile__("or 1,1,1");
    while (*(volatile pthread_cond_t *)c == gen && xpt_abort_gen == agen) {
        spin_pause();
        if ((++spins & 0x3fff) == 0) {
            void (*h)(void) = xpt_idle_hook;
            if (h) h();
        }
    }
    if (low) __asm__ __volatile__("or 2,2,2");
    xpt_wait_tb[me] = 0;
    xpt_site[me] = 0;
    xpt_idle_tb[me] += xpt_tb() - t0;
    pthread_mutex_lock(m);
    return 0;
}
