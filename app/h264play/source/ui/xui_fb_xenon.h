/* XCloud360 — XUI backend Xbox (framebuffer do console). Ver xui_fb_xenon.c. */
#ifndef XCLOUD360_XUI_FB_XENON_H
#define XCLOUD360_XUI_FB_XENON_H

#include "xui.h"

/* (Re)lê o fb do scanout atual e aloca o canvas do tamanho dele. 0 = ok.
 * Chamar ao entrar no menu (depois do gpu_present_finish devolver o
 * scanout pro console), junto com xui_view_invalidate(). */
int           xui_fb_attach(void);
xui_canvas_t *xui_fb_canvas(void);
/* Copia o retângulo sujo do canvas pro fb. */
void          xui_fb_present(xui_rect_t r);

#endif
