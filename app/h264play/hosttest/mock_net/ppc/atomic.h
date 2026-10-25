/* Host-test mock: single-threaded; checks that a lock is never taken twice
 * (would deadlock on the Xbox) and that every lock is released. */
#ifndef MOCK_PPC_ATOMIC_H
#define MOCK_PPC_ATOMIC_H
void lock(unsigned int *l);
void unlock(unsigned int *l);
#endif
