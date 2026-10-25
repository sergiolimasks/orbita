/* XCloud360 v0.17 — ver http_parse.h. */
#include "http_parse.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

enum { S_HDR = 0, S_BODY_CL, S_CH_SIZE, S_CH_DATA, S_CH_CRLF, S_CH_TRAILER, S_BODY_EOF, S_DONE, S_ERR };

void hp_init(http_parser_t *h, size_t body_max) {
    memset(h, 0, sizeof(*h));
    h->content_length = -1;
    h->body_max = body_max;
}

void hp_free(http_parser_t *h) {
    free(h->body);
    h->body = NULL;
    h->body_len = h->body_cap = 0;
}

static int body_append(http_parser_t *h, const uint8_t *p, size_t n) {
    if (h->body_len + n > h->body_max) return HP_ERR_TOOBIG;
    if (h->body_len + n + 1 > h->body_cap) {
        size_t nc = h->body_cap ? h->body_cap : 4096;
        while (nc < h->body_len + n + 1) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(h->body, nc);
        if (!nb) return HP_ERR_TOOBIG;
        h->body = nb;
        h->body_cap = nc;
    }
    if (n) memcpy(h->body + h->body_len, p, n);
    h->body_len += n;
    h->body[h->body_len] = 0;
    return 0;
}

static int ieq_prefix(const char *line, const char *name) {
    size_t l = strlen(name);
    return strncasecmp(line, name, l) == 0 && line[l] == ':';
}

static const char *hval(const char *line, const char *name) {
    const char *v = line + strlen(name) + 1;
    while (*v == ' ' || *v == '\t') v++;
    return v;
}

static void copy_val(char *dst, size_t cap, const char *v) {
    size_t i = 0;
    while (v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < cap) { dst[i] = v[i]; i++; }
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\t')) i--;
    dst[i] = 0;
}

/* Cabeçalho inteiro em h->hdr (terminado por \r\n\r\n). */
static int parse_headers(http_parser_t *h) {
    char *s = h->hdr;
    if (strncmp(s, "HTTP/1.", 7) != 0) return HP_ERR;
    char *sp = strchr(s, ' ');
    if (!sp) return HP_ERR;
    h->status = atoi(sp + 1);
    if (h->status < 100 || h->status > 999) return HP_ERR;
    char *line = strstr(s, "\r\n");
    while (line) {
        line += 2;
        if (line[0] == '\r' || line[0] == 0) break;
        char *end = strstr(line, "\r\n");
        if (!end) break;
        char save = *end;
        *end = 0;
        if (ieq_prefix(line, "Content-Length")) {
            h->content_length = strtol(hval(line, "Content-Length"), NULL, 10);
        } else if (ieq_prefix(line, "Transfer-Encoding")) {
            const char *v = hval(line, "Transfer-Encoding");
            size_t vl = strlen(v);
            while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
            if (vl >= 7 && strncasecmp(v + vl - 7, "chunked", 7) == 0) h->chunked = 1;
        } else if (ieq_prefix(line, "Date")) {
            copy_val(h->date, sizeof(h->date), hval(line, "Date"));
        } else if (ieq_prefix(line, "Content-Type")) {
            copy_val(h->content_type, sizeof(h->content_type), hval(line, "Content-Type"));
        }
        *end = save;
        line = end;
    }
    if (h->status == 204 || h->status == 304 || h->status < 200) h->head_only = 1;
    return 0;
}

int hp_feed(http_parser_t *h, const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        switch (h->st) {
        case S_HDR: {
            if (h->hdr_len >= HP_HDR_MAX) { h->st = S_ERR; return HP_ERR; }
            h->hdr[h->hdr_len++] = (char)p[i++];
            h->hdr[h->hdr_len] = 0;
            if (h->hdr_len >= 4 && memcmp(h->hdr + h->hdr_len - 4, "\r\n\r\n", 4) == 0) {
                if (parse_headers(h) < 0) { h->st = S_ERR; return HP_ERR; }
                if (h->status >= 100 && h->status < 200) {   /* 1xx: espera o próximo */
                    h->hdr_len = 0;
                    h->date[0] = 0;
                    h->content_length = -1;
                    h->chunked = 0;
                    h->head_only = 0;
                    break;
                }
                if (h->head_only) { h->st = S_DONE; return HP_DONE; }
                if (h->chunked) h->st = S_CH_SIZE;
                else if (h->content_length >= 0) {
                    if ((size_t)h->content_length > h->body_max) { h->st = S_ERR; return HP_ERR_TOOBIG; }
                    if (body_append(h, NULL, 0) < 0) { h->st = S_ERR; return HP_ERR_TOOBIG; }
                    if (h->content_length == 0) { h->st = S_DONE; return HP_DONE; }
                    h->st = S_BODY_CL;
                } else h->st = S_BODY_EOF;
            }
            break;
        }
        case S_BODY_CL: {
            size_t want = (size_t)h->content_length - h->body_len;
            size_t take = n - i < want ? n - i : want;
            int r = body_append(h, p + i, take);
            if (r < 0) { h->st = S_ERR; return r; }
            i += take;
            if (h->body_len == (size_t)h->content_length) { h->st = S_DONE; return HP_DONE; }
            break;
        }
        case S_BODY_EOF: {
            int r = body_append(h, p + i, n - i);
            if (r < 0) { h->st = S_ERR; return r; }
            i = n;
            break;
        }
        case S_CH_SIZE:
        case S_CH_CRLF:
        case S_CH_TRAILER: {
            char c = (char)p[i++];
            if (c == '\n') {
                while (h->line_len > 0 && h->line[h->line_len - 1] == '\r') h->line_len--;
                h->line[h->line_len] = 0;
                if (h->st == S_CH_SIZE) {
                    char *e;
                    if (h->line_len == 0) { h->st = S_ERR; return HP_ERR; }
                    unsigned long sz = strtoul(h->line, &e, 16);
                    if (e == h->line || (*e && *e != ';' && *e != ' ' && *e != '\t')) {
                        h->st = S_ERR; return HP_ERR;
                    }
                    h->line_len = 0;
                    if (sz == 0) {
                        if (body_append(h, NULL, 0) < 0) { h->st = S_ERR; return HP_ERR_TOOBIG; }
                        h->st = S_CH_TRAILER;
                    } else {
                        if (h->body_len + sz > h->body_max) { h->st = S_ERR; return HP_ERR_TOOBIG; }
                        h->chunk_left = sz;
                        h->st = S_CH_DATA;
                    }
                } else if (h->st == S_CH_CRLF) {
                    if (h->line_len != 0) { h->st = S_ERR; return HP_ERR; }
                    h->st = S_CH_SIZE;
                } else {   /* trailer: linha vazia termina */
                    if (h->line_len == 0) { h->st = S_DONE; return HP_DONE; }
                    h->line_len = 0;
                }
            } else {
                if (h->line_len + 1 >= sizeof(h->line)) {
                    if (h->st != S_CH_TRAILER) { h->st = S_ERR; return HP_ERR; }
                } else h->line[h->line_len++] = c;
            }
            break;
        }
        case S_CH_DATA: {
            size_t take = n - i < h->chunk_left ? n - i : h->chunk_left;
            int r = body_append(h, p + i, take);
            if (r < 0) { h->st = S_ERR; return r; }
            i += take;
            h->chunk_left -= take;
            if (h->chunk_left == 0) { h->line_len = 0; h->st = S_CH_CRLF; }
            break;
        }
        case S_DONE:
            return HP_DONE;
        default:
            return HP_ERR;
        }
    }
    return h->st == S_DONE ? HP_DONE : HP_OK_MORE;
}

int hp_eof(http_parser_t *h) {
    if (h->st == S_DONE) return HP_DONE;
    if (h->st == S_BODY_EOF) {
        if (body_append(h, NULL, 0) < 0) return HP_ERR;
        h->st = S_DONE;
        return HP_DONE;
    }
    return HP_ERR;
}

static int month_idx(const char *m) {
    static const char k[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int i = 0; i < 12; i++) if (strncmp(m, k + 3 * i, 3) == 0) return i;
    return -1;
}

/* dias desde 1970-01-01 (algoritmo civil de H. Hinnant) */
static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

long long hp_parse_http_date(const char *s) {
    /* IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT" */
    const char *c = strchr(s, ',');
    if (!c) return -1;
    c++;
    while (*c == ' ') c++;
    if (!isdigit((unsigned char)c[0])) return -1;
    int d = atoi(c);
    while (isdigit((unsigned char)*c)) c++;
    while (*c == ' ') c++;
    char mon[4] = {0};
    if (strlen(c) < 3) return -1;
    memcpy(mon, c, 3);
    c += 3;
    while (*c == ' ') c++;
    int y = atoi(c);
    while (isdigit((unsigned char)*c)) c++;
    while (*c == ' ') c++;
    if (strlen(c) < 8 || c[2] != ':' || c[5] != ':') return -1;
    int H = atoi(c), M = atoi(c + 3), S = atoi(c + 6);
    int mi = month_idx(mon);
    if (mi < 0 || d < 1 || d > 31 || y < 1970 || H > 23 || M > 59 || S > 60) return -1;
    return days_from_civil(y, (unsigned)mi + 1, (unsigned)d) * 86400LL + H * 3600 + M * 60 + S;
}
