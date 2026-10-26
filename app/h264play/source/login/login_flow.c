/* XCloud360 v0.17 — ver login_flow.h. */
#include "login_flow.h"
#include "xc_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_MAX (16 * 1024)

static lf_hooks_t g_hooks;   /* zerado = sem ganchos */
void lf_set_hooks(const lf_hooks_t *h) {
    if (h) g_hooks = *h;
    else memset(&g_hooks, 0, sizeof(g_hooks));
}

static void ui_status(const lf_ui_t *ui, const char *t) {
    xc_log("[login] %s", t);
    if (ui && ui->status) ui->status(t);
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

static char *load_token(void) {
    char *buf = (char *)malloc(TOKEN_MAX);
    if (!buf) return NULL;
    int n = xc_store_load(LF_TOKEN_FILE, buf, TOKEN_MAX - 1);
    if (n <= 0) { free(buf); return NULL; }
    buf[n] = 0;
    trim(buf);
    if (strlen(buf) < 20) { xa_secret_free(&buf); return NULL; }
    return buf;
}

static void save_token(const char *t) {
    if (xc_store_save(LF_TOKEN_FILE, t, strlen(t)) == 0)
        xc_log("[login] refresh token salvo no pendrive (%u bytes)", (unsigned)strlen(t));
    else
        xc_log("[login] AVISO: nao consegui salvar o token no pendrive");
}

/* Espera `sec` segundos em fatias (cancelável). 0 = ok, -1 = cancelado. */
static int wait_s(int sec) {
    uint64_t end = xcn_now_ms() + (uint64_t)sec * 1000;
    while (xcn_now_ms() < end) {
        if (xcn_cancelled()) return -1;
        xcn_sleep_ms(100);
    }
    return 0;
}

/* Device code até autorizar. 0 = *refresh novo. */
static int device_login(const lf_ui_t *ui, char **refresh, xc_err_t *e) {
    *refresh = NULL;
    for (int round = 0; round < 20; round++) {
        xa_devcode_t dc;
        ui_status(ui, "Pedindo codigo de login...");
        if (xa_request_device_code(&dc, e) != XA_OK) return -1;
        xc_log("[login] codigo %s em %s (expira em %ds, intervalo %ds)", dc.user_code, dc.uri,
               dc.expires_in, dc.interval);
        if (ui && ui->login_code) ui->login_code(dc.uri, dc.user_code);
        ui_status(ui, "Aguardando o login no celular/PC...");
        uint64_t deadline = xcn_now_ms() + (uint64_t)dc.expires_in * 1000;
        int interval = dc.interval, fails = 0, res = XA_PENDING;
        while (xcn_now_ms() < deadline) {
            if (wait_s(interval) < 0) {
                xa_devcode_free(&dc);
                e->code = HTTPS_ECANCEL;
                snprintf(e->msg, sizeof(e->msg), "login cancelado");
                return -1;
            }
            xc_err_t pe;
            memset(&pe, 0, sizeof(pe));
            res = xa_poll_device_code(&dc, refresh, &pe);
            if (res == XA_OK) break;
            if (res == XA_PENDING) { fails = 0; continue; }
            if (res == XA_SLOWDOWN) { interval += 5; continue; }
            if (res == XA_EXPIRED) break;
            if (res == XA_DENIED) {
                xa_devcode_free(&dc);
                e->code = HTTPS_EHTTP;
                snprintf(e->msg, sizeof(e->msg), "login recusado na conta Microsoft");
                if (ui && ui->login_code) ui->login_code(NULL, NULL);
                return -1;
            }
            /* erro de rede/servidor: tenta de novo no próximo intervalo */
            xc_log("[login] poll falhou: %s", pe.msg);
            if (pe.code == HTTPS_ECANCEL || ++fails >= 6) {
                *e = pe;
                xa_devcode_free(&dc);
                if (ui && ui->login_code) ui->login_code(NULL, NULL);
                return -1;
            }
        }
        xa_devcode_free(&dc);
        if (res == XA_OK) {
            if (ui && ui->login_code) ui->login_code(NULL, NULL);
            ui_status(ui, "Login autorizado");
            return 0;
        }
        xc_log("[login] codigo expirou, pedindo outro");
    }
    e->code = HTTPS_EHTTP;
    snprintf(e->msg, sizeof(e->msg), "login: codigos expiraram");
    return -1;
}

int lf_run(const lf_ui_t *ui, xa_title_t **games, int *ngames, xc_err_t *e) {
    *games = NULL;
    *ngames = 0;
    memset(e, 0, sizeof(*e));
    if (https_init(e) != 0) return -1;
    if (g_hooks.on_start) g_hooks.on_start();
    {   /* seed nova pro próximo boot (entra na personalização do DRBG) */
        unsigned char s[32];
        if (https_random(s, sizeof(s)) == 0) xc_store_save(LF_SEED_FILE, s, sizeof(s));
        memset(s, 0, sizeof(s));
    }

    char *refresh = load_token();
    xc_log("[login] token salvo: %s", refresh ? "sim" : "nao");
    char *access = NULL;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!refresh) {
            if (device_login(ui, &refresh, e) != 0) return -1;
            save_token(refresh);
        }
        ui_status(ui, "Renovando o login Microsoft...");
        char *rnew = NULL;
        int r = xa_refresh(refresh, &rnew, &access, e);
        if (r == XA_OK) {
            if (rnew && strcmp(rnew, refresh) != 0) save_token(rnew);   /* rotaciona */
            if (rnew) { xa_secret_free(&refresh); refresh = rnew; }
            break;
        }
        xa_secret_free(&rnew);
        if (r == XA_INVALID_GRANT) {
            xc_log("[login] login salvo expirou (invalid_grant), pedindo login de novo");
            xc_store_remove(LF_TOKEN_FILE);
            xa_secret_free(&refresh);
            continue;
        }
        xa_secret_free(&refresh);
        return -1;
    }
    xa_secret_free(&refresh);
    if (!access) return -1;

    ui_status(ui, "Entrando na Xbox Live...");
    char *user = NULL, *gssv = NULL;
    int r = xa_user_authenticate(access, &user, e);
    xa_secret_free(&access);
    if (r != XA_OK) return -1;
    r = xa_xsts_authorize(user, "http://gssv.xboxlive.com/", &gssv, e);
    xa_secret_free(&user);
    if (r != XA_OK) return -1;

    ui_status(ui, "Entrando no xCloud...");
    xa_endpoint_t cloud, f2p;
    r = xa_streaming_login("xgpuweb", gssv, &cloud, e);
    if (r != XA_OK) { xa_secret_free(&gssv); return -1; }
    xc_log("[login] xCloud regiao %s", cloud.host);
    xc_err_t e2;
    memset(&e2, 0, sizeof(e2));
    int have_f2p = xa_streaming_login("xgpuwebf2p", gssv, &f2p, &e2) == XA_OK;
    if (!have_f2p) xc_log("[login] xgpuwebf2p indisponivel: %s", e2.msg);
    xa_secret_free(&gssv);

    ui_status(ui, "Baixando o catalogo...");
    xa_title_t *t = NULL;
    int n = 0;
    r = xa_fetch_titles(&cloud, &t, &n, e);
    if (r == XA_OK && g_hooks.on_cloud) g_hooks.on_cloud(&cloud);
    xa_endpoint_free(&cloud);
    if (r != XA_OK) { if (have_f2p) xa_endpoint_free(&f2p); return -1; }
    if (have_f2p) {
        xa_title_t *ads = NULL;
        int na = 0;
        if (xa_fetch_titles(&f2p, &ads, &na, &e2) == XA_OK) xa_merge_f2p(&t, &n, ads, na);
        else xc_log("[cat] catalogo f2p falhou: %s", e2.msg);
        free(ads);
        xa_endpoint_free(&f2p);
    }
    int playable = 0;
    for (int i = 0; i < n; i++) playable += xa_playable(&t[i]);
    xc_log("[cat] %d titulos, %d jogaveis", n, playable);

    /* Nomes: cache do pendrive primeiro, displaycatalog pro que faltar. */
    char *cache = (char *)malloc(512 * 1024);
    int cached = 0;
    if (cache) {
        int cl = xc_store_load(LF_NAMES_FILE, cache, 512 * 1024 - 1);
        if (cl > 0) { cache[cl] = 0; cached = xa_names_from_cache(cache, t, n); }
        free(cache);
    }
    int missing = 0;
    for (int i = 0; i < n; i++) missing += xa_playable(&t[i]) && !t[i].name[0] && t[i].product[0];
    xc_log("[cat] nomes: %d do cache, faltam %d", cached, missing);
    if (missing > 0) {
        ui_status(ui, "Buscando os nomes dos jogos...");
        int got = xa_fetch_names(t, n, NULL, &e2);
        xc_log("[cat] nomes resolvidos: %d", got);
        if (got > 0) {
            char *out = xa_names_to_cache(t, n);
            if (out) { xc_store_save(LF_NAMES_FILE, out, strlen(out)); free(out); }
        }
    }
    n = xa_finalize(t, n);
    if (g_hooks.on_titles) g_hooks.on_titles(t, n);
    *games = t;
    *ngames = n;
    return 0;
}
