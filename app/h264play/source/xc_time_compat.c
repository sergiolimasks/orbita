/* newlib's clock() calls times(), which libxenon does not provide.
 * Bare metal has one application and no process/child CPU accounting;
 * expose its elapsed hardware time in the clock units newlib expects. */
#ifdef XENON
#include <stdint.h>
#include <sys/times.h>
#include <time.h>
#include <ppc/timebase.h>

clock_t times(struct tms *out) {
    uint32_t hi, lo, check;
    /* Reading the low half across a rollover must not move time backwards. */
    do {
        __asm__ __volatile__("mftbu %0" : "=r"(hi));
        __asm__ __volatile__("mftbl %0" : "=r"(lo));
        __asm__ __volatile__("mftbu %0" : "=r"(check));
    } while (hi != check);
    uint64_t tb = ((uint64_t)hi << 32) | lo;
    uint64_t ticks = (tb / PPC_TIMEBASE_FREQ) * CLOCKS_PER_SEC
                   + (tb % PPC_TIMEBASE_FREQ) * CLOCKS_PER_SEC / PPC_TIMEBASE_FREQ;
    clock_t elapsed = (clock_t)ticks;
    if (out) {
        out->tms_utime = elapsed;
        out->tms_stime = 0;
        out->tms_cutime = 0;
        out->tms_cstime = 0;
    }
    return elapsed;
}
#endif
