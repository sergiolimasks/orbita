/* XCloud360 / orbita v0.20.3 — chave beta presa ao console. Ver xc_beta.h e
 * BETA-GATE.md. Portável (Xbox e host): a plataforma vem em xcb_ops_t. */
#include "xc_beta.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

static const char ID_SALT[]   = "orbita-console-id-v1";
static const char HMAC_SALT[] = "orbita-beta-ok-hmac-v1";

/* ------------------------------------------------------------ identidade */

static void put_hw(mbedtls_sha256_context *c, const uint64_t fuse[XCB_NFUSE], const uint8_t mac[6]) {
    for (int i = 0; i < XCB_NFUSE; i++) {
        uint8_t b[8];
        for (int k = 0; k < 8; k++) b[k] = (uint8_t)(fuse[i] >> (56 - 8 * k));   /* big-endian fixo */
        mbedtls_sha256_update(c, b, 8);
    }
    mbedtls_sha256_update(c, mac, 6);
}

void xcb_hwid(const uint64_t fuse[XCB_NFUSE], const uint8_t mac[6], char id[33], uint8_t hkey[32]) {
    uint8_t h[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const unsigned char *)ID_SALT, sizeof(ID_SALT) - 1);
    put_hw(&c, fuse, mac);
    mbedtls_sha256_finish(&c, h);
    for (int i = 0; i < 16; i++) snprintf(id + 2 * i, 3, "%02x", h[i]);
    id[32] = 0;
    /* chave do HMAC: outro sal, então o ID que aparece na tela não assina nada */
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const unsigned char *)HMAC_SALT, sizeof(HMAC_SALT) - 1);
    put_hw(&c, fuse, mac);
    mbedtls_sha256_finish(&c, hkey);
    mbedtls_sha256_free(&c);
    memset(h, 0, sizeof(h));
}

void xcb_id_short(const char *id, char out[16]) {
    int o = 0;
    for (int i = 0; i < 12 && id && id[i]; i++) {
        if (i && i % 4 == 0) out[o++] = '-';
        char ch = id[i];
        out[o++] = (ch >= 'a' && ch <= 'f') ? (char)(ch - 32) : ch;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ chave */

int xcb_key_norm(const char *in, char *out, size_t cap) {
    if (!in) return -1;
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') in++;
    size_t n = 0;
    while (in[n] && in[n] != '\r' && in[n] != '\n') n++;   /* só a 1a linha */
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t')) n--;
    if (n < 8 || n > 64 || n >= cap) return -1;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)in[i] <= 32 || (unsigned char)in[i] >= 127) return -1;
    memcpy(out, in, n);
    out[n] = 0;
    return 0;
}

void xcb_key_canon(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; in && *in && o + 1 < cap; in++) {
        char ch = *in;
        if (ch == '-' || ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        out[o++] = ch;
    }
    if (cap) out[o] = 0;
}

void xcb_key_mask(const char *key, char *out, size_t cap) {
    size_t n = key ? strlen(key) : 0, o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        size_t after = 0;
        for (size_t j = i + 1; j < n; j++) if (key[j] != '-') after++;
        char ch = key[i];
        if (ch == '-') out[o++] = '-';
        else if (after < 3) out[o++] = ch;
        else out[o++] = '*';
    }
    if (cap) out[o] = 0;
}

int xcb_classify(int rc, int status, const char *body) {
    if (rc != 0) return XCB_R_NOCONN;
    if (status >= 200 && status < 300) return XCB_R_OK;
    if (status == 401) return XCB_R_INVALID;
    if (status == 403) return (body && strstr(body, "duplicate")) ? XCB_R_DUPLICATE : XCB_R_REVOKED;
    if (status == 429 || status >= 500) return XCB_R_NOCONN;   /* servidor fora/ocupado */
    return XCB_R_ERROR;
}

/* --------------------------------------------- última validação assinada */

static int ok_mac(const uint8_t hkey[32], const char *id, const char *key, long long ts, char hex[65]) {
    char canon[72], msg[200];
    uint8_t mac[32];
    xcb_key_canon(key, canon, sizeof(canon));
    int n = snprintf(msg, sizeof(msg), "xcbok1|%s|%s|%lld", id, canon, ts);
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (n <= 0 || n >= (int)sizeof(msg) || !md) return -1;
    if (mbedtls_md_hmac(md, hkey, 32, (const unsigned char *)msg, (size_t)n, mac) != 0) return -1;
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    memset(canon, 0, sizeof(canon));
    return 0;
}

int xcb_okfile_make(const uint8_t hkey[32], const char *id, const char *key, long long ts, char *out, size_t cap) {
    char hex[65];
    if (ts < 0 || ok_mac(hkey, id, key, ts, hex) != 0) return -1;
    int n = snprintf(out, cap, "XCBOK1 %lld %s\n", ts, hex);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

int xcb_okfile_check(const uint8_t hkey[32], const char *id, const char *key, const char *buf, size_t n,
                     long long *ts) {
    char b[160], got[80], want[65];
    long long t = -1;
    if (!buf || n == 0 || n >= sizeof(b)) return -1;
    memcpy(b, buf, n);
    b[n] = 0;
    if (sscanf(b, "XCBOK1 %lld %70s", &t, got) != 2 || t <= 0 || strlen(got) != 64) return -1;
    if (ok_mac(hkey, id, key, t, want) != 0) return -1;
    unsigned d = 0;   /* comparação em tempo constante */
    for (int i = 0; i < 64; i++) d |= (unsigned)(got[i] ^ want[i]);
    if (d) return -1;
    if (ts) *ts = t;
    return 0;
}

int xcb_grace_ok(long long last, long long now) {
    if (last <= 0 || now < 0) return 0;
    if (now + XCB_SKEW_S < last) return 0;     /* relógio voltou: não confia */
    return now - last <= XCB_GRACE_S;
}

/* ------------------------------------------------------------ estado */

static xcb_ops_t g_ops;
static char      g_id[33], g_short[16];
static uint8_t   g_hkey[32];
static int       g_inited;
/* lock */
static xcb_snap_t g_st;
static volatile int g_running;
static char      g_key[72];     /* chave validada em uso */
static char      g_arg[72];     /* chave pra validar (submit) */
static int       g_op;
static int       g_spawned;     /* só o main */

static void lk(void)  { if (g_ops.lock) g_ops.lock(); }
static void ulk(void) { if (g_ops.unlock) g_ops.unlock(); }

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    if (!g_ops.log) return;
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_ops.log(b);
}

void xcb_init(const xcb_ops_t *ops, const char *id, const uint8_t hkey[32]) {
    memset(&g_ops, 0, sizeof(g_ops));
    if (ops) g_ops = *ops;
    snprintf(g_id, sizeof(g_id), "%s", id ? id : "");
    xcb_id_short(g_id, g_short);
    if (hkey) memcpy(g_hkey, hkey, 32); else memset(g_hkey, 0, 32);
    memset(&g_st, 0, sizeof(g_st));
    g_st.last_ok = -1;
    g_key[0] = g_arg[0] = 0;
    g_running = 0;
    g_inited = 1;
}

const char *xcb_console_id(void) { return g_id; }
const char *xcb_console_short(void) { return g_short; }

/* main: 1 até a hw 3 estar livre de verdade (como o xcc_bg_running) */
int xcb_running(void) {
    if (g_running) return 1;
    if (g_spawned) {
        if (g_ops.slot_busy && g_ops.slot_busy()) return 1;
        g_spawned = 0;
    }
    return 0;
}

int xcb_authorized(void) {
    lk();
    int a = g_st.authorized;
    ulk();
    return a;
}

void xcb_snapshot(xcb_snap_t *s) {
    lk();
    *s = g_st;
    s->busy = g_running;
    ulk();
}

int xcb_key(char *out, size_t cap) {
    if (!out || !cap) return 0;
    lk();
    snprintf(out, cap, "%s", g_key);
    ulk();
    return (int)strlen(out);
}

static void set_phase(int ph, int http, const char *msg) {
    lk();
    g_st.phase = ph;
    g_st.http = http;
    snprintf(g_st.msg, sizeof(g_st.msg), "%s", msg ? msg : "");
    g_st.seq++;
    ulk();
}

static long long now_s(void) { return g_ops.now ? g_ops.now() : -1; }

/* worker: GET /v1/beta/check. XCB_R_*. */
static int check(const char *key, int *http) {
    char hdr[200], body[512];
    int status = 0;
    body[0] = 0;
    snprintf(hdr, sizeof(hdr), "X-Api-Key: %s\r\nX-Console: %s\r\n", key, g_id);
    int rc = g_ops.get ? g_ops.get(XCB_HOST, XCB_PATH, hdr, &status, body, sizeof(body)) : -1;
    memset(hdr, 0, sizeof(hdr));
    *http = rc == 0 ? status : 0;
    int r = xcb_classify(rc, status, body);
    logf_("[beta] check: %s (HTTP %d)", r == XCB_R_OK ? "ok" : r == XCB_R_INVALID ? "invalida"
                                      : r == XCB_R_REVOKED ? "revogada" : r == XCB_R_DUPLICATE ? "duplicada"
                                      : r == XCB_R_NOCONN ? "sem conexao" : "erro", *http);
    return r;
}

volatile int xcb_key_unsaved = 0;   /* v0.21.37: 1 = a chave digitada não ficou no pendrive */
static void accept_key(const char *key, int save_key, int http) {
    long long t = now_s();
    if (save_key && g_ops.save) {
        if (g_ops.save(XCB_KEY_FILE, key, strlen(key)) != 0) {
            logf_("[beta] nao gravei %s", XCB_KEY_FILE);
            xcb_key_unsaved = 1;
        } else {
            xcb_key_unsaved = 0;
        }
    }
    if (t > 0 && g_ops.save) {
        char f[128];
        int n = xcb_okfile_make(g_hkey, g_id, key, t, f, sizeof(f));
        if (n < 0 || g_ops.save(XCB_OK_FILE, f, (size_t)n) != 0) logf_("[beta] nao gravei %s", XCB_OK_FILE);
    }
    lk();
    snprintf(g_key, sizeof(g_key), "%s", key);
    xcb_key_mask(key, g_st.key_masked, sizeof(g_st.key_masked));
    g_st.authorized = 1;
    if (t > 0) g_st.last_ok = t;
    ulk();
    set_phase(XCB_OK, http, "Chave válida");
}

/* sem conexão: vale a última validação OK assinada (<= 7 dias)? */
static int try_grace(const char *key) {
    char b[160];
    long long ts = -1;
    int n = g_ops.load ? g_ops.load(XCB_OK_FILE, b, sizeof(b) - 1) : -1;
    if (n <= 0 || xcb_okfile_check(g_hkey, g_id, key, b, (size_t)n, &ts) != 0) {
        if (n > 0) logf_("[beta] %s nao confere (outro console, outra chave ou editado)", XCB_OK_FILE);
        return 0;
    }
    long long now = now_s();
    if (!xcb_grace_ok(ts, now)) {
        logf_("[beta] ultima validacao velha demais (%lld s) ou hora desconhecida", now >= 0 ? now - ts : -1LL);
        return 0;
    }
    lk();
    snprintf(g_key, sizeof(g_key), "%s", key);
    xcb_key_mask(key, g_st.key_masked, sizeof(g_st.key_masked));
    g_st.authorized = 1;
    g_st.last_ok = ts;
    ulk();
    return 1;
}

static void result_phase(int r, int http) {
    switch (r) {
    case XCB_R_INVALID:   set_phase(XCB_INVALID, http, "Chave inválida"); break;
    case XCB_R_REVOKED:   set_phase(XCB_REVOKED, http, "Chave revogada: digite a chave atual do site."); break;
    case XCB_R_DUPLICATE: set_phase(XCB_DUPLICATE, http,
                                    "Esta chave é exclusiva de um console e foi bloqueada por uso em outro aparelho. "
                                    "Fale com o administrador."); break;
    case XCB_R_NOCONN:    set_phase(XCB_NOCONN, http, "Não foi possível validar sua chave (sem conexão)"); break;
    default: {
        char m[96];
        snprintf(m, sizeof(m), "Não foi possível validar sua chave (servidor respondeu %d)", http);
        set_phase(XCB_ERROR, http, m);
    }
    }
}

void xcb_run(int op, const char *arg) {
    char key[72], raw[160];
    int http = 0;
    key[0] = 0;
    set_phase(XCB_CHECKING, 0, "Validando sua chave beta…");
    if (op == XCB_OP_BOOT || op == XCB_OP_FILE) {
        int n = g_ops.load ? g_ops.load(XCB_KEY_FILE, raw, sizeof(raw) - 1) : -1;
        if (n > 0) raw[n] = 0;
        if (n <= 0 || xcb_key_norm(raw, key, sizeof(key)) != 0) {
            memset(raw, 0, sizeof(raw));
            set_phase(XCB_NEED_KEY, 0, op == XCB_OP_FILE ? "Não achei XC360/BETAKEY.TXT válido no pendrive" : "");
            return;
        }
        memset(raw, 0, sizeof(raw));
    } else {
        if (xcb_key_norm(arg, key, sizeof(key)) != 0) { set_phase(XCB_INVALID, 0, "Chave inválida"); return; }
    }
    int r = check(key, &http);
    if (r == XCB_R_OK) {
        accept_key(key, op != XCB_OP_BOOT, http);
    } else if (r == XCB_R_NOCONN && op == XCB_OP_BOOT && try_grace(key)) {
        set_phase(XCB_OFFLINE_OK, http, "Sem conexão: usando a última validação (até 7 dias)");
        logf_("[beta] sem conexao: entrou pela ultima validacao");
    } else {
        result_phase(r, http);
    }
    memset(key, 0, sizeof(key));
}

static void worker(void) {
    char a[72];
    int op;
    lk();
    op = g_op;
    memcpy(a, g_arg, sizeof(a));
    memset(g_arg, 0, sizeof(g_arg));
    ulk();
    xcb_run(op, a);
    memset(a, 0, sizeof(a));
    lk();
    g_running = 0;
    g_st.seq++;
    ulk();
}

static int start(int op, const char *key) {
    if (!g_inited || xcb_running()) return -1;
    lk();
    g_op = op;
    snprintf(g_arg, sizeof(g_arg), "%s", key ? key : "");
    g_st.op = op;
    g_running = 1;
    g_st.seq++;
    ulk();
    if (!g_ops.spawn) { worker(); return 0; }
    if (g_ops.spawn(worker) != 0) {
        lk();
        g_running = 0;
        memset(g_arg, 0, sizeof(g_arg));
        ulk();
        return -1;
    }
    g_spawned = 1;
    return 0;
}

int xcb_boot_start(void) { return start(XCB_OP_BOOT, NULL); }
int xcb_submit_start(const char *key) { return start(XCB_OP_SUBMIT, key); }
int xcb_file_start(void) { return start(XCB_OP_FILE, NULL); }
