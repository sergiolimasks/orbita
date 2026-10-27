/* orbita v0.21.25 — hosttest: DIAG.TXT do decoder (source/dec/xc_decdiag.c) */
#include <stdio.h>
#include <string.h>
#include "dec/xc_decdiag.h"

static int g_fail, g_ok;
#define CHECK(c, ...) do { if (c) g_ok++; else { g_fail++; printf("FALHOU %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    xdd_cfg_t c;
    xdd_defaults(&c);
    CHECK(c.deblock == XDD_DEBLOCK_NONE && c.prof == 1 && c.copy_vmx == 1 && c.spin_low == 1 && c.net_low == 1 &&
          !c.lite && c.decthreads == 0 && c.profile == XDD_PROFILE_DEFAULT, "padrões");
    /* o DIAG.TXT real tem as chaves do Wi-Fi junto; CRLF; comentários */
    xdd_parse(&c, "netlog=0\r\noverlay=1\r\ndeblock=nonref\r\ndecthreads=4\r\nprofile=High\r\n# copy=memcpy\r\n"
                  "spinprio=med\r\ndiag=lite\r\n");
    CHECK(c.deblock == XDD_DEBLOCK_NONREF, "deblock nonref");
    CHECK(c.decthreads == 4, "decthreads");
    CHECK(c.profile == XDD_PROFILE_HIGH, "profile high (maiúscula)");
    CHECK(c.copy_vmx == 1, "comentário ignorado");
    CHECK(c.spin_low == 0 && c.net_low == 1, "spinprio med / netprio padrão");
    CHECK(c.lite == 1 && c.netlog_set == 1, "lite + netlog presente");
    CHECK(c.keys == 5, "chaves contadas %d", c.keys);
    xdd_defaults(&c);
    xdd_parse(&c, "deblock = all\ncopy=memcpy\nnetprio=med\ndecthreads=7\nprofile=4k\ndiag=full\ndecprof=0");
    CHECK(c.deblock == XDD_DEBLOCK_ALL && !c.copy_vmx && !c.net_low && c.decthreads == 0 &&
          c.profile == XDD_PROFILE_DEFAULT && !c.lite && !c.prof, "espaços, valores inválidos ignorados");
    xdd_defaults(&c);
    xdd_parse(&c, "deblock=none\nprofile=baseline\nprofile=main");
    CHECK(c.deblock == XDD_DEBLOCK_NONE && c.profile == XDD_PROFILE_MAIN, "a última vence");
    xdd_parse(&c, "");
    xdd_parse(&c, "semigual\n=\nx=");
    CHECK(c.profile == XDD_PROFILE_MAIN, "lixo não muda nada");
    CHECK(!strcmp(xdd_profile_plid(XDD_PROFILE_HIGH), "64001f") && !strcmp(xdd_profile_plid(XDD_PROFILE_MAIN), "4d001f") &&
          !strcmp(xdd_profile_plid(XDD_PROFILE_BASELINE), "42e01f") && xdd_profile_plid(XDD_PROFILE_DEFAULT) == NULL,
          "profile-level-id");
    CHECK(!strcmp(xdd_h264_profile_name(100), "High") && !strcmp(xdd_h264_profile_name(66), "Baseline"), "nomes");
    char d[256];
    xdd_describe(&c, d, sizeof(d));
    CHECK(strstr(d, "profile=main") != NULL, "describe: %s", d);
    printf("dec_test: %d ok, %d falhas\n", g_ok, g_fail);
    return g_fail != 0;
}
