/* XCloud360 — ver xc_update.h e app/h264play/UPDATE.md. */
#include "xc_update.h"
#include "xcu_port.h"
#include "xcu_pubkey.h"
#include "../login/xc_port.h"
#include "../login/cjson/cJSON.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

/* =============================================================== semver */

typedef struct {
    unsigned long n[3];
    const char   *pre;      /* NULL = release */
    size_t        pre_len;
} sv_t;

static int sv_parse(const char *v, sv_t *o) {
    if (!v) return -1;
    const char *p = v;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*p)) return -1;
        if (*p == '0' && isdigit((unsigned char)p[1])) return -1;   /* sem zero à esquerda */
        unsigned long x = 0;
        int d = 0;
        while (isdigit((unsigned char)*p)) {
            x = x * 10 + (unsigned long)(*p++ - '0');
            if (++d > 6) return -1;
        }
        o->n[i] = x;
        if (i < 2 && *p++ != '.') return -1;
    }
    o->pre = NULL;
    o->pre_len = 0;
    if (*p == '-') {
        p++;
        o->pre = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '.' || *p == '-')) p++;
        o->pre_len = (size_t)(p - o->pre);
        if (!o->pre_len || o->pre[0] == '.' || o->pre[o->pre_len - 1] == '.' || strstr(o->pre, ".."))
            return -1;
    }
    if (*p == '+') {   /* build metadata: ignorado na ordem */
        p++;
        while (*p && (isalnum((unsigned char)*p) || *p == '.' || *p == '-')) p++;
    }
    return *p ? -1 : 0;
}

int xcu_semver_valid(const char *v) { sv_t s; return sv_parse(v, &s) == 0; }

/* compara um identificador de pre-release (até o próximo '.') */
static int pre_id_cmp(const char *a, size_t al, const char *b, size_t bl) {
    int an = 1, bn = 1;
    for (size_t i = 0; i < al; i++) if (!isdigit((unsigned char)a[i])) an = 0;
    for (size_t i = 0; i < bl; i++) if (!isdigit((unsigned char)b[i])) bn = 0;
    if (an && bn) {
        if (al != bl) return al < bl ? -1 : 1;   /* sem zero à esquerda: mais dígitos = maior */
        int c = memcmp(a, b, al);
        return c < 0 ? -1 : c > 0;
    }
    if (an != bn) return an ? -1 : 1;            /* numérico < alfanumérico */
    size_t m = al < bl ? al : bl;
    int c = memcmp(a, b, m);
    if (c) return c < 0 ? -1 : 1;
    return al == bl ? 0 : (al < bl ? -1 : 1);
}

int xcu_semver_cmp(const char *a, const char *b) {
    sv_t x, y;
    int vx = sv_parse(a, &x) == 0, vy = sv_parse(b, &y) == 0;
    if (!vx || !vy) return vx - vy;
    for (int i = 0; i < 3; i++)
        if (x.n[i] != y.n[i]) return x.n[i] < y.n[i] ? -1 : 1;
    if (!x.pre || !y.pre) return (x.pre ? -1 : 0) + (y.pre ? 1 : 0);
    const char *p = x.pre, *q = y.pre, *pe = x.pre + x.pre_len, *qe = y.pre + y.pre_len;
    while (p < pe && q < qe) {
        const char *pd = memchr(p, '.', (size_t)(pe - p)), *qd = memchr(q, '.', (size_t)(qe - q));
        if (!pd) pd = pe;
        if (!qd) qd = qe;
        int c = pre_id_cmp(p, (size_t)(pd - p), q, (size_t)(qd - q));
        if (c) return c;
        p = pd < pe ? pd + 1 : pe;
        q = qd < qe ? qd + 1 : qe;
    }
    return (p < pe) - (q < qe);   /* mais identificadores = maior */
}

/* ============================================================ manifesto */

static int copy_str(cJSON *o, const char *k, char *dst, size_t cap) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(it) || !it->valuestring) return -1;
    size_t n = strlen(it->valuestring);
    if (n >= cap) return -1;
    memcpy(dst, it->valuestring, n + 1);
    return 0;
}

static void seterr(char *e, size_t cap, const char *m) { if (e && cap) snprintf(e, cap, "%s", m); }

int xcu_manifest_parse(const char *json, size_t len, xcu_manifest_t *m, char *err, size_t errcap) {
    memset(m, 0, sizeof(*m));
    cJSON *o = cJSON_ParseWithLength(json, len);
    if (!o || !cJSON_IsObject(o)) { cJSON_Delete(o); seterr(err, errcap, "manifesto nao e JSON"); return -1; }
    int rc = -2;
    char sig64[160];
    cJSON *f = cJSON_GetObjectItemCaseSensitive(o, "format");
    cJSON *sz = cJSON_GetObjectItemCaseSensitive(o, "size");
    if (!cJSON_IsNumber(f) || f->valuedouble != 1) { seterr(err, errcap, "formato de manifesto desconhecido"); goto out; }
    m->format = 1;
    if (copy_str(o, "channel", m->channel, sizeof(m->channel)) ||
        (strcmp(m->channel, "stable") && strcmp(m->channel, "beta"))) { seterr(err, errcap, "canal invalido"); goto out; }
    if (copy_str(o, "version", m->version, sizeof(m->version)) || !xcu_semver_valid(m->version)) {
        seterr(err, errcap, "versao invalida no manifesto"); goto out;
    }
    if (!cJSON_IsNumber(sz) || sz->valuedouble < XCU_MIN_SIZE || sz->valuedouble > XCU_MAX_SIZE ||
        sz->valuedouble != (double)(uint32_t)sz->valuedouble) { seterr(err, errcap, "tamanho invalido"); goto out; }
    m->size = (uint32_t)sz->valuedouble;
    if (copy_str(o, "sha256", m->sha256, sizeof(m->sha256)) || strlen(m->sha256) != 64) {
        seterr(err, errcap, "sha256 invalido"); goto out;
    }
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)m->sha256[i]) || isupper((unsigned char)m->sha256[i])) {
            seterr(err, errcap, "sha256 invalido"); goto out;
        }
    if (copy_str(o, "url", m->url, sizeof(m->url))) { seterr(err, errcap, "url invalida"); goto out; }
    {   /* só o caminho que a ponte serve pra ESTA versão */
        char want[96];
        snprintf(want, sizeof(want), "/v1/update/%s/xenon.elf", m->version);
        size_t wl = strlen(want);
        if (strncmp(m->url, want, wl) || (m->url[wl] && strcmp(m->url + wl, "?env=test"))) {
            seterr(err, errcap, "url invalida"); goto out;
        }
    }
    if (copy_str(o, "notes", m->notes, sizeof(m->notes))) { seterr(err, errcap, "notas invalidas"); goto out; }
    if (copy_str(o, "sig", sig64, sizeof(sig64)) ||
        mbedtls_base64_decode(m->sig, sizeof(m->sig), &m->sig_len, (const unsigned char *)sig64, strlen(sig64)) ||
        m->sig_len < 8) { seterr(err, errcap, "assinatura ausente"); goto out; }
    {   /* v0.20.3: obrigatória? (conferido em xcu_manifest_mandatory) */
        cJSON *md = cJSON_GetObjectItemCaseSensitive(o, "mandatory");
        if (cJSON_IsBool(md)) {
            m->has_mandatory = 1;
            m->mandatory_val = cJSON_IsTrue(md) ? 1 : 0;
        }
        if (copy_str(o, "sig2", sig64, sizeof(sig64)) == 0 &&
            mbedtls_base64_decode(m->sig2, sizeof(m->sig2), &m->sig2_len, (const unsigned char *)sig64,
                                  strlen(sig64)) != 0)
            m->sig2_len = 0;
    }
    rc = 0;
out:
    cJSON_Delete(o);
    return rc;
}

int xcu_manifest_message(const xcu_manifest_t *m, char *out, size_t cap) {
    int n = snprintf(out, cap, "xc360-update-v1\nchannel=%s\nversion=%s\nsize=%u\nsha256=%s\nurl=%s\nnotes=%s",
                     m->channel, m->version, (unsigned)m->size, m->sha256, m->url, m->notes);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int xcu_manifest_message2(const xcu_manifest_t *m, char *out, size_t cap) {
    int n = xcu_manifest_message(m, out, cap);
    if (n < 0) return -1;
    int k = snprintf(out + n, cap - (size_t)n, "\nmandatory=%d", m->mandatory_val ? 1 : 0);
    return (k < 0 || (size_t)(n + k) >= cap) ? -1 : n + k;
}

static int verify_bytes(const char *msg, int n, const uint8_t *sig, size_t sig_len, const uint8_t *pub,
                        size_t pub_len) {
    unsigned char h[32];
    if (n < 0 || sig_len < 8) return -1;
    if (mbedtls_sha256((const unsigned char *)msg, (size_t)n, h, 0)) return -1;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int r = mbedtls_pk_parse_public_key(&pk, pub, pub_len);
    if (r == 0 && !mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECKEY)) r = -1;
    if (r == 0) r = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, h, sizeof(h), sig, sig_len);
    mbedtls_pk_free(&pk);
    return r == 0 ? 0 : -1;
}

int xcu_manifest_verify(const xcu_manifest_t *m, const uint8_t *pub, size_t pub_len) {
    char msg[1024];
    int n = xcu_manifest_message(m, msg, sizeof(msg));
    return verify_bytes(msg, n, m->sig, m->sig_len, pub, pub_len);
}

int xcu_manifest_mandatory(const xcu_manifest_t *m, const uint8_t *pub, size_t pub_len) {
    if (!m->has_mandatory || !m->sig2_len) return 1;
    char msg[1100];
    int n = xcu_manifest_message2(m, msg, sizeof(msg));
    if (verify_bytes(msg, n, m->sig2, m->sig2_len, pub, pub_len) != 0) return 1;   /* adulterado: obrigatória */
    return m->mandatory_val ? 1 : 0;
}

/* ================================================================ estado */

static xcu_snap_t     g_st;
static xcu_manifest_t g_man;
static int            g_man_ok = 0;
static volatile int   g_cancel = 0, g_running = 0;
static xcu_config_t   g_cfg;
static int            g_inited = 0;

static void st_set(int phase, const char *status) {
    xcup_lock();
    if (phase >= 0) g_st.phase = phase;
    if (status) snprintf(g_st.status, sizeof(g_st.status), "%s", status);
    g_st.cancellable = g_running && (g_st.phase == XCU_CHECKING || g_st.phase == XCU_DOWNLOADING ||
                                     g_st.phase == XCU_VERIFYING);
    g_st.seq++;
    xcup_unlock();
}

static void st_progress(uint32_t done, uint32_t total) {
    xcup_lock();
    g_st.done = done;
    g_st.total = total;
    g_st.pct = total ? (int)((uint64_t)done * 100 / total) : 0;
    g_st.seq++;
    xcup_unlock();
}

static void st_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void st_fail(const char *fmt, ...) {
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    xcup_lock();
    snprintf(g_st.err, sizeof(g_st.err), "%s", b);
    xcup_unlock();
    st_set(XCU_FAILED, "Falha na atualizacao");
    xc_log("[update] FALHOU: %s", b);
}

void xcu_init(const char *current_version, const xcu_config_t *cfg) {
    memset(&g_st, 0, sizeof(g_st));
    snprintf(g_st.current, sizeof(g_st.current), "%s", current_version ? current_version : "0.0.0");
    {   /* Sufixo de VARIANTE do Makefile ("0.20.0-full", "-trace") não é
         * pre-release: sem isto 0.20.0-full < 0.20.0 e o menu ofereceria a
         * mesma versão. Pre-releases de verdade começam com alpha/beta/rc/test. */
        char *d = strchr(g_st.current, '-');
        if (d && strncmp(d + 1, "alpha", 5) && strncmp(d + 1, "beta", 4) && strncmp(d + 1, "rc", 2) &&
            strncmp(d + 1, "test", 4))
            *d = 0;
    }
    memset(&g_cfg, 0, sizeof(g_cfg));
    if (cfg) g_cfg = *cfg;
    if (!g_cfg.host) g_cfg.host = XCU_HOST;
    if (!g_cfg.pubkey_der) { g_cfg.pubkey_der = xcu_pubkey_der; g_cfg.pubkey_len = sizeof(xcu_pubkey_der); }
    g_man_ok = 0;
    g_inited = 1;
    st_set(XCU_IDLE, "");
}

void xcu_cancel(void) { g_cancel = 1; xcup_sync(); }
int  xcu_running(void) { return g_running; }

void xcu_snapshot(xcu_snap_t *s) {
    xcup_lock();
    *s = g_st;
    s->busy = g_running;
    xcup_unlock();
}

static int cancel_hook(void) { return g_cancel; }
static int cancelled(void) { return g_cancel; }

/* v0.20.3: ID de hardware do console (xcb_hwid), mandado como X-Console
 * junto com a chave. */
static char g_console[40];
void xcu_set_console_id(const char *id) { snprintf(g_console, sizeof(g_console), "%s", id ? id : ""); }

/* "X-Api-Key: <chave>\r\nX-Console: <id>\r\n" se houver XC360/BETAKEY.TXT
 * válido (sem ID, só a chave). */
static void beta_header(char *out, size_t cap) {
    char k[80];
    out[0] = 0;
    int n = xc_store_load("BETAKEY.TXT", k, sizeof(k) - 1);
    if (n <= 0) return;
    k[n] = 0;
    char *s = k, *e;
    while (*s && isspace((unsigned char)*s)) s++;
    for (e = s; *e && !isspace((unsigned char)*e); e++)
        if (!isalnum((unsigned char)*e) && *e != '-') return;
    *e = 0;
    if (strlen(s) < 8 || strlen(s) > 64) return;
    if (g_console[0]) snprintf(out, cap, "X-Api-Key: %s\r\nX-Console: %s\r\n", s, g_console);
    else snprintf(out, cap, "X-Api-Key: %s\r\n", s);
    memset(k, 0, sizeof(k));
}

/* ================================================================= check */

/* Sobras de uma troca interrompida (o app pode ter subido pela rede/TFTP):
 * sem xenon.elf mas com backup/novo verificado -> põe de volta. */
static void recover_elf(void) {
    if (xcup_fsize(XCU_ELF) > 0) return;
    if (xcup_fsize(XCU_OLD) > 0 && xcup_rename(XCU_OLD, XCU_ELF) == 0) {
        xc_log("[update] xenon.elf faltando: restaurado do XENON.OLD");
        return;
    }
    xc_log("[update] AVISO: pendrive sem xenon.elf e sem backup");
}

static void do_check(void) {
    char hdr[200], path[64], err[96];
    int status = 0;
    char *body = NULL;
    size_t len = 0;
    st_set(XCU_CHECKING, "Procurando atualizacao...");
    if (xcup_prepare() == 0) recover_elf();
    beta_header(hdr, sizeof(hdr));
    snprintf(path, sizeof(path), "/v1/update/latest%s", g_cfg.test_env ? "?env=test" : "");
    if (xcup_get(g_cfg.host, path, hdr[0] ? hdr : NULL, 16 * 1024, &status, &body, &len, err, sizeof(err)) != 0) {
        if (cancelled()) { st_set(XCU_CANCELLED, "Cancelado"); return; }
        st_fail("sem conexao com o servidor (%s)", err);
        return;
    }
    if (status == 404) {
        free(body);
        st_set(XCU_UP_TO_DATE, "Nenhuma versao publicada");
        return;
    }
    if (status != 200) {
        free(body);
        st_fail(status == 429 ? "servidor ocupado, tente daqui a 1 minuto" : "servidor respondeu %d", status);
        return;
    }
    xcu_manifest_t m;
    int pr = xcu_manifest_parse(body, len, &m, err, sizeof(err));
    free(body);
    if (pr != 0) { st_fail("%s", err); return; }
    if (xcu_manifest_verify(&m, g_cfg.pubkey_der, g_cfg.pubkey_len) != 0) {
        st_fail("assinatura do manifesto NAO confere (ignorado)");
        return;
    }
    int mand = xcu_manifest_mandatory(&m, g_cfg.pubkey_der, g_cfg.pubkey_len);   /* fora do lock (ECDSA) */
    xcup_lock();
    g_man = m;
    g_man_ok = 1;
    g_st.mandatory = mand;
    snprintf(g_st.latest, sizeof(g_st.latest), "%s", m.version);
    snprintf(g_st.channel, sizeof(g_st.channel), "%s", m.channel);
    snprintf(g_st.notes, sizeof(g_st.notes), "%s", m.notes);
    g_st.total = m.size;
    xcup_unlock();
    if (xcu_semver_cmp(m.version, g_st.current) > 0) {
        char s[96];
        snprintf(s, sizeof(s), "Nova versao v%s%s disponivel", m.version, strcmp(m.channel, "beta") ? "" : " (beta)");
        st_set(XCU_AVAILABLE, s);
        xc_log("[update] disponivel: v%s (%s, %u bytes); rodando v%s", m.version, m.channel,
               (unsigned)m.size, g_st.current);
    } else {
        st_set(XCU_UP_TO_DATE, "Voce ja esta na versao mais nova");
    }
}

/* =============================================================== install */

static void hex32(const unsigned char *h, char *o) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { o[2 * i] = d[h[i] >> 4]; o[2 * i + 1] = d[h[i] & 15]; }
    o[64] = 0;
}

/* sha256 do arquivo no pendrive; progresso opcional. 0 = bate com want. */
static int file_sha_ok(const char *path, uint32_t size, const char *want, int progress, int *io_err) {
    static unsigned char buf[64 * 1024];
    mbedtls_sha256_context c;
    unsigned char h[32];
    char hx[65];
    *io_err = 0;
    if (xcup_fsize(path) != (long long)size) return -1;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    uint32_t off = 0;
    while (off < size) {
        if (progress && cancelled()) { mbedtls_sha256_free(&c); return -1; }
        size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
        long r = xcup_read(path, off, buf, n);
        if (r != (long)n) { *io_err = 1; mbedtls_sha256_free(&c); return -1; }
        if (off == 0 && memcmp(buf, "\x7f" "ELF", 4)) { mbedtls_sha256_free(&c); return -1; }
        mbedtls_sha256_update(&c, buf, n);
        off += (uint32_t)n;
        if (progress) st_progress(off, size);
    }
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    hex32(h, hx);
    return strcmp(hx, want) ? -1 : 0;
}

/* Copia src -> dst (dst não pode existir). 0 = ok. Último recurso da troca. */
static int copy_file(const char *src, const char *dst, uint32_t size) {
    static unsigned char buf[32 * 1024];
    xcup_unlink(dst);
    for (uint32_t off = 0; off < size;) {
        size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
        if (xcup_read(src, off, buf, n) != (long)n) return -1;
        if (xcup_write_at(dst, off, buf, n) != 0) return -1;
        off += (uint32_t)n;
    }
    return 0;
}

static int download(const xcu_manifest_t *m, const char *bhdr) {
    static char inf_want[128], inf_have[128];
    snprintf(inf_want, sizeof(inf_want), "%s %u %s\n", m->sha256, (unsigned)m->size, m->version);
    long long have = xcup_fsize(XCU_NEW);
    int il = -1;
    {
        long r = xcup_read(XCU_INF, 0, inf_have, sizeof(inf_have) - 1);
        if (r > 0) { inf_have[r] = 0; il = (int)r; }
    }
    if (have > 0 && il > 0 && !strcmp(inf_have, inf_want) && have <= (long long)m->size) {
        xc_log("[update] retomando o download de %lld/%u bytes", have, (unsigned)m->size);
    } else {
        if (xcup_unlink(XCU_NEW) != 0) { st_fail("nao consegui apagar o XENON.NEW antigo"); return -1; }
        have = 0;
        if (xcup_write_file(XCU_INF, inf_want, strlen(inf_want)) != 0) { st_fail("erro gravando no pendrive"); return -1; }
    }
    {   /* espaço: o que falta + folga. Sem espaço, o backup velho sai primeiro. */
        long long need = (long long)m->size - have + 512 * 1024;
        long long fr = xcup_free_bytes();
        if (fr >= 0 && fr < need && xcup_fsize(XCU_OLD) > 0) {
            xc_log("[update] pouco espaco: apagando o backup antigo XENON.OLD");
            xcup_unlink(XCU_OLD);
            fr = xcup_free_bytes();
        }
        if (fr >= 0 && fr < need) {
            st_fail("pendrive sem espaco (precisa de %lld KB livres)", need / 1024);
            return -1;
        }
    }
    st_set(XCU_DOWNLOADING, "Baixando...");
    st_progress((uint32_t)have, m->size);
    char hdr[320], err[96];
    int fails = 0;
    uint32_t off = (uint32_t)have;
    while (off < m->size) {
        if (cancelled()) return 1;
        uint32_t n = m->size - off < XCU_CHUNK ? m->size - off : XCU_CHUNK;
        snprintf(hdr, sizeof(hdr), "Range: bytes=%u-%u\r\n%s", (unsigned)off, (unsigned)(off + n - 1), bhdr);
        int status = 0;
        char *body = NULL;
        size_t len = 0;
        int r = xcup_get(g_cfg.host, m->url, hdr, XCU_CHUNK + 1024, &status, &body, &len, err, sizeof(err));
        if (r == 0 && status == 206 && len == n) {
            int w = xcup_write_at(XCU_NEW, off, body, n);
            free(body);
            if (w != 0) { st_fail("erro gravando no pendrive (%u)", (unsigned)off); return -1; }
            off += n;
            fails = 0;
            st_progress(off, m->size);
            continue;
        }
        free(body);
        if (cancelled()) return 1;
        if (r == 0 && (status == 401 || status == 404 || status == 416)) {
            if (status == 416) xcup_unlink(XCU_NEW);   /* .NEW maior que devia: recomeça na próxima */
            st_fail(status == 401 ? "versao beta: chave beta invalida" : "arquivo nao encontrado no servidor (%d)", status);
            return -1;
        }
        if (++fails > 6) {
            if (r == 0) st_fail("download falhou: servidor respondeu %d", status);
            else st_fail("download falhou: %s", err);
            return -1;
        }
        xc_log("[update] pedaco %u falhou (%s %d), tentativa %d", (unsigned)off, r ? err : "http", status, fails);
        xcup_net_close();
        for (int t = 0; t < fails * 10 && !cancelled(); t++) xcn_sleep_ms(100);
    }
    return 0;
}

/* Troca com rollback. 0 = xenon.elf novo instalado e conferido. */
static int swap_in(const xcu_manifest_t *m) {
    int io = 0;
    int had_elf = xcup_fsize(XCU_ELF) > 0;
    if (had_elf) {
        if (xcup_unlink(XCU_OLD) != 0) { st_fail("nao consegui apagar o XENON.OLD"); return -1; }
        if (xcup_rename(XCU_ELF, XCU_OLD) != 0) {
            st_fail("nao consegui renomear o xenon.elf (nada mudou)");
            return -1;
        }
    }
    if (xcup_rename(XCU_NEW, XCU_ELF) == 0 && file_sha_ok(XCU_ELF, m->size, m->sha256, 0, &io) == 0)
        return 0;
    /* falhou: volta o anterior. Ordem: rename do backup, cópia do backup,
     * cópia do novo (já verificado) — o pendrive tem que sair com um ELF. */
    xc_log("[update] troca falhou, desfazendo");
    if (xcup_fsize(XCU_ELF) >= 0) {
        if (xcup_fsize(XCU_NEW) < 0) xcup_rename(XCU_ELF, XCU_NEW);
        else xcup_unlink(XCU_ELF);
    }
    if (had_elf) {
        if (xcup_fsize(XCU_ELF) < 0 && xcup_rename(XCU_OLD, XCU_ELF) == 0) {
            st_fail("troca falhou; versao anterior mantida");
            return -1;
        }
        long long os = xcup_fsize(XCU_OLD);
        if (os > 0 && copy_file(XCU_OLD, XCU_ELF, (uint32_t)os) == 0) {
            st_fail("troca falhou; versao anterior restaurada (copia)");
            return -1;
        }
    }
    if (xcup_fsize(XCU_NEW) == (long long)m->size && copy_file(XCU_NEW, XCU_ELF, m->size) == 0 &&
        file_sha_ok(XCU_ELF, m->size, m->sha256, 0, &io) == 0) {
        xc_log("[update] instalado por copia (renomear falhou)");
        xcup_unlink(XCU_NEW);
        return 0;
    }
    st_fail("ERRO GRAVE: pendrive sem xenon.elf valido. Copie XC360/XENON.OLD pra raiz como xenon.elf");
    return -1;
}

static void do_install(void) {
    xcu_manifest_t m;
    char bhdr[200];
    xcup_lock();
    m = g_man;
    int ok = g_man_ok;
    xcup_unlock();
    if (!ok) { st_fail("procure a atualizacao antes"); return; }
    if (xcup_prepare() != 0) { st_fail("pendrive nao encontrado"); return; }
    beta_header(bhdr, sizeof(bhdr));
    xc_log("[update] instalando v%s (%s)", m.version, m.channel);
    int r = download(&m, bhdr);
    if (r == 1) { st_set(XCU_CANCELLED, "Cancelado (o download continua de onde parou)"); return; }
    if (r != 0) return;
    st_set(XCU_VERIFYING, "Conferindo o arquivo...");
    st_progress(0, m.size);
    int io = 0;
    if (file_sha_ok(XCU_NEW, m.size, m.sha256, 1, &io) != 0) {
        if (cancelled()) { st_set(XCU_CANCELLED, "Cancelado"); return; }
        if (io) { st_fail("erro lendo o pendrive"); return; }
        xcup_unlink(XCU_NEW);
        xcup_unlink(XCU_INF);
        st_fail("arquivo baixado corrompido (sha256); tente de novo");
        return;
    }
    st_set(XCU_INSTALLING, "Instalando... nao desligue o Xbox");
    if (swap_in(&m) != 0) return;
    xcup_unlink(XCU_INF);
    char s[96];
    snprintf(s, sizeof(s), "Atualizado para v%s. Reinicie o Xbox.", m.version);
    st_progress(m.size, m.size);
    st_set(XCU_DONE, s);
    xc_log("[update] v%s instalada (backup em %s)", m.version, XCU_OLD);
}

/* ================================================================ worker */

static volatile int g_job = 0;   /* 1 = check, 2 = install */

static void worker(void) {
    xcn_set_cancel_hook(cancel_hook);
    if (g_job == 1) do_check();
    else do_install();
    xcup_net_close();
    xcn_set_cancel_hook(NULL);
    xcup_sync();
    g_running = 0;
    st_set(-1, NULL);
}

static int start(int job) {
    if (!g_inited || g_running || xcup_slot_busy()) return -1;
    g_job = job;
    g_cancel = 0;
    g_running = 1;
    xcup_lock();
    g_st.err[0] = 0;
    xcup_unlock();
    xcup_sync();
    if (xcup_spawn(worker) != 0) { g_running = 0; return -1; }
    return 0;
}

int xcu_check_start(void) { return start(1); }

int xcu_install_start(void) {
    xcup_lock();
    int ok = g_man_ok && xcu_semver_cmp(g_man.version, g_st.current) > 0 &&
             (g_st.phase == XCU_AVAILABLE || g_st.phase == XCU_FAILED || g_st.phase == XCU_CANCELLED);
    xcup_unlock();
    return ok ? start(2) : -1;
}
