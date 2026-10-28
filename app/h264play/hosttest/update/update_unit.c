/*
 * XCloud360 — testes do host do xc_update (sem rede): semver, manifesto,
 * assinatura, download com retomada, falhas na troca. Fixtures geradas pelo
 * run_update.sh (chave de TESTE, nunca a de release).
 * Uso: update_unit <fixtures> <pasta-pendrive>
 */
#include "../../source/update/xc_update.h"
#include "../../source/settings/xc_notes.h"   /* v0.21.33 */
#include "../../source/login/xc_port.h"
#include "../../source/update/xcu_pubkey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*xcup_get_fn)(const char *, const char *, const char *, size_t, int *, char **, size_t *, char *, size_t);
void xcup_test_root(const char *d);
void xcup_test_set_get(xcup_get_fn f);
void xcup_fail_renames(int a, int b);
void xcup_fail_write_after(long long bytes);
void xcup_free_override(long long b);

static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("  FALHOU %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char g_fx[256], g_root[256];
static char *g_elf = NULL, *g_old = NULL;
static size_t g_elf_n = 0, g_old_n = 0;
static uint8_t g_pub[200];
static size_t g_pub_n = 0;

static char *slurp(const char *dir, const char *name, size_t *n) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("sem fixture %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)l + 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) exit(2);
    b[l] = 0;
    fclose(f);
    if (n) *n = (size_t)l;
    return b;
}

/* ----------------------------------------------------- servidor falso */
static const char *g_manifest = NULL;   /* corpo do /latest (NULL = 404) */
static int  g_req_n = 0;                /* pedidos de pedaço */
static int  g_net_fail_a = 0, g_net_fail_b = 0;   /* pedaços a..b caem */
static long g_first_range = -1;
static int  g_require_key = 0, g_saw_key = 0, g_saw_console = 0;
static int  g_delay_ms = 0;

static int fake_get(const char *host, const char *path, const char *hdr, size_t max_body,
                    int *status, char **body, size_t *len, char *err, size_t errcap) {
    (void)host;
    *body = NULL; *len = 0; *status = 0;
    int key = hdr && strstr(hdr, "X-Api-Key: ABCDE-FGHJK\r\n");
    g_saw_key |= key;
    /* v0.20.3: com chave vai também o ID de hardware */
    if (key && strstr(hdr, "X-Api-Key: ABCDE-FGHJK\r\nX-Console: 0123456789abcdef0123456789abcdef\r\n")) g_saw_console = 1;
    if (!strncmp(path, "/v1/update/latest", 17)) {
        if (!g_manifest) { *status = 404; return 0; }
        *status = 200;
        *len = strlen(g_manifest);
        *body = strdup(g_manifest);
        return 0;
    }
    if (strstr(path, "/xenon.elf")) {
        g_req_n++;
        if (g_delay_ms) xcn_sleep_ms(g_delay_ms);
        if (xcn_cancelled()) { snprintf(err, errcap, "cancelado"); return -1; }
        if (g_net_fail_a && g_req_n >= g_net_fail_a && g_req_n <= g_net_fail_b) {
            snprintf(err, errcap, "TCP caiu (simulado)");
            return -1;
        }
        if (g_require_key && !key) { *status = 401; return 0; }
        const char *r = hdr ? strstr(hdr, "Range: bytes=") : NULL;
        if (!r) { *status = 400; return 0; }
        unsigned long a = 0, b = 0;
        sscanf(r + 13, "%lu-%lu", &a, &b);
        if (g_first_range < 0) g_first_range = (long)a;
        if (a >= g_elf_n) { *status = 416; return 0; }
        if (b >= g_elf_n) b = g_elf_n - 1;
        size_t n = b - a + 1;
        if (n > max_body) { snprintf(err, errcap, "grande demais"); return -1; }
        *body = malloc(n + 1);
        memcpy(*body, g_elf + a, n);
        (*body)[n] = 0;
        *len = n;
        *status = 206;
        return 0;
    }
    *status = 404;
    return 0;
}

/* ------------------------------------------------------------- pendrive */
static void usb_reset(int with_elf) {
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/XC360'", g_root, g_root);
    if (system(cmd)) exit(2);
    if (with_elf) {
        char p[600];
        snprintf(p, sizeof(p), "%s/xenon.elf", g_root);
        FILE *f = fopen(p, "wb");
        fwrite(g_old, 1, g_old_n, f);
        fclose(f);
    }
    xcup_fail_renames(0, 0);
    xcup_fail_write_after(-1);
    xcup_free_override(-2);
    g_req_n = 0; g_net_fail_a = g_net_fail_b = 0; g_first_range = -1;
    g_require_key = g_saw_key = 0; g_delay_ms = 0;
}

static int file_is(const char *name, const char *data, size_t n) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", g_root, name);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char *b = malloc(n + 1);
    size_t r = fread(b, 1, n + 1, f);
    fclose(f);
    int ok = r == n && !memcmp(b, data, n);
    free(b);
    return ok;
}

static long fsz(const char *name) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", g_root, name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fclose(f);
    return l;
}

/* ------------------------------------------------------------ helpers */
static xcu_snap_t wait_done(int max_s) {
    xcu_snap_t s;
    for (int i = 0; i < max_s * 100; i++) {
        xcu_snapshot(&s);
        if (!s.busy) return s;
        usleep(10000);
    }
    printf("  timeout esperando o worker\n");
    xcu_snapshot(&s);
    return s;
}

static void init_with(const char *cur) {
    xcu_config_t c = { NULL, 0, g_pub, g_pub_n };
    xcu_init(cur, &c);
}

static xcu_snap_t check(const char *cur, const char *manifest) {
    init_with(cur);
    g_manifest = manifest;
    CHECK(xcu_check_start() == 0);
    return wait_done(10);
}

static xcu_snap_t install(void) {
    int r = xcu_install_start();
    CHECK(r == 0);
    return wait_done(60);
}

static void t_semver(void) {
    printf("semver\n");
    CHECK(xcu_semver_valid("0.20.0"));
    CHECK(xcu_semver_valid("1.2.3-beta.1"));
    CHECK(xcu_semver_valid("0.0.0-test"));
    CHECK(!xcu_semver_valid("1.2"));
    CHECK(!xcu_semver_valid("01.2.3"));
    CHECK(!xcu_semver_valid("1.2.3-"));
    CHECK(!xcu_semver_valid("1.2.3-a..b"));
    CHECK(!xcu_semver_valid("v1.2.3"));
    CHECK(xcu_semver_cmp("0.20.0", "0.19.2") > 0);
    CHECK(xcu_semver_cmp("0.19.10", "0.19.9") > 0);
    CHECK(xcu_semver_cmp("0.21.0-beta.1", "0.20.0") > 0);
    CHECK(xcu_semver_cmp("0.21.0-beta.1", "0.21.0") < 0);
    CHECK(xcu_semver_cmp("0.21.0-beta.2", "0.21.0-beta.10") < 0);
    CHECK(xcu_semver_cmp("0.21.0-beta", "0.21.0-beta.1") < 0);
    CHECK(xcu_semver_cmp("0.21.0-alpha", "0.21.0-beta") < 0);
    CHECK(xcu_semver_cmp("0.21.0-1", "0.21.0-alpha") < 0);
    CHECK(xcu_semver_cmp("1.0.0", "1.0.0") == 0);
    CHECK(xcu_semver_cmp("1.0.0+abc", "1.0.0") == 0);
    CHECK(xcu_semver_cmp("lixo", "0.0.1") < 0);
}

static void t_manifest(void) {
    printf("manifesto + assinatura\n");
    xcu_manifest_t m;
    char e[96];
    size_t n;
    char *good = slurp(g_fx, "good.json", &n);
    CHECK(xcu_manifest_parse(good, n, &m, e, sizeof(e)) == 0);
    CHECK(!strcmp(m.version, "0.21.0-beta.1") && !strcmp(m.channel, "beta") && m.size == g_elf_n);
    CHECK(strstr(m.notes, "\xc3\xa7") != NULL);   /* UTF-8 preservado ("ç") */
    CHECK(xcu_manifest_verify(&m, g_pub, g_pub_n) == 0);
    xcu_manifest_t t = m;
    t.size++;
    CHECK(xcu_manifest_verify(&t, g_pub, g_pub_n) != 0);
    t = m; t.sha256[0] = t.sha256[0] == 'a' ? 'b' : 'a';
    CHECK(xcu_manifest_verify(&t, g_pub, g_pub_n) != 0);
    t = m; snprintf(t.channel, sizeof(t.channel), "stable");
    CHECK(xcu_manifest_verify(&t, g_pub, g_pub_n) != 0);
    char *bad = slurp(g_fx, "badsig.json", &n);
    CHECK(xcu_manifest_parse(bad, n, &m, e, sizeof(e)) == 0);
    CHECK(xcu_manifest_verify(&m, g_pub, g_pub_n) != 0);
    char *other = slurp(g_fx, "otherkey.json", &n);
    CHECK(xcu_manifest_parse(other, n, &m, e, sizeof(e)) == 0);
    CHECK(xcu_manifest_verify(&m, g_pub, g_pub_n) != 0);
    /* a chave embutida de release não aceita assinatura da chave de teste */
    CHECK(xcu_manifest_parse(good, strlen(good), &m, e, sizeof(e)) == 0);
    CHECK(xcu_manifest_verify(&m, xcu_pubkey_der, sizeof(xcu_pubkey_der)) != 0);
    /* malformados */
    const char *bads[] = {
        "", "[]", "{\"format\":2}",
        "{\"format\":1,\"channel\":\"nightly\",\"version\":\"1.0.0\",\"size\":100000,\"sha256\":\""
        "0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"/v1/update/1.0.0/xenon.elf\",\"notes\":\"\",\"sig\":\"AAAAAAAAAAAA\"}",
        "{\"format\":1,\"channel\":\"beta\",\"version\":\"1.0\",\"size\":100000,\"sha256\":\""
        "0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"/v1/update/1.0/xenon.elf\",\"notes\":\"\",\"sig\":\"AAAAAAAAAAAA\"}",
        "{\"format\":1,\"channel\":\"beta\",\"version\":\"1.0.0\",\"size\":10,\"sha256\":\""
        "0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"/v1/update/1.0.0/xenon.elf\",\"notes\":\"\",\"sig\":\"AAAAAAAAAAAA\"}",
        "{\"format\":1,\"channel\":\"beta\",\"version\":\"1.0.0\",\"size\":100000,\"sha256\":\"00\",\"url\":\"/v1/update/1.0.0/xenon.elf\",\"notes\":\"\",\"sig\":\"AAAAAAAAAAAA\"}",
        "{\"format\":1,\"channel\":\"beta\",\"version\":\"1.0.0\",\"size\":100000,\"sha256\":\""
        "0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"https://mal.com/x\",\"notes\":\"\",\"sig\":\"AAAAAAAAAAAA\"}",
        "{\"format\":1,\"channel\":\"beta\",\"version\":\"1.0.0\",\"size\":100000,\"sha256\":\""
        "0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"/v1/update/0.9.0/xenon.elf\",\"notes\":\"\",\"sig\":\"AAAAAAAAAAAA\"}",
        "{\"format\":1,\"channel\":\"beta\",\"version\":\"1.0.0\",\"size\":100000,\"sha256\":\""
        "0000000000000000000000000000000000000000000000000000000000000000\",\"url\":\"/v1/update/1.0.0/xenon.elf\",\"notes\":\"\"}",
    };
    for (size_t i = 0; i < sizeof(bads) / sizeof(bads[0]); i++)
        CHECK(xcu_manifest_parse(bads[i], strlen(bads[i]), &m, e, sizeof(e)) != 0);
    free(good); free(bad); free(other);
}

static char *g_good, *g_badsig, *g_wrongsha, *g_stable_old;

static void t_check(void) {
    printf("check\n");
    usb_reset(1);
    xcu_snap_t s = check("0.20.0", g_good);
    CHECK(s.phase == XCU_AVAILABLE && !strcmp(s.latest, "0.21.0-beta.1") && !strcmp(s.channel, "beta"));
    CHECK(s.total == g_elf_n);
    s = check("0.21.0-beta.1", g_good);
    CHECK(s.phase == XCU_UP_TO_DATE);
    s = check("0.21.0", g_good);
    CHECK(s.phase == XCU_UP_TO_DATE);
    s = check("0.21.0-beta.1-full", g_good);    /* não é variante: beta.1-full > beta.1 */
    CHECK(s.phase == XCU_UP_TO_DATE);
    s = check("0.20.0-full", g_stable_old);    /* variante do Makefile = 0.20.0 > 0.19.0 */
    CHECK(s.phase == XCU_UP_TO_DATE);
    s = check("0.20.0", NULL);
    CHECK(s.phase == XCU_UP_TO_DATE);
    s = check("0.20.0", g_badsig);
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "assinatura"));
    CHECK(xcu_install_start() != 0);           /* não instala manifesto recusado */
    CHECK(file_is("xenon.elf", g_old, g_old_n) && fsz(XCU_NEW) < 0);
    s = check("0.20.0", "{lixo");
    CHECK(s.phase == XCU_FAILED);
    /* beta key no header */
    {
        char p[600];
        snprintf(p, sizeof(p), "%s/XC360/BETAKEY.TXT", g_root);
        FILE *f = fopen(p, "w"); fputs("ABCDE-FGHJK\n", f); fclose(f);
        g_saw_key = 0;
        g_saw_console = 0;
        xcu_set_console_id("0123456789abcdef0123456789abcdef");
        s = check("0.20.0", g_good);
        CHECK(s.phase == XCU_AVAILABLE && g_saw_key);
        CHECK(g_saw_console);   /* X-Console junto da chave */
        unlink(p);
        g_saw_key = 0;
        s = check("0.20.0", g_good);
        CHECK(!g_saw_key);
    }
}

/* v0.20.3: atualização obrigatória */
static void t_mandatory(void) {
    printf("obrigatoria\n");
    xcu_manifest_t m;
    char e[96];
    const char *names[] = { "good.json", "mand.json", "opt.json", "opt_tampered.json", "opt_nosig2.json" };
    const int want[] = { 1, 1, 0, 1, 1 };   /* ausente / true / false assinado / adulterado / sem sig2 */
    for (int i = 0; i < 5; i++) {
        size_t n;
        char *j = slurp(g_fx, names[i], &n);
        CHECK(xcu_manifest_parse(j, n, &m, e, sizeof(e)) == 0);
        CHECK(xcu_manifest_verify(&m, g_pub, g_pub_n) == 0);   /* o "sig" de sempre vale em todos */
        CHECK(xcu_manifest_mandatory(&m, g_pub, g_pub_n) == want[i]);
        if (xcu_manifest_mandatory(&m, g_pub, g_pub_n) != want[i]) printf("  (%s)\n", names[i]);
        free(j);
    }
    /* sig2 de outra chave não deixa virar opcional */
    {
        size_t n;
        char *j = slurp(g_fx, "opt.json", &n);
        CHECK(xcu_manifest_parse(j, n, &m, e, sizeof(e)) == 0);
        CHECK(xcu_manifest_mandatory(&m, xcu_pubkey_der, sizeof(xcu_pubkey_der)) == 1);
        free(j);
    }
    /* snapshot expõe */
    usb_reset(1);
    char *mj = slurp(g_fx, "mand.json", NULL), *oj = slurp(g_fx, "opt.json", NULL);
    xcu_snap_t s = check("0.20.3", mj);
    CHECK(s.phase == XCU_AVAILABLE && s.mandatory == 1);
    s = check("0.20.3", oj);
    CHECK(s.phase == XCU_AVAILABLE && s.mandatory == 0);
    s = check("0.20.3", g_good);
    CHECK(s.phase == XCU_AVAILABLE && s.mandatory == 1);   /* campo ausente = obrigatória */
    free(mj); free(oj);
}

static void t_install_ok(void) {
    printf("instalar (caminho feliz)\n");
    usb_reset(1);
    xcu_snap_t s = check("0.20.0", g_good);
    CHECK(s.phase == XCU_AVAILABLE);
    s = install();
    CHECK(s.phase == XCU_DONE && s.pct == 100);
    CHECK(file_is("xenon.elf", g_elf, g_elf_n));
    CHECK(file_is(XCU_OLD, g_old, g_old_n));
    CHECK(fsz(XCU_NEW) < 0 && fsz(XCU_INF) < 0);
    CHECK(g_req_n == (int)((g_elf_n + XCU_CHUNK - 1) / XCU_CHUNK));
    {   /* v0.21.33: NOTES.TXT com as notas assinadas da versão instalada */
        size_t nn = 0;
        char *nb = slurp(g_root, XCN_PATH, &nn);
        xcn_notes_t no;
        CHECK(xcn_notes_parse(nb, nn, &no) == 0);
        CHECK(!strcmp(no.version, "0.21.0-beta.1") && strstr(no.notes, "Correções") != NULL);
        free(nb);
    }
    /* de novo, agora com XENON.OLD já existindo: o backup vira o da vez */
    s = check("0.20.0", g_good);
    s = install();
    CHECK(s.phase == XCU_DONE && file_is("xenon.elf", g_elf, g_elf_n) && file_is(XCU_OLD, g_elf, g_elf_n));
}

static void t_resume(void) {
    printf("download interrompido + retomada\n");
    usb_reset(1);
    xcu_snap_t s = check("0.20.0", g_good);
    xcup_fail_write_after(600 * 1024);          /* pendrive "sai" no 3º pedaço */
    s = install();
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "gravando"));
    CHECK(file_is("xenon.elf", g_old, g_old_n));
    long part = fsz(XCU_NEW);
    CHECK(part >= 2 * XCU_CHUNK && part < (long)g_elf_n);
    xcup_fail_write_after(-1);
    g_first_range = -1; g_req_n = 0;
    s = install();                              /* FAILED -> pode tentar de novo */
    CHECK(s.phase == XCU_DONE);
    CHECK(g_first_range == part);               /* retomou de onde parou */
    CHECK(file_is("xenon.elf", g_elf, g_elf_n) && file_is(XCU_OLD, g_old, g_old_n));

    printf("rede cai no meio (tentativas)\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    g_net_fail_a = 2; g_net_fail_b = 3;         /* 2 quedas seguidas e volta */
    s = install();
    CHECK(s.phase == XCU_DONE && file_is("xenon.elf", g_elf, g_elf_n));

    printf("rede morta -> falha, retomada depois\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    g_net_fail_a = 3; g_net_fail_b = 1000;
    s = install();
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "download"));
    CHECK(fsz(XCU_NEW) == 2 * XCU_CHUNK && file_is("xenon.elf", g_old, g_old_n));
    g_net_fail_a = 0; g_first_range = -1;
    s = install();
    CHECK(s.phase == XCU_DONE && g_first_range == 2 * XCU_CHUNK);

    printf("cancelar no meio + retomada\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    g_delay_ms = 150;
    CHECK(xcu_install_start() == 0);
    usleep(400 * 1000);
    xcu_snapshot(&s);
    CHECK(s.phase == XCU_DOWNLOADING && s.cancellable);
    xcu_cancel();
    s = wait_done(10);
    CHECK(s.phase == XCU_CANCELLED);
    CHECK(fsz(XCU_NEW) > 0 && file_is("xenon.elf", g_old, g_old_n));
    g_delay_ms = 0;
    s = install();
    CHECK(s.phase == XCU_DONE && file_is("xenon.elf", g_elf, g_elf_n));

    printf(".NEW de outra release e' descartado\n");
    usb_reset(1);
    {
        char p[600];
        snprintf(p, sizeof(p), "%s/" XCU_NEW, g_root);
        FILE *f = fopen(p, "wb"); fwrite(g_old, 1, 1000, f); fclose(f);
        snprintf(p, sizeof(p), "%s/" XCU_INF, g_root);
        f = fopen(p, "wb"); fputs("outra 123 0.1.0\n", f); fclose(f);
    }
    s = check("0.20.0", g_good);
    g_first_range = -1;
    s = install();
    CHECK(s.phase == XCU_DONE && g_first_range == 0 && file_is("xenon.elf", g_elf, g_elf_n));
}

static void t_bad_data(void) {
    printf("sha256 errado\n");
    usb_reset(1);
    xcu_snap_t s = check("0.20.0", g_wrongsha);
    CHECK(s.phase == XCU_AVAILABLE);
    s = install();
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "sha256"));
    CHECK(file_is("xenon.elf", g_old, g_old_n) && fsz(XCU_NEW) < 0 && fsz(XCU_OLD) < 0);

    printf("beta exige chave (401)\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    g_require_key = 1;
    s = install();
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "chave"));
    CHECK(file_is("xenon.elf", g_old, g_old_n));

    printf("sem espaco\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    xcup_free_override(100 * 1024);
    s = install();
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "espaco"));
    CHECK(file_is("xenon.elf", g_old, g_old_n));
}

static void t_swap_faults(void) {
    xcu_snap_t s;
    printf("troca: 1o rename falha (xenon.elf -> OLD)\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    xcup_fail_renames(1, 1);
    s = install();
    CHECK(s.phase == XCU_FAILED && file_is("xenon.elf", g_old, g_old_n));
    CHECK(file_is(XCU_NEW, g_elf, g_elf_n));    /* .NEW verificado fica: tentar de novo não baixa */
    g_req_n = 0;
    s = install();
    CHECK(s.phase == XCU_DONE && g_req_n == 0 && file_is("xenon.elf", g_elf, g_elf_n));

    printf("troca: 2o rename falha (NEW -> xenon.elf), rollback por rename\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    xcup_fail_renames(2, 2);
    s = install();
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "anterior"));
    CHECK(file_is("xenon.elf", g_old, g_old_n));

    printf("troca: todos os renames depois do 1o falham -> restaura por copia\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    xcup_fail_renames(2, 0);
    s = install();
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "copia"));
    CHECK(file_is("xenon.elf", g_old, g_old_n));

    printf("troca: renames E copias falham (pior caso) -> erro grave, nada apagado\n");
    usb_reset(1);
    s = check("0.20.0", g_good);
    xcup_fail_renames(2, 0);
    xcup_fail_write_after(-1);
    /* a cópia do OLD falha (orçamento acaba depois do download, antes da cópia
     * terminar), a do NEW também -> pior caso: mensagem de erro grave */
    {
        long long dl = (long long)g_elf_n + 200;   /* download + INF cabem */
        xcup_fail_write_after(dl + 1000);
    }
    s = install();
    CHECK(s.phase == XCU_FAILED && strstr(s.err, "GRAVE"));
    /* mesmo no pior caso nada foi apagado: backup e novo verificados estão lá */
    CHECK(file_is(XCU_OLD, g_old, g_old_n) && file_is(XCU_NEW, g_elf, g_elf_n));

    printf("pendrive sem xenon.elf (troca cortada por energia): check restaura do OLD\n");
    usb_reset(0);
    {
        char p[600];
        snprintf(p, sizeof(p), "%s/" XCU_OLD, g_root);
        FILE *f = fopen(p, "wb"); fwrite(g_old, 1, g_old_n, f); fclose(f);
    }
    s = check("0.20.0", g_good);
    CHECK(file_is("xenon.elf", g_old, g_old_n));
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("uso: %s <fixtures> <pasta>\n", argv[0]); return 2; }
    snprintf(g_fx, sizeof(g_fx), "%s", argv[1]);
    snprintf(g_root, sizeof(g_root), "%s", argv[2]);
    char st[600];
    snprintf(st, sizeof(st), "%s/XC360", g_root);
    setenv("XC_STORE", st, 1);   /* xc_store_load("BETAKEY.TXT") = XC360/BETAKEY.TXT */
    xcup_test_root(g_root);
    xcup_test_set_get(fake_get);
    g_elf = slurp(g_fx, "new.elf", &g_elf_n);
    g_old = slurp(g_fx, "old.elf", &g_old_n);
    char *pub = slurp(g_fx, "pub.der", &g_pub_n);
    memcpy(g_pub, pub, g_pub_n);
    free(pub);
    g_good = slurp(g_fx, "good.json", NULL);
    g_badsig = slurp(g_fx, "badsig.json", NULL);
    g_wrongsha = slurp(g_fx, "wrongsha.json", NULL);
    g_stable_old = slurp(g_fx, "stable_old.json", NULL);

    t_semver();
    t_manifest();
    t_check();
    t_mandatory();
    t_install_ok();
    t_resume();
    t_bad_data();
    t_swap_faults();
    printf("%d/%d checagens ok\n", g_checks - g_fails, g_checks);
    if (g_fails) { printf("UPDATE UNIT: %d FALHAS\n", g_fails); return 1; }
    printf("UPDATE UNIT OK\n");
    return 0;
}
