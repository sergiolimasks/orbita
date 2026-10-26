/*
 * XCloud360 / orbita v0.20.3 — testes do host da chave beta (source/beta):
 * identidade de hardware, teclado na tela, classificação das respostas,
 * BETAOK.TXT assinado (adulterado, outro console, outra chave), janela de 7
 * dias e a máquina de estados com transporte falso. Pendrive = pasta.
 *
 * Modos:
 *   beta_unit unit <pasta>            transporte falso (sem rede), ASan+UBSan
 *   beta_unit fake <pasta> <porta>    servidor fake LOCAL (hosttest/beta/
 *                                     fake_server.py, HTTP puro) com o
 *                                     contrato novo: 204/401/403 revoked/
 *                                     403 duplicate/400 sem X-Console
 *   beta_unit live <pasta>            x360.pistatrack.com pelo NOSSO https.c,
 *                                     chave de dev em $XCB_LIVE_KEY
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../source/beta/xc_beta.h"
#include "../../source/beta/xc_keypad.h"
#include "../../source/login/https.h"
#include "../../source/login/xc_port.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_dir[512];

/* ---------------------------------------------------------------- pendrive */
static int st_load(const char *name, void *buf, size_t cap) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (int)n;
}
static int st_save(const char *name, const void *buf, size_t n) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}
static void st_put(const char *name, const char *txt) { st_save(name, txt, strlen(txt)); }
static void st_rm(const char *name) {
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    unlink(p);
}
static int st_read(const char *name, char *out, size_t cap) {
    int n = st_load(name, out, cap - 1);
    if (n < 0) return -1;
    out[n] = 0;
    return n;
}

/* ---------------------------------------------------------------- relógio */
static long long g_now = 1792900000LL;
static long long f_now(void) { return g_now; }

/* ---------------------------------------------------------------- servidor falso */
static int  g_status = 204, g_netdown, g_calls;
static char g_body_resp[128], g_hdr[256], g_path[64], g_host[64];
static int f_get(const char *host, const char *path, const char *headers, int *status, char *body, size_t cap) {
    g_calls++;
    snprintf(g_host, sizeof(g_host), "%s", host);
    snprintf(g_path, sizeof(g_path), "%s", path);
    snprintf(g_hdr, sizeof(g_hdr), "%s", headers ? headers : "");
    if (g_netdown) return -1;
    *status = g_status;
    snprintf(body, cap, "%s", g_body_resp);
    return 0;
}
static void reply(int st, const char *body) { g_status = st; snprintf(g_body_resp, sizeof(g_body_resp), "%s", body); }

static char g_logbuf[4096];
static void f_log(const char *l) { strncat(g_logbuf, l, sizeof(g_logbuf) - strlen(g_logbuf) - 2); strcat(g_logbuf, "\n"); }

static const uint64_t FUSE_A[XCB_NFUSE] = { 0xc0ffffffffffffffULL, 0x0f0f0f0f0f0f0f0fULL, 0x0f0f0f0f0f0ff00fULL,
                                            0x1122334455667788ULL, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL,
                                            0x99aabbccddeeff00ULL };
static const uint8_t MAC_A[6] = { 0x00, 0x22, 0x48, 0x1a, 0x2b, 0x3c };

static char    g_id[33];
static uint8_t g_hk[32];

static void init_console(const uint64_t *fuse, const uint8_t *mac, int (*get)(const char *, const char *, const char *,
                                                                            int *, char *, size_t)) {
    xcb_hwid(fuse, mac, g_id, g_hk);
    xcb_ops_t o;
    memset(&o, 0, sizeof(o));
    o.get = get;
    o.now = f_now;
    o.load = st_load;
    o.save = st_save;
    o.log = f_log;
    xcb_init(&o, g_id, g_hk);
}

static xcb_snap_t snap(void) { xcb_snap_t s; xcb_snapshot(&s); return s; }

/* ================================================================ identidade */
static void t_hwid(void) {
    printf("identidade de hardware\n");
    char a[33], b[33], c[33], d[33];
    uint8_t ka[32], kb[32], kc[32], kd[32];
    xcb_hwid(FUSE_A, MAC_A, a, ka);
    xcb_hwid(FUSE_A, MAC_A, b, kb);
    CHECK(!strcmp(a, b) && !memcmp(ka, kb, 32), "estável: mesmas entradas, mesmo id");
    CHECK(strlen(a) == 32 && strspn(a, "0123456789abcdef") == 32, "32 hex minúsculos (%s)", a);
    uint64_t f2[XCB_NFUSE];
    memcpy(f2, FUSE_A, sizeof(f2));
    f2[4] ^= 1;   /* um bit da CPU key */
    xcb_hwid(f2, MAC_A, c, kc);
    CHECK(strcmp(a, c) && memcmp(ka, kc, 32), "fusível diferente -> id diferente");
    uint8_t m2[6];
    memcpy(m2, MAC_A, 6);
    m2[5]++;
    xcb_hwid(FUSE_A, m2, d, kd);
    CHECK(strcmp(a, d) && memcmp(ka, kd, 32), "MAC diferente -> id diferente");
    /* o id não carrega fusível/MAC em claro, e a chave do HMAC não é o id */
    CHECK(!strstr(a, "1122334455667788") && !strstr(a, "99aabbcc") && !strstr(a, "1a2b3c"), "nada em claro");
    char kh[65];
    for (int i = 0; i < 32; i++) snprintf(kh + 2 * i, 3, "%02x", ka[i]);
    CHECK(strncmp(kh, a, 32) != 0, "chave do HMAC != id mostrado");
    char s[16];
    xcb_id_short("a1b2c3d4e5f60718293a4b5c6d7e8f90", s);
    CHECK(!strcmp(s, "A1B2-C3D4-E5F6"), "curto: %s", s);
}

/* ================================================================ chave */
static void t_key(void) {
    printf("chave: formato, máscara, respostas\n");
    char o[80];
    CHECK(xcb_key_norm("  ABCDE-FGHJK \r\n", o, sizeof(o)) == 0 && !strcmp(o, "ABCDE-FGHJK"), "apara: %s", o);
    CHECK(xcb_key_norm("curta", o, sizeof(o)) != 0, "curta");
    CHECK(xcb_key_norm("tem espaco no meio", o, sizeof(o)) != 0, "espaço");
    CHECK(xcb_key_norm("ABCDE-FGHJK\nlixo", o, sizeof(o)) == 0 && !strcmp(o, "ABCDE-FGHJK"), "só a 1a linha");
    xcb_key_canon("abcde-fghjk", o, sizeof(o));
    CHECK(!strcmp(o, "ABCDEFGHJK"), "canon: %s", o);
    xcb_key_mask("ABCDE-FGHJK", o, sizeof(o));
    CHECK(!strcmp(o, "*****-**HJK"), "máscara: %s", o);
    CHECK(xcb_classify(0, 204, "") == XCB_R_OK, "204");
    CHECK(xcb_classify(0, 401, "{\"reason\":\"invalid\"}") == XCB_R_INVALID, "401");
    CHECK(xcb_classify(0, 403, "{\"reason\":\"revoked\"}") == XCB_R_REVOKED, "403 revoked");
    CHECK(xcb_classify(0, 403, "{\"reason\":\"duplicate\"}") == XCB_R_DUPLICATE, "403 duplicate");
    CHECK(xcb_classify(0, 403, "") == XCB_R_REVOKED, "403 sem corpo = bloqueada");
    CHECK(xcb_classify(-1, 0, "") == XCB_R_NOCONN, "rede");
    CHECK(xcb_classify(0, 502, "") == XCB_R_NOCONN && xcb_classify(0, 429, "") == XCB_R_NOCONN, "5xx/429 = servidor fora");
    CHECK(xcb_classify(0, 400, "") == XCB_R_ERROR, "400 = erro");
}

/* ================================================================ teclado */
static void t_keypad(void) {
    printf("teclado na tela\n");
    xkp_t k;
    xkp_init(&k);
    char t[16];
    CHECK(xkp_row_len(0) == 8 && xkp_row_len(3) == 7 && xkp_row_len(XKP_ROW_ACT) == 3, "grade 8/8/8/7 + 3 ações");
    CHECK(xkp_char_at(0, 0) == '2' && xkp_char_at(1, 0) == 'A' && xkp_char_at(3, 6) == 'Z' && !xkp_char_at(3, 7),
          "alfabeto na grade");
    const char *al = XCB_ALPHABET;
    CHECK(!strchr(al, '0') && !strchr(al, 'O') && !strchr(al, '1') && !strchr(al, 'I') && !strchr(al, 'L'),
          "sem 0/O/1/I/L");
    /* navegação: para nas pontas */
    xkp_nav(&k, -1, 0); xkp_nav(&k, 0, -1);
    CHECK(k.row == 0 && k.col == 0, "canto: não dá a volta");
    for (int i = 0; i < 10; i++) xkp_nav(&k, 1, 0);
    CHECK(k.col == 7, "direita para na 8a");
    xkp_nav(&k, 0, 1); xkp_nav(&k, 0, 1); xkp_nav(&k, 0, 1);
    CHECK(k.row == 3 && k.col == 6, "fileira de 7: coluna presa (%d)", k.col);
    xkp_nav(&k, 0, 1);
    CHECK(k.row == XKP_ROW_ACT && k.col == XKP_ACT_OK, "desce pras ações (Confirmar embaixo de Z)");
    xkp_nav(&k, -1, 0);
    CHECK(k.col == XKP_ACT_FILE, "ações: esquerda");
    xkp_nav(&k, 0, -1);
    CHECK(k.row == 3 && k.col == 4, "sobe na coluna do meio da ação (%d)", k.col);
    /* digitar: A na tecla em foco */
    for (int i = 0; i < 6; i++) xkp_nav(&k, -1, -1);   /* um passo por vez, como o direcional */
    CHECK(xkp_press(&k) == XKP_TYPED && k.n == 1 && k.raw[0] == '2', "A digita a tecla");
    CHECK(xkp_type(&k, 'a') && xkp_type(&k, 'b') && xkp_type(&k, 'C') && xkp_type(&k, 'D'), "minúscula aceita");
    xkp_text(&k, t);
    CHECK(!strcmp(t, "2ABCD-"), "hífen automático no 5o: %s", t);
    CHECK(!xkp_type(&k, '0') && !xkp_type(&k, 'O') && !xkp_type(&k, 'I') && !xkp_type(&k, '-') && k.n == 5,
          "fora do alfabeto ignora");
    CHECK(xkp_press(&k) == XKP_TYPED, "digita de novo");   /* foco ainda no '2' */
    xkp_text(&k, t);
    CHECK(!strcmp(t, "2ABCD-2"), "segundo bloco: %s", t);
    /* Confirmar só com 10 */
    k.row = XKP_ROW_ACT; k.col = XKP_ACT_OK;
    CHECK(xkp_press(&k) == XKP_NONE && !xkp_complete(&k), "Confirmar incompleto não vale");
    for (const char *p = "EFGH"; *p; p++) xkp_type(&k, *p);
    CHECK(xkp_complete(&k) && k.row == XKP_ROW_ACT && k.col == XKP_ACT_OK, "10 letras: foco pula pro Confirmar");
    xkp_text(&k, t);
    CHECK(!strcmp(t, "2ABCD-2EFGH"), "chave: %s", t);
    CHECK(!xkp_type(&k, 'Z') && k.n == 10, "11a ignorada");
    CHECK(xkp_press(&k) == XKP_CONFIRM, "Confirmar");
    /* apagar: B e a tecla Apagar; o hífen some junto do 5o */
    CHECK(xkp_backspace(&k) && k.n == 9, "B apaga");
    k.col = XKP_ACT_DEL;
    for (int i = 0; i < 4; i++) CHECK(xkp_press(&k) == XKP_DELETED, "Apagar %d", i);
    xkp_text(&k, t);
    CHECK(!strcmp(t, "2ABCD-") && k.n == 5, "apagou até o hífen: %s", t);
    xkp_backspace(&k);
    xkp_text(&k, t);
    CHECK(!strcmp(t, "2ABC"), "hífen sai com a 5a: %s", t);
    while (xkp_backspace(&k)) {}
    CHECK(k.n == 0 && !xkp_backspace(&k), "vazio: B não apaga (o main usa pra voltar)");
    k.row = XKP_ROW_ACT; k.col = XKP_ACT_DEL;
    CHECK(xkp_press(&k) == XKP_NONE, "Apagar vazio = nada");
    k.col = XKP_ACT_FILE;
    CHECK(xkp_press(&k) == XKP_FROM_FILE, "Ler do pendrive");
}

/* ================================================================ BETAOK.TXT */
static void t_okfile(void) {
    printf("ultima validacao assinada\n");
    char a[33], b[33], f[160];
    uint8_t ka[32], kb[32];
    xcb_hwid(FUSE_A, MAC_A, a, ka);
    uint8_t m2[6];
    memcpy(m2, MAC_A, 6);
    m2[0] ^= 2;
    xcb_hwid(FUSE_A, m2, b, kb);
    int n = xcb_okfile_make(ka, a, "ABCDE-FGHJK", 1792900000LL, f, sizeof(f));
    long long ts = 0;
    CHECK(n > 0 && xcb_okfile_check(ka, a, "ABCDE-FGHJK", f, (size_t)n, &ts) == 0 && ts == 1792900000LL, "confere");
    CHECK(xcb_okfile_check(ka, a, "abcdefghjk", f, (size_t)n, NULL) == 0, "mesma chave em outra grafia");
    CHECK(xcb_okfile_check(ka, a, "ZZZZZ-FGHJK", f, (size_t)n, NULL) != 0, "outra chave");
    CHECK(xcb_okfile_check(kb, b, "ABCDE-FGHJK", f, (size_t)n, NULL) != 0, "outro console (arquivo copiado)");
    char t[160];
    snprintf(t, sizeof(t), "%s", f);
    char *sp = strchr(t, ' ');
    sp[3] = sp[3] == '9' ? '8' : '9';   /* mexe na data */
    CHECK(xcb_okfile_check(ka, a, "ABCDE-FGHJK", t, strlen(t), NULL) != 0, "data editada à mão");
    snprintf(t, sizeof(t), "%s", f);
    t[n - 2] = t[n - 2] == '0' ? '1' : '0';   /* mexe no HMAC */
    CHECK(xcb_okfile_check(ka, a, "ABCDE-FGHJK", t, strlen(t), NULL) != 0, "HMAC editado");
    CHECK(xcb_okfile_check(ka, a, "ABCDE-FGHJK", "lixo", 4, NULL) != 0, "lixo");
    long long day = 86400;
    CHECK(xcb_grace_ok(1000 * day, 1000 * day + 7 * day), "7 dias certinho");
    CHECK(xcb_grace_ok(1000 * day, 1000 * day + 3 * day), "3 dias");
    CHECK(!xcb_grace_ok(1000 * day, 1000 * day + 7 * day + 1), "7 dias + 1 s");
    CHECK(!xcb_grace_ok(1000 * day, -1), "hora desconhecida");
    CHECK(!xcb_grace_ok(1000 * day, 1000 * day - 2 * day), "relógio voltou");
    CHECK(xcb_grace_ok(1000 * day, 1000 * day - 300), "relógio um pouco atrás");
}

/* ================================================================ estados */
static void t_states(void) {
    printf("estados (transporte falso)\n");
    st_rm(XCB_KEY_FILE); st_rm(XCB_OK_FILE);
    init_console(FUSE_A, MAC_A, f_get);
    xcb_snap_t s;
    /* boot sem chave: teclado */
    g_calls = 0;
    xcb_boot_start();
    s = snap();
    CHECK(s.phase == XCB_NEED_KEY && !s.authorized && g_calls == 0, "sem BETAKEY.TXT -> teclado, sem pedido");
    /* 401 na digitada: não grava nada */
    reply(401, "{\"reason\":\"invalid\"}");
    xcb_submit_start("ABCDE-FGHJK");
    s = snap();
    char b[200];
    CHECK(s.phase == XCB_INVALID && !s.authorized && st_read(XCB_KEY_FILE, b, sizeof(b)) < 0, "401: inválida, nada gravado");
    CHECK(!strcmp(g_host, XCB_HOST) && !strcmp(g_path, XCB_PATH), "GET %s%s", g_host, g_path);
    char want[128];
    snprintf(want, sizeof(want), "X-Api-Key: ABCDE-FGHJK\r\nX-Console: %s\r\n", g_id);
    CHECK(!strcmp(g_hdr, want), "headers X-Api-Key + X-Console: %s", g_hdr);
    /* 204: grava a chave e a validação assinada */
    reply(204, "");
    g_now = 1792900000LL;
    xcb_submit_start("ABCDE-FGHJK");
    s = snap();
    CHECK(s.phase == XCB_OK && s.authorized && s.last_ok == g_now, "204: ok");
    CHECK(st_read(XCB_KEY_FILE, b, sizeof(b)) > 0 && !strcmp(b, "ABCDE-FGHJK"), "BETAKEY.TXT gravado: %s", b);
    int n = st_read(XCB_OK_FILE, b, sizeof(b));
    CHECK(n > 0 && xcb_okfile_check(g_hk, g_id, "ABCDE-FGHJK", b, (size_t)n, NULL) == 0, "BETAOK.TXT assinado");
    CHECK(!strcmp(s.key_masked, "*****-**HJK"), "mascarada: %s", s.key_masked);
    char k[80];
    CHECK(xcb_key(k, sizeof(k)) == 11 && !strcmp(k, "ABCDE-FGHJK"), "chave em uso");
    CHECK(!strstr(g_logbuf, "ABCDE") && !strstr(g_logbuf, "1122334455667788") && !strstr(g_logbuf, "1a2b3c"),
          "log sem chave/fusível/MAC");
    /* próximo boot: valida de novo (a cada boot) */
    init_console(FUSE_A, MAC_A, f_get);
    g_calls = 0;
    xcb_boot_start();
    s = snap();
    CHECK(s.phase == XCB_OK && s.authorized && g_calls == 1, "boot com chave: valida (calls %d)", g_calls);
    /* revogada / duplicada no boot: bloqueia */
    init_console(FUSE_A, MAC_A, f_get);
    reply(403, "{\"reason\":\"revoked\"}");
    xcb_boot_start();
    s = snap();
    CHECK(s.phase == XCB_REVOKED && !s.authorized && strstr(s.msg, "revogada"), "403 revoked: %s", s.msg);
    init_console(FUSE_A, MAC_A, f_get);
    reply(403, "{\"reason\":\"duplicate\"}");
    xcb_boot_start();
    s = snap();
    CHECK(s.phase == XCB_DUPLICATE && !s.authorized && strstr(s.msg, "exclusiva de um console"), "403 duplicate");
    /* outra chave depois do bloqueio */
    reply(204, "");
    xcb_submit_start("KMNPQ-RSTUV");
    s = snap();
    CHECK(s.phase == XCB_OK && s.authorized, "outra chave libera");
    /* sem rede, validado há 3 dias: entra */
    init_console(FUSE_A, MAC_A, f_get);
    g_netdown = 1;
    g_now += 3 * 86400;
    xcb_boot_start();
    s = snap();
    CHECK(s.phase == XCB_OFFLINE_OK && s.authorized, "sem rede <= 7 dias: entra (%d)", s.phase);
    /* servidor fora (502) conta como sem conexão */
    init_console(FUSE_A, MAC_A, f_get);
    g_netdown = 0;
    reply(502, "");
    xcb_boot_start();
    s = snap();
    CHECK(s.phase == XCB_OFFLINE_OK, "502 = servidor fora: vale a janela");
    /* > 7 dias: bloqueia com "sem conexão" */
    init_console(FUSE_A, MAC_A, f_get);
    g_netdown = 1;
    g_now += 5 * 86400;
    xcb_boot_start();
    s = snap();
    CHECK(s.phase == XCB_NOCONN && !s.authorized && strstr(s.msg, "sem conex"), "> 7 dias: %s", s.msg);
    /* hora desconhecida */
    g_now = -1;
    init_console(FUSE_A, MAC_A, f_get);
    xcb_boot_start();
    CHECK(snap().phase == XCB_NOCONN, "sem hora confiável: não entra");
    g_now = 1792900000LL + 86400;
    /* arquivo de validação adulterado */
    n = st_read(XCB_OK_FILE, b, sizeof(b));
    char *sp = strchr(b, ' ');
    sp[4] = sp[4] == '9' ? '8' : '9';
    st_save(XCB_OK_FILE, b, (size_t)n);
    init_console(FUSE_A, MAC_A, f_get);
    xcb_boot_start();
    s = snap();
    CHECK(s.phase == XCB_NOCONN && !s.authorized && strstr(g_logbuf, "nao confere"), "adulterado: não vale");
    /* pendrive levado pra outro console (BETAOK.TXT válido, mas de outro hw) */
    g_netdown = 0; reply(204, "");
    init_console(FUSE_A, MAC_A, f_get);
    xcb_submit_start("KMNPQ-RSTUV");   /* grava um válido pro A */
    uint8_t m2[6];
    memcpy(m2, MAC_A, 6);
    m2[3] ^= 0x10;
    init_console(FUSE_A, m2, f_get);
    g_netdown = 1;
    xcb_boot_start();
    CHECK(snap().phase == XCB_NOCONN, "BETAOK.TXT de outro console não vale");
    /* digitada sem rede: não grava e não usa a janela */
    st_rm(XCB_KEY_FILE);
    init_console(FUSE_A, MAC_A, f_get);
    xcb_submit_start("ABCDE-FGHJK");
    CHECK(snap().phase == XCB_NOCONN && st_read(XCB_KEY_FILE, b, sizeof(b)) < 0, "submit sem rede: nada gravado");
    /* 400 (sem X-Console no servidor): erro, não a janela */
    g_netdown = 0;
    reply(400, "{\"reason\":\"console\"}");
    xcb_submit_start("ABCDE-FGHJK");
    s = snap();
    CHECK(s.phase == XCB_ERROR && strstr(s.msg, "400"), "400: %s", s.msg);
    /* ler do pendrive (colado no PC) */
    reply(204, "");
    xcb_file_start();
    s = snap();
    CHECK(s.phase == XCB_NEED_KEY && strstr(s.msg, "BETAKEY.TXT"), "pendrive sem arquivo: %s", s.msg);
    st_put(XCB_KEY_FILE, "  chave-de-dev-0123456789abcdef \r\n");
    xcb_file_start();
    s = snap();
    CHECK(s.phase == XCB_OK && s.authorized && strstr(g_hdr, "X-Api-Key: chave-de-dev-0123456789abcdef\r\n"),
          "pendrive: valida a do arquivo (%s)", g_hdr);
}

/* ---------------------------------------------------------------- thread */
static void *th_fn(void *a) { ((void (*)(void))a)(); return NULL; }
static int th_spawn(void (*fn)(void)) {
    pthread_t t;
    if (pthread_create(&t, NULL, th_fn, (void *)fn)) return -1;
    pthread_detach(t);
    return 0;
}
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static void th_lock(void) { pthread_mutex_lock(&g_mu); }
static void th_unlock(void) { pthread_mutex_unlock(&g_mu); }
static int f_get_slow(const char *h, const char *p, const char *hd, int *st, char *b, size_t c) {
    usleep(150000);
    return f_get(h, p, hd, st, b, c);
}

static void t_thread(void) {
    printf("worker em thread\n");
    xcb_hwid(FUSE_A, MAC_A, g_id, g_hk);
    xcb_ops_t o;
    memset(&o, 0, sizeof(o));
    o.get = f_get_slow; o.now = f_now; o.load = st_load; o.save = st_save;
    o.spawn = th_spawn; o.lock = th_lock; o.unlock = th_unlock;
    xcb_init(&o, g_id, g_hk);
    g_netdown = 0;
    reply(204, "");
    xcb_snap_t s0 = snap();
    CHECK(xcb_submit_start("ABCDE-FGHJK") == 0, "sobe");
    CHECK(xcb_running() && xcb_submit_start("ABCDE-FGHJK") != 0, "ocupado: recusa outro");
    xcb_snap_t s = snap();
    CHECK(s.busy, "busy no snapshot (spinner)");
    for (int i = 0; i < 100 && xcb_running(); i++) usleep(20000);
    s = snap();
    CHECK(!xcb_running() && s.phase == XCB_OK && s.seq != s0.seq, "terminou: ok");
}

/* ================================================================ servidor fake local */
static int g_port;
static int http_get(const char *host, const char *path, const char *headers, int *status, char *body, size_t cap) {
    (void)host;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { if (fd >= 0) close(fd); return -1; }
    char req[600];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\n%sConnection: close\r\n\r\n", path, XCB_HOST,
                     headers ? headers : "");
    if (write(fd, req, (size_t)n) != n) { close(fd); return -1; }
    char resp[2048];
    size_t got = 0;
    ssize_t r;
    while (got < sizeof(resp) - 1 && (r = read(fd, resp + got, sizeof(resp) - 1 - got)) > 0) got += (size_t)r;
    close(fd);
    resp[got] = 0;
    if (sscanf(resp, "HTTP/1.%*d %d", status) != 1) return -1;
    char *bd = strstr(resp, "\r\n\r\n");
    snprintf(body, cap, "%s", bd ? bd + 4 : "");
    return 0;
}

static void t_fake_server(void) {
    printf("servidor fake local (contrato da ponte) na porta %d\n", g_port);
    st_rm(XCB_KEY_FILE); st_rm(XCB_OK_FILE);
    g_now = 1792900000LL;
    /* console A: chave boa -> 204 */
    init_console(FUSE_A, MAC_A, http_get);
    xcb_submit_start("GOODK-EY234");
    CHECK(snap().phase == XCB_OK, "A: 204 (%d)", snap().http);
    xcb_boot_start();
    CHECK(snap().phase == XCB_OK, "A de novo (boot): 204");
    /* desconhecida -> 401 */
    xcb_submit_start("ZZZZZ-ZZZZZ");
    CHECK(snap().phase == XCB_INVALID && snap().http == 401, "401");
    /* revogada -> 403 revoked */
    xcb_submit_start("REVKD-KEY23");
    CHECK(snap().phase == XCB_REVOKED && snap().http == 403, "403 revoked");
    /* a mesma chave boa no console B -> duplicate, e a chave cai pros dois */
    uint64_t fb[XCB_NFUSE];
    memcpy(fb, FUSE_A, sizeof(fb));
    fb[3] = 0xdeadbeefcafef00dULL;
    init_console(fb, MAC_A, http_get);
    xcb_submit_start("GOODK-EY234");
    CHECK(snap().phase == XCB_DUPLICATE && snap().http == 403, "B: 403 duplicate");
    init_console(FUSE_A, MAC_A, http_get);
    xcb_boot_start();
    CHECK(snap().phase == XCB_DUPLICATE && !xcb_authorized(), "A também derrubada (boot bloqueia)");
    /* sem X-Console -> 400 (o app sempre manda; aqui o pedido é montado na mão) */
    int st = 0;
    char b[256];
    CHECK(http_get(XCB_HOST, XCB_PATH, "X-Api-Key: NEWKY-23456\r\n", &st, b, sizeof(b)) == 0 && st == 400,
          "sem X-Console: %d", st);
    CHECK(strstr(b, "console") != NULL, "400 {reason: console}: %s", b);
    /* 503 (backend fora, sem cache) = sem conexão: no boot vale a janela de 7 dias */
    st_put(XCB_KEY_FILE, "UNAVL-KEY23");
    {
        char f[160];
        int n = xcb_okfile_make(g_hk, g_id, "UNAVL-KEY23", g_now - 2 * 86400, f, sizeof(f));
        st_save(XCB_OK_FILE, f, (size_t)n);
    }
    init_console(FUSE_A, MAC_A, http_get);
    xcb_boot_start();
    CHECK(snap().phase == XCB_OFFLINE_OK && snap().http == 503, "503 + validada há 2 dias: entra");
    xcb_submit_start("UNAVL-KEY23");
    CHECK(snap().phase == XCB_NOCONN, "503 na digitada: sem conexão");
}

/* ================================================================ live */
static int live_get(const char *host, const char *path, const char *headers, int *status, char *body, size_t cap) {
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    if (https_init(&e) != 0) return -1;
    https_resp_t r;
    memset(&r, 0, sizeof(r));
    if (https_request("beta", "GET", host, path, headers, NULL, 0, 4096, &r, &e) != 0) {
        printf("  rede: %s\n", e.msg);
        return -1;
    }
    *status = r.status;
    snprintf(body, cap, "%s", r.body ? r.body : "");
    https_free(&r);
    return 0;
}
static long long live_now(void) { return xc_time_now(); }

static void t_live(void) {
    const char *key = getenv("XCB_LIVE_KEY");
    printf("live: https://%s%s (chave de dev, console de teste)\n", XCB_HOST, XCB_PATH);
    if (!key || !key[0]) { CHECK(0, "sem XCB_LIVE_KEY"); return; }
    st_rm(XCB_KEY_FILE); st_rm(XCB_OK_FILE);
    uint64_t f[XCB_NFUSE] = { 0 };
    uint8_t m[6] = { 0, 0, 0, 0, 0, 1 };
    xcb_hwid(f, m, g_id, g_hk);
    xcb_ops_t o;
    memset(&o, 0, sizeof(o));
    o.get = live_get; o.now = live_now; o.load = st_load; o.save = st_save; o.log = f_log;
    xcb_init(&o, g_id, g_hk);
    xcb_submit_start(key);
    xcb_snap_t s = snap();
    CHECK(s.phase == XCB_OK && s.http == 204 && s.last_ok > 1700000000LL, "chave de dev: %d (HTTP %d)", s.phase, s.http);
    char b[200];
    int n = st_read(XCB_OK_FILE, b, sizeof(b));
    CHECK(n > 0 && xcb_okfile_check(g_hk, g_id, key, b, (size_t)n, NULL) == 0, "BETAOK.TXT com a hora do Date");
    xcb_submit_start("AAAAA-BBBBB");
    s = snap();
    CHECK(s.phase == XCB_INVALID && s.http == 401, "chave falsa: 401 (%d)", s.http);
    /* informativo: o servidor já tem o contrato novo? */
    int st = 0;
    char hdr[160];
    snprintf(hdr, sizeof(hdr), "X-Api-Key: %s\r\n", key);
    live_get(XCB_HOST, XCB_PATH, hdr, &st, b, sizeof(b));
    printf("  (info) chave de dev sem X-Console: %d (a de dev passa com ou sem; nunca vincula)\n", st);
    st = 0;
    live_get(XCB_HOST, XCB_PATH, "X-Api-Key: AAAAA-BBBBB\r\n", &st, b, sizeof(b));
    CHECK(st == 400 && strstr(b, "console"), "sem X-Console (chave comum): %d %s", st, b);
    st_rm(XCB_KEY_FILE); st_rm(XCB_OK_FILE);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "unit";
    snprintf(g_dir, sizeof(g_dir), "%s", argc > 2 ? argv[2] : "/tmp/xc360-beta-store");
    mkdir(g_dir, 0700);
    setenv("XC_STORE", g_dir, 1);
    if (!strcmp(mode, "unit")) {
        t_hwid();
        t_key();
        t_keypad();
        t_okfile();
        t_states();
        t_thread();
    } else if (!strcmp(mode, "fake")) {
        g_port = argc > 3 ? atoi(argv[3]) : 18090;
        t_fake_server();
    } else if (!strcmp(mode, "live")) {
        t_live();
    }
    st_rm(XCB_KEY_FILE); st_rm(XCB_OK_FILE);
    printf("BETA %s %s: %d/%d checks\n", mode, fails ? "FAILED" : "PASSED", checks - fails, checks);
    return fails ? 1 : 0;
}
