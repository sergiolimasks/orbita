/* XCloud360 v0.21.38 — idioma do aplicativo.
 *
 * L("texto em português") devolve o texto no idioma escolhido nas
 * Configurações (XC360/UILANG.TXT). A chave é o próprio texto em português;
 * sem tradução na tabela (xc_i18n_tab.h, gerada por tools/i18n_tr.py) ele
 * fica em português, então texto novo nunca quebra. */
#ifndef XC_I18N_H
#define XC_I18N_H
#include <stdint.h>

enum { XI_PT = 0, XI_EN, XI_ES, XI_NLANG };
extern volatile int xi_lang;
extern const char *const XI_CODES[XI_NLANG];   /* "pt", "en", "es" (UILANG.TXT) */

void        xi_init(void);
const char *L(const char *pt);

#endif
