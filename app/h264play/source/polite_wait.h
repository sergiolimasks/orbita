/*
 * XCloud360 — espera "educada" pra threads que dividem o núcleo com um
 * decodificador (SMT do Xenon). udelay() do libxenon é um laço apertado no
 * timebase: o irmão de SMT perde metade dos ciclos. Aqui a thread baixa a
 * prioridade de SMT (or 1,1,1) e cede ciclos (db16cyc) enquanto espera.
 */
#ifndef XCLOUD360_POLITE_WAIT_H
#define XCLOUD360_POLITE_WAIT_H
#include <ppc/timebase.h>

static inline void xc_polite_udelay(unsigned us) {
    uint64_t t0 = mftb();
    __asm__ __volatile__("or 1,1,1");
    while (tb_diff_usec(mftb(), t0) < us)
        __asm__ __volatile__("db16cyc\n\tdb16cyc\n\tdb16cyc\n\tdb16cyc" ::: "memory");
    __asm__ __volatile__("or 2,2,2");
}
#endif
