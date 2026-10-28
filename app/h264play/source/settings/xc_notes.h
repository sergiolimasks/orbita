/*
 * orbita — "Novidades desta versão" (v0.21.33, MENU-SETTINGS-PLAN.md §4).
 *
 * XC360/NOTES.TXT é gravado pelo worker da atualização (hw 3) quando a
 * instalação termina (XCU_DONE), com as notas do manifesto assinado:
 *   XCN1
 *   <versão instalada>
 *   <hora Unix da instalação, ou -1>
 *   <notas, UTF-8, até o fim>
 * No boot o main lê; só vale se <versão> = XCLOUD360_BUILD_VERSION. Sem
 * arquivo, mas a checagem achou latest == current: as notas do manifesto.
 * C puro (hosttest/settings).
 */
#ifndef XC_NOTES_H
#define XC_NOTES_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XCN_FILE      "NOTES.TXT"
#define XCN_PATH      "XC360/NOTES.TXT"
#define XCN_NOTES_MAX 512
#define XCN_FILE_MAX  (XCN_NOTES_MAX + 96)

typedef struct {
    char      version[32];
    long long installed;          /* hora Unix, -1 = não sabe */
    char      notes[XCN_NOTES_MAX + 1];
} xcn_notes_t;

/* bytes (sem NUL) ou -1 */
int xcn_notes_format(const char *version, long long installed, const char *notes, char *out, size_t cap);
/* 0 = ok; -1 = arquivo ruim */
int xcn_notes_parse(const char *s, size_t n, xcn_notes_t *o);

#ifdef __cplusplus
}
#endif
#endif
