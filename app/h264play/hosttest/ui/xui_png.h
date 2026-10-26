#ifndef XCLOUD360_XUI_PNG_H
#define XCLOUD360_XUI_PNG_H
#include <stdint.h>
/* px = 0x00RRGGBB, stride em pixels. 0 = ok. */
int xui_png_write(const char *path, const uint32_t *px, int w, int h, int stride);
#endif
