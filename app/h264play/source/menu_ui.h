/*
 * XCloud360 — desenho do menu direto no framebuffer do console (v0.16).
 *
 * Por que não printf/console_set_cursor: console_putch faz memdcbst do fb
 * INTEIRO (3.7 MB) a cada caractere, ~0.2 ms por letra — uma tela de menu
 * levaria meio segundo e piscaria. Aqui cada célula é escrita uma vez (fundo
 * + letra juntos, nada de limpar antes), com a fonte 8x16 do próprio
 * libxenon (fontdata_8x16) em escala 1..N, e um cache por linha pula o que
 * não mudou. O fb é o mesmo que o console usa (alias não-cacheado
 * |0x80000000, como fb_direct_init do main.c).
 *
 * Enquanto o menu está na tela, o stdout vai pra um buffer de log (as
 * últimas linhas aparecem no rodapé) em vez de rabiscar a tela. Só o main
 * desenha; o hook de log pode ser chamado por qualquer thread (lock).
 */
#ifndef XCLOUD360_MENU_UI_H
#define XCLOUD360_MENU_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_RGB(r, g, b) (((uint32_t)(b) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(r) << 8))

#define UI_LOG_LINES 4
#define UI_LOG_COLS  150

/* Relê endereço/tamanho do fb do scanout atual. 0 = ok. */
int  ui_attach(void);
int  ui_width(void);
int  ui_height(void);
void ui_clear(uint32_t bg);
/* Esquece o cache de linhas (próximo ui_line redesenha tudo). */
void ui_invalidate(void);
/* Linha de texto em (x,y) px, `cols` células (completa com espaço, corta o
 * resto). `slot` identifica a linha no cache (0..UI_SLOTS-1). */
#define UI_SLOTS 64
void ui_line(int slot, int x, int y, int scale, uint32_t fg, uint32_t bg,
             int cols, const char *text);
/* Retângulo sólido (fundo da barra de título, cursor). */
void ui_fill(int x, int y, int w, int h, uint32_t color);
/* Garante que o que foi escrito chegou na RAM que o scanout lê. */
void ui_flush(void);

/* stdout -> buffer de log (instalar de novo depois de console_open). */
void ui_log_install(void);
/* Copia as últimas linhas (mais antiga primeiro); retorna o seq atual (muda
 * quando chega texto novo). */
unsigned ui_log_get(char out[UI_LOG_LINES][UI_LOG_COLS]);

#ifdef __cplusplus
}
#endif

#endif
