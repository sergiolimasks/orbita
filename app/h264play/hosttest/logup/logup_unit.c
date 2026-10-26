/*
 * XCloud360 v0.20.1 — testes do host do log beta (source/login/xc_logup.c),
 * sem rede: transporte falso (o "servidor" responde o status que o teste
 * mandar e guarda os corpos), relógio falso, pendrive = $XC_STORE.
 * Cobre: sem chave não envia nada e guarda só o mínimo; chave inválida no
 * arquivo; chave aceita (lote, JSON, header, ID criado e reaproveitado);
 * período/flush/intervalo mínimo; buffer cheio descarta o mais antigo;
 * 429/502/rede com backoff e retry; 413 diminui o lote; 401 para e expõe
 * XC_BETA_INVALID; xc_beta_save_key grava e religa; filtro de segredos.
 * v0.20.3: ID de HARDWARE vem do main (xlu_set_console_id; sem ele nada sai,
 * nenhum CONSOLE.ID é criado), header X-Console, xlu_use_key (chave já
 * validada pelo gate beta: liga sem regravar o BETAKEY.TXT).
 * Uso: logup_unit <store_dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../source/login/xc_logup.h"
#include "../../source/login/https.h"
#include "../../source/login/xc_port.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- servidor falso ---- */
static int  g_status = 204, g_calls, g_netdown;
static char g_body[600 * 1024], g_hdr[256], g_host[64], g_path[64];
static size_t g_blen;
static int fake_post(const char *host, const char *path, const char *headers, const char *body, size_t len,
                     int *status) {
    g_calls++;
    snprintf(g_host, sizeof(g_host), "%s", host);
    snprintf(g_path, sizeof(g_path), "%s", path);
    snprintf(g_hdr, sizeof(g_hdr), "%s", headers);
    if (len >= sizeof(g_body)) len = sizeof(g_body) - 1;
    memcpy(g_body, body, len);
    g_body[len] = 0;
    g_blen = len;
    if (g_netdown) return -1;
    *status = g_status;
    return 0;
}

static unsigned long long g_now = 1000000;
static unsigned long long fake_now(void) { return g_now; }

static const char *g_store;
static void put_file(const char *name, const char *txt) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", g_store, name);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(txt, f); fclose(f); }
}
static int read_file(const char *name, char *out, size_t cap) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", g_store, name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return (int)n;
}
static void rm_file(const char *name) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", g_store, name);
    unlink(p);
}

static int steps(int n) { int w = 0; for (int i = 0; i < n; i++) w += xlu_step(); return w; }

#define HWID "0123456789abcdef0123456789abcdef"
static void fresh(void) {
    xlu_reset();
    xlu_init("v0.20.1");
    xlu_set_console_id(HWID);
    xlu_set_transport(fake_post);
    xlu_set_clock(fake_now);
    g_status = 204; g_calls = 0; g_netdown = 0; g_body[0] = 0;
}

static int count_lines(const char *body) {   /* strings no array "lines" */
    const char *p = strstr(body, "\"lines\":[");
    if (!p) return -1;
    p += 9;
    if (*p == ']') return 0;
    int n = 1;
    for (int q = 0; *p && !(*p == ']' && !q); p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') q = !q;
        else if (*p == ',' && !q) n++;
    }
    return n;
}

static void test_redact(void) {
    char b[700];
    fresh();
    xlu_add_secret("K7Q2M4XP");
    snprintf(b, sizeof(b), "[login] digite K7Q2M4XP em microsoft.com/link");
    xlu_redact(b, sizeof(b));
    CHECK(!strstr(b, "K7Q2M4XP") && strstr(b, "[x]"), "login code: %s", b);
    snprintf(b, sizeof(b), "Authorization: XBL3.0 x=123;eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.abc");
    xlu_redact(b, sizeof(b));
    CHECK(!strstr(b, "eyJhbGci") && strstr(b, "[token]"), "jwt: %s", b);
    snprintf(b, sizeof(b), "mail fulano.de.tal@outlook.com ok");
    xlu_redact(b, sizeof(b));
    CHECK(!strstr(b, "fulano") && strstr(b, "[email]") && strstr(b, " ok"), "email: %s", b);
    /* o que é diagnóstico fica: UUID de sessão, bigId, números do STAT */
    snprintf(b, sizeof(b), "[sess] id 3f2504e0-4f89-11d3-9a0c-0305e82c3301 BT5P2X999VH2 STAT t=12 dec_ms=9.1");
    char o[700];
    snprintf(o, sizeof(o), "%s", b);
    xlu_redact(b, sizeof(b));
    CHECK(!strcmp(b, o), "diagnostico intacto: %s", b);
}

int main(int argc, char **argv) {
    g_store = argc > 1 ? argv[1] : "/tmp/xc360-logup-store";
    mkdir(g_store, 0700);
    setenv("XC_STORE", g_store, 1);
    xc_err_t e;
    CHECK(https_init(&e) == 0, "https_init");   /* DRBG pro CONSOLE.ID */

    test_redact();

    /* ---- sem chave: nada sai, buffer fica no mínimo ---- */
    rm_file(XLU_KEY_FILE); rm_file("CONSOLE.ID");
    fresh();
    CHECK(xc_beta_state() == XC_BETA_UNKNOWN && !xc_beta_active(), "boot: estado desconhecido");
    for (int i = 0; i < 2000; i++) { char l[64]; snprintf(l, sizeof(l), "linha de boot %04d xxxxxxxxxxxxxxxxxxxxxxxx", i); xlu_line(l); }
    xlu_stats_t st;
    xlu_stats(&st);
    CHECK(st.buffered > XLU_MIN_BYTES, "antes de ler o pendrive guarda tudo (%zu)", st.buffered);
    steps(5);
    xlu_flush_soon();
    g_now += 120000;
    steps(5);
    xlu_stats(&st);
    CHECK(g_calls == 0 && xc_beta_state() == XC_BETA_NONE, "sem chave: 0 envios (calls %d)", g_calls);
    CHECK(st.buffered <= XLU_MIN_BYTES && st.buffered > 0, "sem chave: só o mínimo (%zu)", st.buffered);
    char id[40];
    CHECK(xc_console_id(id, sizeof(id)) == 32 && !strcmp(id, HWID) && read_file("CONSOLE.ID", id, sizeof(id)) < 0,
          "ID de hardware do main; nenhum CONSOLE.ID no pendrive");

    /* chave inválida no arquivo = sem chave */
    put_file(XLU_KEY_FILE, "curta\n");
    fresh();
    steps(3);
    CHECK(xc_beta_state() == XC_BETA_NONE && g_calls == 0, "chave curta = NONE");
    put_file(XLU_KEY_FILE, "tem espaco no meio\n");
    fresh();
    steps(3);
    CHECK(xc_beta_state() == XC_BETA_NONE, "chave com espaco = NONE");

    /* ---- com chave ---- */
    put_file(XLU_KEY_FILE, "  BetaKey-Tester01 \r\n");
    fresh();
    xlu_set_game("Ara: Hist\xc3\xb3ria N\xc3\xa3o Contada");
    xlu_line("[boot] linha 1 \"aspas\" e \\ barra");
    xlu_line("[boot] linha 2 \xff invalido");
    CHECK(steps(1) == 1 && xc_beta_active(), "chave lida -> ACTIVE");
    CHECK(xc_console_id(id, sizeof(id)) == 32 && !strcmp(id, HWID), "ID = o de hardware (%s)", id);
    char idf[40];
    CHECK(read_file("CONSOLE.ID", idf, sizeof(idf)) < 0, "nada de CONSOLE.ID aleatório");
    steps(1);   /* 1o lote sai logo */
    CHECK(g_calls == 1 && !strcmp(g_host, XLU_HOST) && !strcmp(g_path, XLU_PATH), "1o lote (calls %d)", g_calls);
    CHECK(strstr(g_hdr, "X-Api-Key: BetaKey-Tester01\r\n") && strstr(g_hdr, "application/json"), "header: %s", g_hdr);
    CHECK(strstr(g_hdr, "X-Console: " HWID "\r\n") != NULL, "header X-Console: %s", g_hdr);
    char want[120];
    snprintf(want, sizeof(want), "{\"console\":\"%s\",\"app\":\"v0.20.1\",\"game\":\"Ara: Hist\xc3\xb3ria", id);
    CHECK(!strncmp(g_body, want, strlen(want)), "corpo: %.120s", g_body);
    CHECK(strstr(g_body, "linha 1 \\\"aspas\\\" e \\\\ barra") && strstr(g_body, "linha 2 ? invalido"),
          "escape/UTF-8: %s", g_body);
    CHECK(!strstr(g_body, "BetaKey-Tester01"), "a chave nunca vai no corpo");
    CHECK(count_lines(g_body) >= 2, "linhas no lote (%d)", count_lines(g_body));
    xlu_stats(&st);
    CHECK(st.buffered == 0 && st.batches == 1, "204 limpa o buffer (%zu)", st.buffered);
    /* ID reaproveitado no próximo boot */
    fresh();
    steps(1);
    char id2[40];
    xc_console_id(id2, sizeof(id2));
    CHECK(!strcmp(id, id2), "ID estável entre boots");
    /* sem ID de hardware: não manda nada (nem cria um aleatório) */
    {
        xlu_reset(); xlu_init("v0.20.1"); xlu_set_transport(fake_post); xlu_set_clock(fake_now);
        int c = g_calls;
        xlu_line("sem id"); steps(3); g_now += XLU_PERIOD_MS; xlu_flush_soon(); steps(3);
        CHECK(g_calls == c && xc_console_id(id2, sizeof(id2)) == 0, "sem ID: nada sai");
        xlu_set_console_id("XYZ");   /* formato errado = ignora */
        CHECK(xc_console_id(id2, sizeof(id2)) == 0, "ID inválido ignorado");
        xlu_set_console_id(HWID);
        g_now += XLU_PERIOD_MS; steps(2);
        CHECK(g_calls == c + 1 && strstr(g_hdr, "X-Console: " HWID), "com ID volta a sair");
        fresh();
        steps(1);
    }

    /* período: nada antes de 60 s; flush antecipa (respeitando o mínimo) */
    xlu_line("x"); steps(1);
    int c0 = g_calls;
    xlu_line("y");
    g_now += 1000;  steps(3);
    CHECK(g_calls == c0, "sem envio antes do período");
    xlu_flush_soon();
    steps(1);
    CHECK(g_calls == c0, "flush respeita o intervalo mínimo");
    g_now += XLU_MIN_GAP_MS; steps(1);
    CHECK(g_calls == c0 + 1, "flush manda depois do intervalo mínimo");
    xlu_line("z");
    g_now += XLU_PERIOD_MS; steps(1);
    CHECK(g_calls == c0 + 2, "período de 60 s");

    /* 429 / 502 / rede: backoff e retry; nada se perde */
    xlu_line("depois vem 429");
    g_status = 429; g_now += XLU_PERIOD_MS; steps(1);
    int c1 = g_calls;
    xlu_stats(&st);
    CHECK(st.buffered > 0 && st.backoff_until_ms > g_now, "429: guarda e espera");
    g_now += 10000; xlu_flush_soon(); steps(3);
    CHECK(g_calls == c1, "em backoff não tenta");
    g_status = 502; g_now += 30000; steps(1);
    CHECK(g_calls == c1 + 1, "retry depois do backoff");
    xlu_stats(&st);
    CHECK(st.backoff_until_ms - g_now >= 60000, "backoff dobra (%llu)", st.backoff_until_ms - g_now);
    g_netdown = 1; g_now += 60000; steps(1);
    CHECK(g_calls == c1 + 2, "rede fora conta como tentativa");
    g_netdown = 0; g_status = 204; g_now += 600000; steps(1);
    xlu_stats(&st);
    CHECK(st.buffered == 0 && strstr(g_body, "depois vem 429"), "entregue depois do backoff");

    /* 413: lote menor, depois entrega */
    for (int i = 0; i < 3000; i++) { char l[120]; snprintf(l, sizeof(l), "carga %05d %s", i, "................................................"); xlu_line(l); }
    g_status = 413; g_now += XLU_PERIOD_MS; steps(1);
    size_t big = g_blen;
    g_status = 204; g_now += XLU_MIN_GAP_MS; xlu_flush_soon(); steps(1);
    CHECK(g_blen < big && g_blen > 0, "413 -> lote menor (%zu -> %zu)", big, g_blen);
    for (int i = 0; i < 100; i++) { g_now += XLU_MIN_GAP_MS; xlu_flush_soon(); steps(1); }
    xlu_stats(&st);
    CHECK(st.buffered == 0 && st.rejected_413 == 1, "413 drenado (%zu)", st.buffered);

    /* buffer cheio: descarta o mais antigo, nunca passa do teto */
    for (int i = 0; i < 6000; i++) { char l[120]; snprintf(l, sizeof(l), "enche %05d %s", i, "................................................"); xlu_line(l); }
    xlu_stats(&st);
    CHECK(st.buffered <= XLU_BUF_BYTES && st.lines_dropped > 0, "cheio: %zu, %u descartadas", st.buffered, st.lines_dropped);
    g_status = 401; g_now += XLU_PERIOD_MS; steps(1);
    CHECK(!strstr(g_body, "enche 00000"), "a mais antiga saiu");

    /* 401: para e expõe o estado; buffer volta ao mínimo */
    CHECK(xc_beta_state() == XC_BETA_INVALID && !xc_beta_active(), "401 -> INVALID");
    int c2 = g_calls;
    for (int i = 0; i < 10; i++) { g_now += XLU_PERIOD_MS; xlu_flush_soon(); steps(1); }
    xlu_stats(&st);
    CHECK(g_calls == c2 && st.buffered <= XLU_MIN_BYTES, "INVALID: não envia mais (%d)", g_calls - c2);

    /* chave nova pelo teclado (xc_beta_save_key): grava e religa */
    CHECK(xc_beta_save_key("x") == -1 && xc_beta_save_key("com espaco 123") == -1, "save: formato");
    CHECK(xc_beta_save_key("  NovaChave-02\n") == 0, "save aceita");
    g_status = 204;
    steps(1);
    char kf[100];
    CHECK(read_file(XLU_KEY_FILE, kf, sizeof(kf)) > 0 && !strcmp(kf, "NovaChave-02"), "BETAKEY.TXT gravado (%s)", kf);
    CHECK(xc_beta_active(), "religou");
    xlu_line("depois da chave nova"); xlu_flush_soon(); g_now += XLU_PERIOD_MS; steps(1);
    CHECK(strstr(g_hdr, "X-Api-Key: NovaChave-02\r\n") && strstr(g_body, "depois da chave nova"), "envia com a nova");
    xlu_line("a chave NovaChave-02 nao vaza");
    g_now += XLU_PERIOD_MS; steps(1);
    CHECK(!strstr(g_body, "NovaChave-02"), "chave filtrada do log");

    /* v0.20.3: xlu_use_key (chave validada pelo gate): liga sem regravar e
     * sem ler o arquivo */
    put_file(XLU_KEY_FILE, "ArquivoVelho-99\n");
    fresh();
    xlu_use_key("ABCDE-FGHJK");
    xlu_line("depois do gate");
    steps(2);
    CHECK(read_file(XLU_KEY_FILE, kf, sizeof(kf)) > 0 && !strcmp(kf, "ArquivoVelho-99\n"), "use_key não grava");
    CHECK(xc_beta_active() && strstr(g_hdr, "X-Api-Key: ABCDE-FGHJK\r\nX-Console: " HWID), "use_key envia: %s", g_hdr);
    /* 403 (chave derrubada por duplicidade): para, como o 401 */
    g_status = 403; xlu_line("x"); g_now += XLU_PERIOD_MS; steps(1);
    CHECK(xc_beta_state() == XC_BETA_INVALID, "403 -> INVALID");

    rm_file(XLU_KEY_FILE);
    printf("LOGUP UNIT %s: %d/%d checks\n", fails ? "FAILED" : "PASSED", checks - fails, checks);
    return fails ? 1 : 0;
}
