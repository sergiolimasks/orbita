/*
 * XCloud360 stage 4 — pthread pieces usrsctp needs beyond deps/xenon-pthread
 * (which the app already links for the frame-threaded decoder: mutex, cond,
 * create, join). TARGET=xenon only. All WEAK so a fuller implementation wins.
 *
 * RULE: the whole WebRTC stack (libpeer + usrsctp + libsrtp + mbedTLS) is
 * driven from ONE hardware thread (see xc_webrtc.h). Under that rule usrsctp's
 * reader/writer locks are never contended, so they are plain counters here
 * (a real spinning rwlock could self-deadlock on usrsctp's nested
 * read-acquisitions, which POSIX permits).
 */
#include <errno.h>
#include <pthread.h>

__attribute__((weak)) int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) {
    (void)a;
    *l = 0;
    return 0;
}
__attribute__((weak)) int pthread_rwlock_destroy(pthread_rwlock_t *l) { (void)l; return 0; }
__attribute__((weak)) int pthread_rwlock_rdlock(pthread_rwlock_t *l) { (*l)++; return 0; }
__attribute__((weak)) int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) { (*l)++; return 0; }
__attribute__((weak)) int pthread_rwlock_wrlock(pthread_rwlock_t *l) { (*l)++; return 0; }
__attribute__((weak)) int pthread_rwlock_trywrlock(pthread_rwlock_t *l) { (*l)++; return 0; }
__attribute__((weak)) int pthread_rwlock_unlock(pthread_rwlock_t *l) { if (*l) (*l)--; return 0; }
__attribute__((weak)) int pthread_rwlockattr_init(pthread_rwlockattr_t *a) { (void)a; return 0; }
__attribute__((weak)) int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) { (void)a; return 0; }
__attribute__((weak)) int pthread_mutexattr_init(pthread_mutexattr_t *a) { (void)a; return 0; }
__attribute__((weak)) int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
__attribute__((weak)) int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t) { (void)a; (void)t; return 0; }
__attribute__((weak)) pthread_t pthread_self(void) {
    unsigned int pir;
    __asm__ __volatile__("mfspr %0, 1023" : "=r"(pir));
    return (pthread_t)pir;
}

/* usrsctp's timer THREAD body references nanosleep; with
 * usrsctp_init_nothreads() it never runs. Weak, millisecond granularity. */
#include <time.h>
#include "xcw_port.h"
__attribute__((weak)) int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    xcw_idle((int)(req->tv_sec * 1000 + req->tv_nsec / 1000000));
    return 0;
}
