/*
 * XCloud360 v0.17 — teste ao vivo (host) das partes inofensivas:
 *  1. DNS (nosso dns_msg.c por UDP) + TCP + TLS 1.2 + verificação da cadeia
 *     contra as raízes embutidas, pra cada host do fluxo;
 *  2. o mesmo com um host que DEVE falhar (raiz fora da lista / nome errado);
 *  3. GET real no displaycatalog (Browse) — corpo + nomes;
 *  4. pedido de device code (só imprime o código; não faz poll).
 * XC_TIME_UNKNOWN=1: roda tudo como o Xbox sem hora (bootstrap pelo Date).
 */
#include "../../source/login/https.h"
#include "../../source/login/xbox_auth.h"
#include "../../source/login/xc_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *HOSTS[] = {
    "login.microsoftonline.com", "user.auth.xboxlive.com", "xsts.auth.xboxlive.com",
    "xgpuweb.gssv-play-prod.xboxlive.com", "xgpuwebf2p.gssv-play-prod.xboxlive.com",
    "brs.core.gssv-play-prod.xboxlive.com", "displaycatalog.mp.microsoft.com",
};

int main(int argc, char **argv) {
    int fails = 0;
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    if (https_init(&e) != 0) { printf("FAIL init: %s\n", e.msg); return 1; }
    int only_dc = argc > 1 && !strcmp(argv[1], "devicecode");
    int no_dc = argc > 1 && !strcmp(argv[1], "nodc");

    if (!only_dc) {
        if (xc_time_now() < 0) {
            /* Sem hora: o 1º pedido HTTP acerta o relógio pelo Date. */
            https_resp_t r;
            int rc = https_request("hora", "GET", "displaycatalog.mp.microsoft.com",
                                   "/v7.0/products?bigIds=9NKX70BBCDRN&market=US&languages=en-US&fieldsTemplate=Browse",
                                   "Accept: application/json\r\n", NULL, 0, 4 << 20, &r, &e);
            printf("%s bootstrap de hora pelo Date: rc=%d status=%d agora=%lld %s\n",
                   rc == 0 && xc_time_now() > 0 ? "PASS" : "FAIL", rc, r.status, xc_time_now(), rc ? e.msg : "");
            if (rc != 0 || xc_time_now() < 0) fails++;
            https_free(&r);
        }
        for (unsigned i = 0; i < sizeof(HOSTS) / sizeof(HOSTS[0]); i++) {
            uint32_t ip = 0;
            int d = xcn_resolve(HOSTS[i], &ip, 6000);
            uint64_t t0 = xcn_now_ms();
            int rc = d == 0 ? https_probe(HOSTS[i], &e) : -1;
            printf("%s %-42s dns=%u.%u.%u.%u tls+verify %s (%ums)\n", rc == 0 ? "PASS" : "FAIL", HOSTS[i],
                   ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255, rc == 0 ? "ok" : e.msg,
                   (unsigned)(xcn_now_ms() - t0));
            if (rc != 0) fails++;
        }
        /* Negativos: precisam FALHAR na verificação. */
        static const char *BAD[] = { "www.google.com", "expired.badssl.com", "wrong.host.badssl.com" };
        for (unsigned i = 0; i < 3; i++) {
            int rc = https_probe(BAD[i], &e);
            printf("%s %-42s (esperado falhar) -> %s\n", rc == HTTPS_ECERT ? "PASS" : "FAIL", BAD[i],
                   rc == 0 ? "ACEITOU (ruim!)" : e.msg);
            if (rc != HTTPS_ECERT) fails++;
        }
        /* GET real com corpo (chunked ou Content-Length) e parse de nomes. */
        https_resp_t r;
        int rc = https_request("nomes", "GET", "displaycatalog.mp.microsoft.com",
                               "/v7.0/products?bigIds=9NKX70BBCDRN,9PNCL2R6G8D0&market=US&languages=en-US&fieldsTemplate=Browse",
                               "Accept: application/json\r\n", NULL, 0, 8 << 20, &r, &e);
        xa_title_t t[2];
        memset(t, 0, sizeof(t));
        strcpy(t[0].id, "FORZAHORIZON5"); strcpy(t[0].product, "9NKX70BBCDRN");
        strcpy(t[1].id, "COD"); strcpy(t[1].product, "9PNCL2R6G8D0");
        int got = rc == 0 && r.status == 200 ? xa_parse_names(r.body, t, 2) : -1;
        printf("%s displaycatalog GET: rc=%d HTTP %d, %u bytes, nomes=%d [%s] [%s]\n",
               got == 2 ? "PASS" : "FAIL", rc, r.status, (unsigned)r.len, got, t[0].name, t[1].name);
        if (got != 2) fails++;
        https_free(&r);
    }

    /* Device code: só pede e mostra (não faz poll nem completa login). */
    xa_devcode_t dc;
    int rc = no_dc ? -1 : xa_request_device_code(&dc, &e);
    if (no_dc) {
        printf("(device code pulado nesta rodada)\n");
    } else if (rc == XA_OK) {
        printf("PASS device code: acesse %s e digite %s (expira em %ds, intervalo %ds; device_code %u chars, nao usado)\n",
               dc.uri, dc.user_code, dc.expires_in, dc.interval, (unsigned)strlen(dc.device_code));
        xa_devcode_free(&dc);
    } else {
        printf("FAIL device code: %s\n", e.msg);
        fails++;
    }
    printf("login_live: %d falhas\n", fails);
    return fails ? 1 : 0;
}
