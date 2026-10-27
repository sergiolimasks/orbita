/* XCloud360 v0.20.1 (v0.20.3: ID de hardware + X-Console) — log dos testadores beta pra nuvem. Ver xc_logup.h e
 * LOGS-UPLOAD.md. Portável: xc_port.h + https.h (Xbox e host). */
#include "xc_logup.h"
#include "xc_port.h"
#include "https.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- lock: qualquer thread escreve, ninguém espera nada com ele ---- */
/* v0.21.14: lock instrumentado (folha: nada é pego com ele seguro). As
 * linhas (xlu_line) só TENTAM: quem loga nunca espera o envio/snapshot. */
#include "../xc_lock.h"
static xlk_t g_lk = XLK_INIT("log beta (xlu)");
static void lk(void) { xlk_lock(&g_lk, "xlu"); }
static void ulk(void) { xlk_unlock(&g_lk); }
static unsigned g_line_drops = 0;
unsigned xlu_line_drops(void) { return g_line_drops; }

#define NSECRETS 8

/* ---- compartilhado (lock) ---- */
static char     g_buf[XLU_BUF_BYTES];      /* linhas terminadas em NUL, em ordem */
static size_t   g_len;
static unsigned long long g_base;          /* posição lógica de g_buf[0] */
static int      g_inited;
static char     g_app[24], g_game[96];
/* v0.20.5: último jogo das linhas ainda não enviadas. O lote do jogo só sai
 * depois da sessão, quando g_game já voltou a "" (o campo chegava vazio). */
static char     g_batch_game[96];
static char     g_secret[NSECRETS][72];
static int      g_secret_next;
static volatile int g_flush;
static int      g_beta;                    /* XC_BETA_* */
static char     g_id[33];
static char     g_newkey[72];              /* xc_beta_save_key -> worker */
static int      g_newkey_on, g_newkey_save;
static xlu_stats_t g_st;
/* ---- só o worker ---- */
static xlu_post_fn g_post;
static char     g_key[72];
static unsigned long long g_last_send, g_backoff_until;
static unsigned g_backoff_ms;
static size_t   g_batch_max = XLU_BATCH_MAX;
static https_conn_t *g_conn;

/* ------------------------------------------------------------ filtro */

static int tokc(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' ||
           c == '_' || c == '.' || c == '+' || c == '/' || c == '=';
}
static int hexc(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-';
}

/* troca [a, a+n) por rep, no lugar */
static void splice(char *s, size_t cap, size_t a, size_t n, const char *rep) {
    size_t l = strlen(s), rl = strlen(rep);
    if (a + n > l) n = l - a;
    if (l - n + rl + 1 > cap) rl = 0;
    memmove(s + a + rl, s + a + n, l - a - n + 1);
    memcpy(s + a, rep, rl);
}

void xlu_redact(char *s, size_t cap) {
    /* 1. segredos registrados (código de login, a chave beta) */
    char sec[NSECRETS][72];
    lk();
    memcpy(sec, g_secret, sizeof(sec));
    ulk();
    for (int k = 0; k < NSECRETS; k++) {
        size_t sl = strlen(sec[k]);
        if (sl < 4) continue;
        char *p;
        while ((p = strstr(s, sec[k])) != NULL) splice(s, cap, (size_t)(p - s), sl, "[x]");
    }
    /* 2. sequências longas com cara de token (JWT, base64): >= 32 chars,
     * letras e dígitos. Só hexa/hífen (UUID, hash) não é segredo e fica. */
    for (size_t i = 0; s[i];) {
        if (!tokc((unsigned char)s[i])) { i++; continue; }
        size_t j = i;
        int letter = 0, digit = 0, nonhex = 0;
        for (; s[j] && tokc((unsigned char)s[j]); j++) {
            unsigned char c = (unsigned char)s[j];
            if (c >= '0' && c <= '9') digit = 1;
            else if ((c | 32) >= 'a' && (c | 32) <= 'z') letter = 1;
            if (!hexc(c)) nonhex = 1;
        }
        if (j - i >= 32 && letter && digit && nonhex) {
            splice(s, cap, i, j - i, "[token]");
            i += 7;
        } else {
            i = j;
        }
    }
    /* 3. e-mails */
    for (size_t i = 0; s[i]; i++) {
        if (s[i] != '@' || i == 0) continue;
        size_t a = i, b = i + 1;
        while (a > 0 && tokc((unsigned char)s[a - 1])) a--;
        while (s[b] && tokc((unsigned char)s[b])) b++;
        if (a < i && memchr(s + i + 1, '.', b - i - 1)) {
            splice(s, cap, a, b - a, "[email]");
            i = a + 6;
        }
    }
}

/* ------------------------------------------------------------ buffer */

void xlu_init(const char *app) {
    lk();
    snprintf(g_app, sizeof(g_app), "%s", app ? app : "");
    g_inited = 1;
    ulk();
}

void xlu_set_transport(xlu_post_fn post) { g_post = post; }

static unsigned long long (*g_clock)(void);
void xlu_set_clock(unsigned long long (*now_ms)(void)) { g_clock = now_ms; }

void xlu_reset(void) {
    lk();
    g_len = 0; g_base = 0; g_flush = 0; g_game[0] = 0; g_batch_game[0] = 0;
    memset(g_secret, 0, sizeof(g_secret));
    g_secret_next = 0;
    g_beta = XC_BETA_UNKNOWN;
    g_id[0] = 0; g_newkey[0] = 0; g_newkey_on = 0; g_newkey_save = 0;
    memset(&g_st, 0, sizeof(g_st));
    ulk();
    g_key[0] = 0; g_last_send = 0; g_backoff_until = 0; g_backoff_ms = 0;
    g_batch_max = XLU_BATCH_MAX;
}

static size_t cap_now(void) {   /* lock */
    return (g_beta == XC_BETA_NONE || g_beta == XC_BETA_INVALID) ? XLU_MIN_BYTES : XLU_BUF_BYTES;
}

/* lock: tira linhas antigas até o buffer ter no máximo `keep` bytes */
static void trim_to(size_t keep) {
    size_t cut = 0;
    unsigned n = 0;
    while (cut < g_len && g_len - cut > keep) {
        cut += strlen(g_buf + cut) + 1;
        n++;
    }
    if (!cut) return;
    memmove(g_buf, g_buf + cut, g_len - cut);
    g_len -= cut;
    g_base += cut;
    g_st.lines_dropped += n;
}

void xlu_line(const char *text) {
    if (!text || !g_inited) return;
    char l[XLU_LINE_MAX + 64];
    size_t n = 0;
    for (const char *p = text; *p && n < XLU_LINE_MAX; p++) l[n++] = ((unsigned char)*p < 32) ? ' ' : *p;
    l[n] = 0;
    xlu_redact(l, sizeof(l));
    n = strlen(l);
    if (!xlk_try(&g_lk, "xlu_line", 2000)) { g_line_drops++; return; }
    size_t cap = cap_now();
    if (g_len + n + 1 > cap) {
        /* cheio: descarta um quarto de uma vez (não move 200 KB por linha) */
        size_t keep = cap - n - 1;
        if (keep > cap * 3 / 4) keep = cap * 3 / 4;
        trim_to(keep);
    }
    if (g_len + n + 1 <= cap) {
        memcpy(g_buf + g_len, l, n + 1);
        g_len += n + 1;
    }
    ulk();
}

void xlu_set_game(const char *name) {
    lk();
    snprintf(g_game, sizeof(g_game), "%s", name ? name : "");
    if (g_game[0]) memcpy(g_batch_game, g_game, sizeof(g_batch_game));
    ulk();
}

void xlu_flush_soon(void) { g_flush = 1; }

void xlu_add_secret(const char *s) {
    if (!s || strlen(s) < 4 || strlen(s) >= sizeof(g_secret[0])) return;
    lk();
    for (int k = 0; k < NSECRETS; k++) if (!strcmp(g_secret[k], s)) { ulk(); return; }
    snprintf(g_secret[g_secret_next], sizeof(g_secret[0]), "%s", s);
    g_secret_next = (g_secret_next + 1) % NSECRETS;
    ulk();
}

void xlu_stats(xlu_stats_t *s) {
    lk();
    *s = g_st;
    s->buffered = g_len;
    s->beta = g_beta;
    s->backoff_until_ms = g_backoff_until;
    ulk();
}

/* ------------------------------------------------------- beta / ID */

/* apara as pontas e valida: 8-64 ASCII imprimível sem espaço no meio */
static int key_norm(char *out, size_t cap, const char *in) {
    if (!in) return -1;
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') in++;
    size_t n = strlen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '\r' || in[n - 1] == '\n')) n--;
    const char *nl = memchr(in, '\n', n);   /* só a 1a linha */
    if (nl) {
        n = (size_t)(nl - in);
        while (n && (in[n - 1] == ' ' || in[n - 1] == '\r' || in[n - 1] == '\t')) n--;
    }
    if (n < 8 || n > 64 || n >= cap) return -1;
    for (size_t i = 0; i < n; i++) if ((unsigned char)in[i] <= 32 || (unsigned char)in[i] >= 127) return -1;
    memcpy(out, in, n);
    out[n] = 0;
    return 0;
}

int xc_beta_active(void) { return xc_beta_state() == XC_BETA_ACTIVE; }

int xc_beta_state(void) {
    lk();
    int b = g_beta;
    ulk();
    return b;
}

int xc_console_id(char *out, size_t cap) {
    if (!out || !cap) return 0;
    lk();
    snprintf(out, cap, "%s", g_id);
    ulk();
    return (int)strlen(out);
}

static int new_key(const char *key, int save) {
    char k[72];
    if (key_norm(k, sizeof(k), key) != 0) return -1;
    lk();
    memcpy(g_newkey, k, sizeof(k));
    g_newkey_on = 1;
    g_newkey_save = save;
    ulk();
    memset(k, 0, sizeof(k));
    return 0;
}

int xc_beta_save_key(const char *key) { return new_key(key, 1); }
void xlu_use_key(const char *key) { (void)new_key(key, 0); }

void xlu_set_console_id(const char *id) {
    int ok = id && strlen(id) == 32;
    for (int i = 0; ok && i < 32; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) ok = 0;
    lk();
    if (ok) memcpy(g_id, id, 33); else g_id[0] = 0;
    ulk();
}

static void set_beta(int b) {
    lk();
    g_beta = b;
    trim_to(cap_now());
    ulk();
}

/* ------------------------------------------------------------ envio */

/* JSON string: escapa; UTF-8 inválido vira '?'. Retorna bytes escritos. */
static size_t jstr(char *o, size_t cap, const char *s) {
    size_t w = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (cap < 3) return 0;
    o[w++] = '"';
    while (*p && w + 8 < cap) {
        unsigned c = *p;
        if (c == '"' || c == '\\') { o[w++] = '\\'; o[w++] = (char)c; p++; continue; }
        if (c < 32) { w += (size_t)snprintf(o + w, cap - w, "\\u%04x", c); p++; continue; }
        if (c < 0x80) { o[w++] = (char)c; p++; continue; }
        int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        int ok = n > 0 && c >= 0xC2;
        for (int k = 1; ok && k < n; k++) if ((p[k] & 0xC0) != 0x80) ok = 0;
        if (!ok) { o[w++] = '?'; p++; continue; }
        for (int k = 0; k < n; k++) o[w++] = (char)p[k];
        p += n;
    }
    o[w++] = '"';
    return w;
}

static int https_post(const char *host, const char *path, const char *headers, const char *body, size_t len,
                      int *status) {
    if (!g_conn) g_conn = https_conn_new(host);
    if (!g_conn) return -1;
    https_resp_t r;
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    int rc = https_conn_request(g_conn, "logs", "POST", path, headers, body, len, 16 * 1024, &r, &e);
    if (rc != 0) {
        printf("[logs] %s: %s\n", host, e.msg);   /* só UART: não realimenta o log */
        https_conn_free(g_conn);
        g_conn = NULL;
        return -1;
    }
    *status = r.status;
    https_free(&r);
    return 0;
}

/* lock: tira do começo do buffer tudo até a posição lógica `end` */
static void drop_until(unsigned long long end) {
    if (end <= g_base) return;
    size_t cut = (size_t)(end - g_base);
    if (cut > g_len) cut = g_len;
    memmove(g_buf, g_buf + cut, g_len - cut);
    g_len -= cut;
    g_base += cut;
}

static void backoff(unsigned long long now) {
    g_backoff_ms = g_backoff_ms ? g_backoff_ms * 2 : 30000;
    if (g_backoff_ms > 600000) g_backoff_ms = 600000;
    g_backoff_until = now + g_backoff_ms;
}

/* worker: um lote. 1 = tentou. */
static int send_batch(unsigned long long now) {
    char *body = (char *)malloc(g_batch_max + 1024);
    if (!body) return 0;
    char game[96], id[33];
    lk();
    memcpy(game, g_game[0] ? g_game : g_batch_game, sizeof(game));
    memcpy(id, g_id, sizeof(id));
    ulk();
    size_t w = (size_t)snprintf(body, g_batch_max, "{\"console\":\"%s\",\"app\":", id);
    w += jstr(body + w, 64, g_app);
    w += (size_t)snprintf(body + w, 16, ",\"game\":");
    w += jstr(body + w, 256, game);
    w += (size_t)snprintf(body + w, 16, ",\"lines\":[");
    /* linhas: cópia sob o lock (o buffer pode andar enquanto envia) */
    unsigned long long end;
    unsigned nl = 0;
    lk();
    size_t off = 0;
    while (off < g_len) {
        const char *l = g_buf + off;
        size_t ll = strlen(l);
        if (w + ll * 6 + 8 > g_batch_max) break;   /* pior caso do escape */
        if (nl) body[w++] = ',';
        w += jstr(body + w, ll * 6 + 4, l);
        off += ll + 1;
        nl++;
    }
    end = g_base + off;
    ulk();
    if (!nl) { free(body); return 0; }
    w += (size_t)snprintf(body + w, 4, "]}");
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "Content-Type: application/json\r\nX-Api-Key: %s\r\nX-Console: %s\r\n", g_key, id);
    int status = 0;
    int rc = (g_post ? g_post : https_post)(XLU_HOST, XLU_PATH, hdr, body, w, &status);
    free(body);
    memset(hdr, 0, sizeof(hdr));
    g_last_send = now;
    lk();
    g_st.last_status = rc == 0 ? status : rc;
    ulk();
    if (rc == 0 && status >= 200 && status < 300) {
        lk();
        drop_until(end);
        if (!g_game[0]) g_batch_game[0] = 0;   /* lote do jogo entregue */
        g_st.batches++;
        g_st.lines_sent += nl;
        ulk();
        g_backoff_ms = 0;
        g_backoff_until = 0;
        return 1;
    }
    lk();
    g_st.errors++;
    ulk();
    if (rc == 0 && (status == 401 || status == 403)) {
        xc_log("[logs] chave beta recusada (HTTP %d): envio parado ate o proximo boot", status);
        set_beta(XC_BETA_INVALID);
    } else if (rc == 0 && status == 413 && g_batch_max > 8 * 1024) {
        g_batch_max /= 2;   /* lote menor já no próximo passo */
        lk();
        g_st.rejected_413++;
        ulk();
    } else if (rc == 0 && status >= 400 && status < 500 && status != 429) {
        /* corpo recusado (400, ou 413 mesmo com 8 KB): não insiste no mesmo
         * lote, que some do buffer */
        lk();
        drop_until(end);
        if (status == 413) g_st.rejected_413++;
        g_st.lines_dropped += nl;
        ulk();
        backoff(now);
    } else {
        backoff(now);   /* 429, 5xx, rede */
    }
    return 1;
}

int xlu_step(void) {
    if (!g_inited) return 0;
    unsigned long long now = g_clock ? g_clock() : xcn_now_ms();
    int beta = xc_beta_state();
    lk();
    int pend = g_newkey_on;
    ulk();
    if (beta == XC_BETA_UNKNOWN && !pend) {
        char k[160];
        int l = xc_store_load(XLU_KEY_FILE, k, sizeof(k) - 1);
        if (l > 0) k[l] = 0;
        if (l > 0 && key_norm(g_key, sizeof(g_key), k) == 0) {
            xlu_add_secret(g_key);
            set_beta(XC_BETA_ACTIVE);
            xc_log("[logs] chave beta encontrada: envio ligado");
        } else {
            set_beta(XC_BETA_NONE);
            printf("[logs] sem %s valido: nada e enviado\n", XLU_KEY_FILE);
        }
        memset(k, 0, sizeof(k));
        return 1;
    }
    {
        char nk[72];
        int on, save;
        lk();
        on = g_newkey_on;
        save = g_newkey_save;
        memcpy(nk, g_newkey, sizeof(nk));
        g_newkey_on = 0;
        ulk();
        if (on) {
            if (save && xc_store_save(XLU_KEY_FILE, nk, strlen(nk)) != 0) xc_log("[logs] nao gravei %s", XLU_KEY_FILE);
            memcpy(g_key, nk, sizeof(nk));
            xlu_add_secret(g_key);
            g_backoff_ms = 0;
            g_backoff_until = 0;
            set_beta(XC_BETA_ACTIVE);
            xc_log(save ? "[logs] chave beta nova gravada: envio ligado" : "[logs] chave beta validada: envio ligado");
            memset(nk, 0, sizeof(nk));
            return 1;
        }
    }
    if (beta != XC_BETA_ACTIVE) return 0;
    lk();
    int have_id = g_id[0] != 0;
    ulk();
    if (!have_id) return 0;   /* v0.20.3: sem ID de hardware nada sai */
    if (now < g_backoff_until) return 0;
    if (g_last_send && now - g_last_send < XLU_MIN_GAP_MS) return 0;
    lk();
    size_t len = g_len;
    ulk();
    if (!len) { g_flush = 0; return 0; }
    int due = g_flush || len >= XLU_BUF_BYTES * 3 / 4 || !g_last_send || now - g_last_send >= XLU_PERIOD_MS;
    if (!due) return 0;
    g_flush = 0;
    return send_batch(now);
}

void xlu_finish(void) {
    if (g_conn) { https_conn_free(g_conn); g_conn = NULL; }
}
