/* Separate object, like libxenon's edram.o: its Xe_Fatal call is an
 * undefined reference, so -Wl,--wrap=Xe_Fatal routes it to gpu_present.c. */
#include "xenos/xe.h"
void Xe_Fatal(struct XenosDevice *xe, const char *fmt, ...);
void edram_fail_hook(struct XenosDevice *xe);
void edram_fail_hook(struct XenosDevice *xe) { Xe_Fatal(xe, "damnit, EDRAM init failed, again and again (mock)"); }
