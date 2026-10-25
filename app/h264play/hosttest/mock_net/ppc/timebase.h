/* Host-test mock (ctrl_test.c): timebase the test sets by hand. */
#ifndef MOCK_NET_TIMEBASE_H
#define MOCK_NET_TIMEBASE_H
#include <stdint.h>
#define PPC_TIMEBASE_FREQ (3192000000LL/64LL)
uint64_t mock_mftb(void);
#define mftb() mock_mftb()
static inline unsigned long tb_diff_msec(uint64_t end, uint64_t start) {
    return (end - start) / (PPC_TIMEBASE_FREQ / 1000);
}
#endif
