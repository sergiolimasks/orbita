/*
 * XCloud360 — teste live do xc_update contra a ponte de verdade
 * (x360.pistatrack.com, ?env=test = releases-test/), com o NOSSO HTTPS
 * (https.c + raízes embutidas) e a chave pública EMBUTIDA.
 * Uso: update_live <pasta-pendrive> <modo>
 *   beta-only: release de teste só no beta. Sem chave = nada; com chave
 *              (env XCU_BETA_KEY) = disponível; baixar sem chave = 401;
 *              com chave instala.
 *   promoted:  release de teste promovida a stable. Sem chave = disponível
 *              e instala; com chave = a mesma versão.
 */
#include "../../source/update/xc_update.h"
#include "../../source/login/xc_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void xcup_test_root(const char *d);

static int g_fails = 0;
#define CHECK(c) do { if (!(c)) { g_fails++; printf("  FALHOU %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static char g_root[256];

static xcu_snap_t wait_done(void) {
    xcu_snap_t s;
    unsigned seq = 0;
    for (int i = 0; i < 120 * 50; i++) {
        xcu_snapshot(&s);
        if (s.seq != seq && s.phase == XCU_DOWNLOADING && (s.pct % 25 == 0)) {
            printf("    %s %u/%u (%d%%)\n", s.status, (unsigned)s.done, (unsigned)s.total, s.pct);
        }
        seq = s.seq;
        if (!s.busy) return s;
        usleep(20000);
    }
    printf("  timeout\n");
    return s;
}

static void set_key(const char *k) {
    char p[600];
    snprintf(p, sizeof(p), "%s/XC360/BETAKEY.TXT", g_root);
    if (!k) { unlink(p); return; }
    FILE *f = fopen(p, "w");
    fprintf(f, "%s\n", k);
    fclose(f);
}

static void fresh_usb(void) {
    char cmd[700], p[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/XC360'", g_root, g_root);
    if (system(cmd)) exit(2);
    snprintf(p, sizeof(p), "%s/xenon.elf", g_root);
    FILE *f = fopen(p, "wb");
    fputs("\x7f" "ELF velho", f);
    fclose(f);
}

static xcu_snap_t check(void) {
    xcu_config_t c = { NULL, 1, NULL, 0 };
    xcu_init("0.0.0-alpha", &c);
    CHECK(xcu_check_start() == 0);
    xcu_snap_t s = wait_done();
    printf("  check: fase %d latest=%s canal=%s status=\"%s\" err=\"%s\"\n", s.phase, s.latest, s.channel,
           s.status, s.err);
    return s;
}

static xcu_snap_t install(void) {
    CHECK(xcu_install_start() == 0);
    xcu_snap_t s = wait_done();
    printf("  install: fase %d status=\"%s\" err=\"%s\"\n", s.phase, s.status, s.err);
    return s;
}

static long fsz(const char *n) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", g_root, n);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fclose(f);
    return l;
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("uso: %s <pasta> beta-only|promoted\n", argv[0]); return 2; }
    snprintf(g_root, sizeof(g_root), "%s", argv[1]);
    char st[600];
    snprintf(st, sizeof(st), "%s/XC360", g_root);
    setenv("XC_STORE", st, 1);
    xcup_test_root(g_root);
    const char *key = getenv("XCU_BETA_KEY");
    if (!key) { printf("defina XCU_BETA_KEY (chave de testador)\n"); return 2; }
    xcu_snap_t s;

    if (!strcmp(argv[2], "beta-only")) {
        printf("[sem chave]\n");
        fresh_usb(); set_key(NULL);
        s = check();
        CHECK(s.phase == XCU_UP_TO_DATE);
        printf("[com chave]\n");
        set_key(key);
        s = check();
        CHECK(s.phase == XCU_AVAILABLE && !strcmp(s.channel, "beta") && !strcmp(s.latest, "0.0.0-test"));
        printf("[baixar sem chave -> 401]\n");
        set_key(NULL);
        s = install();
        CHECK(s.phase == XCU_FAILED && strstr(s.err, "chave"));
        CHECK(fsz("xenon.elf") == 10);
        printf("[baixar com chave]\n");
        set_key(key);
        s = install();
        CHECK(s.phase == XCU_DONE && fsz("xenon.elf") == (long)s.total && fsz(XCU_OLD) == 10);
    } else {
        printf("[sem chave, promovida]\n");
        fresh_usb(); set_key(NULL);
        s = check();
        CHECK(s.phase == XCU_AVAILABLE && !strcmp(s.channel, "stable") && !strcmp(s.latest, "0.0.0-test"));
        s = install();
        CHECK(s.phase == XCU_DONE && fsz("xenon.elf") == (long)s.total && fsz(XCU_OLD) == 10);
        printf("[com chave: mesma versao]\n");
        fresh_usb(); set_key(key);
        s = check();
        CHECK(s.phase == XCU_AVAILABLE && !strcmp(s.latest, "0.0.0-test"));
    }
    if (g_fails) { printf("UPDATE LIVE (%s): %d FALHAS\n", argv[2], g_fails); return 1; }
    printf("UPDATE LIVE (%s) OK\n", argv[2]);
    return 0;
}
