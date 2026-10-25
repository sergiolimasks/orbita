/* Host-test mock: simulated timebase (sim.c). Each read advances 1 us so
 * spin loops make progress. */
#ifndef MOCK_TIMEBASE_H
#define MOCK_TIMEBASE_H
#include <stdint.h>
#define PPC_TIMEBASE_FREQ (3192000000LL/64LL)
uint64_t mock_mftb(void);
#define mftb() mock_mftb()
#endif
