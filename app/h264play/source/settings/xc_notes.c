/*
 * orbita — XC360/NOTES.TXT (v0.21.33). Ver xc_notes.h.
 */
#include "xc_notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xcn_notes_format(const char *version, long long installed, const char *notes, char *out, size_t cap) {
    if (!version || !version[0] || strchr(version, '\n')) return -1;
    int n = snprintf(out, cap, "XCN1\n%s\n%lld\n%.*s", version, installed < 0 ? -1LL : installed, XCN_NOTES_MAX,
                     notes ? notes : "");
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/* próxima linha de [*p, e) em buf (sem \r\n); 0 = acabou */
static int take_line(const char **p, const char *e, char *buf, size_t cap) {
    if (*p >= e) return 0;
    const char *nl = memchr(*p, '\n', (size_t)(e - *p));
    const char *le = nl ? nl : e;
    size_t k = (size_t)(le - *p);
    if (k && (*p)[k - 1] == '\r') k--;
    if (k >= cap) return 0;
    memcpy(buf, *p, k);
    buf[k] = 0;
    *p = nl ? nl + 1 : e;
    return 1;
}

int xcn_notes_parse(const char *s, size_t n, xcn_notes_t *o) {
    memset(o, 0, sizeof(*o));
    o->installed = -1;
    if (!s) return -1;
    const char *p = s, *e = s + n;
    char l[40];
    if (!take_line(&p, e, l, sizeof(l)) || strcmp(l, "XCN1")) return -1;
    if (!take_line(&p, e, o->version, sizeof(o->version)) || !o->version[0]) return -1;
    if (!take_line(&p, e, l, sizeof(l))) return -1;
    char *end = NULL;
    long long t = strtoll(l, &end, 10);
    if (end == l || *end) return -1;
    o->installed = t > 0 ? t : -1;
    size_t k = (size_t)(e - p);
    if (k > XCN_NOTES_MAX) k = XCN_NOTES_MAX;
    memcpy(o->notes, p, k);
    o->notes[k] = 0;
    /* tira o \n / espaços do fim */
    while (k && (o->notes[k - 1] == '\n' || o->notes[k - 1] == '\r' || o->notes[k - 1] == ' ')) o->notes[--k] = 0;
    return 0;
}
