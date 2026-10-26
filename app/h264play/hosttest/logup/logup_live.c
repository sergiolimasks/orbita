/*
 * XCloud360 v0.20.1 — teste AO VIVO (host) do log beta: o NOSSO https.c
 * (mbedTLS + raízes embutidas, incl. ISRG Root X1/YR) manda 1 lote real
 * pra https://x360.pistatrack.com/v1/logs, com console de teste
 * 00000000000000000000000000000001 e app "hosttest".
 * A chave vem de $XLU_LIVE_KEY (o run_logup.sh lê ~/credentials/xc360-logs-key;
 * nunca vai pra arquivo do repo) e é gravada só no pendrive falso
 * ($XC_STORE, apagado no fim).
 * Uso: logup_live <store_dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../source/login/xc_logup.h"
#include "../../source/login/https.h"
#include "../../source/login/xc_port.h"

static void put(const char *dir, const char *name, const char *txt) {
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(txt, f); fclose(f); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/xc360-logup-live";
    const char *key = getenv("XLU_LIVE_KEY");
    if (!key || !key[0]) { printf("FAIL sem XLU_LIVE_KEY\n"); return 1; }
    mkdir(dir, 0700);
    setenv("XC_STORE", dir, 1);
    put(dir, XLU_KEY_FILE, key);

    xc_err_t e;
    if (https_init(&e) != 0) { printf("FAIL https_init %s\n", e.msg); return 1; }
    xlu_init("hosttest");
    xlu_set_console_id("00000000000000000000000000000001");   /* v0.20.3: console de teste */
    xlu_set_game("Ara: Hist\xc3\xb3ria N\xc3\xa3o Contada");
    xlu_line("[hosttest] logup_live: lote de teste do host (pode apagar)");
    xlu_line("STAT t=2 dec_fps=59.9 disp_fps=59.9 ver=hosttest");
    xlu_stats_t st;
    for (int i = 0; i < 20; i++) {
        xlu_step();
        xlu_stats(&st);
        if (st.batches || st.errors) break;
    }
    char id[40];
    xc_console_id(id, sizeof(id));
    printf("%s live: beta=%d status=%d batches=%u errors=%u console=%s\n",
           st.batches == 1 && st.last_status >= 200 && st.last_status < 300 ? "PASS" : "FAIL", st.beta,
           st.last_status, st.batches, st.errors, id);
    xlu_finish();
    char p[600];
    snprintf(p, sizeof(p), "%s/%s", dir, XLU_KEY_FILE); unlink(p);
    return st.batches == 1 ? 0 : 1;
}
