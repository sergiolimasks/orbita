/* Force-included into gpu_present.c for the host test (see run.sh). */
#include <stdint.h>
extern uint32_t mock_regs[];
#define GP_REGS_BASE ((uintptr_t)mock_regs)
#define GP_SPIN_PAUSE() do { } while (0)
