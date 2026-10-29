/* XCloud360 v0.17 — ver xbox_auth.h. */
#include "xbox_auth.h"
#include "xc_port.h"
#include "cjson/cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LOGIN_HOST   "login.microsoftonline.com"
#define DEVCODE_PATH "/consumers/oauth2/v2.0/devicecode"
#define TOKEN_PATH   "/consumers/oauth2/v2.0/token"

#define H_FORM "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n"
#define H_XBL  "Content-Type: application/json\r\nAccept: application/json\r\nx-xbl-contract-version: 1\r\n"
#define H_GSSV "Content-Type: application/json\r\nAccept: application/json\r\nx-gssv-client: XboxComBrowser\r\n"

/* Mesmo X-MS-Device-Info do green-nx (catalog.cpp): o GSSV espera isto. */
#define DEVICE_INFO \
    "X-MS-Device-Info: {\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\"," \
    "\"clientAppType\":\"browser\",\"clientAppVersion\":\"26.1.97\"," \
    "\"clientSdkVersion\":\"10.3.7\",\"httpEnvironment\":\"prod\"," \
    "\"sdkInstallId\":\"\"}},\"dev\":{\"hw\":{\"make\":\"Microsoft\"," \
    "\"model\":\"unknown\",\"sdktype\":\"web\"},\"os\":{\"name\":\"android\"," \
    "\"ver\":\"22631.2715\",\"platform\":\"desktop\"},\"displayInfo\":" \
    "{\"dimensions\":{\"widthInPixels\":1280,\"heightInPixels\":720}," \
    "\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}},\"browser\":" \
    "{\"browserName\":\"chrome\",\"browserVersion\":\"140.0.3485.54\"}}}\r\n"

#define SMALL_BODY   (256 * 1024)
#define TITLES_BODY  (16 * 1024 * 1024)
#define NAMES_BODY   (8 * 1024 * 1024)
#define NAMES_BATCH  20

/* ------------------------------------------------------------------ util */

void xa_secret_free(char **s) {
    if (*s) {
        volatile char *p = *s;
        while (*p) *p++ = 0;
        free(*s);
        *s = NULL;
    }
}

void xa_devcode_free(xa_devcode_t *d) { xa_secret_free(&d->device_code); }
void xa_endpoint_free(xa_endpoint_t *p) { xa_secret_free(&p->token); }

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *xa_urlencode(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *o = (char *)malloc(n * 3 + 1), *p = o;
    if (!o) return NULL;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') *p++ = (char)c;
        else { *p++ = '%'; *p++ = hex[c >> 4]; *p++ = hex[c & 15]; }
    }
    *p = 0;
    return o;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static int jint(const cJSON *o, const char *k, int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static int jbool(const cJSON *o, const char *k) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

static void set_err(xc_err_t *e, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(xc_err_t *e, int code, const char *fmt, ...) {
    va_list ap;
    e->code = code;
    e->tls = 0;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
}

/* HTTP != 2xx: mensagem com o status e o "error" do JSON se houver (os
 * corpos de erro da MS não têm token; mesmo assim só o código vai pro log). */
static void http_fail(xc_err_t *e, const char *step, const https_resp_t *r) {
    const char *kind = NULL;
    cJSON *j = cJSON_Parse(r->body ? r->body : "");
    if (j) kind = jstr(j, "error");
    if (!kind && j) {
        /* XSTS: {"XErr":2148916233,...} (sem conta Xbox etc.) */
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(j, "XErr");
        if (cJSON_IsNumber(x)) {
            set_err(e, HTTPS_EHTTP, "HTTP %d em %s (XErr %.0f)", r->status, step, x->valuedouble);
            cJSON_Delete(j);
            return;
        }
    }
    if (kind) {
        char k[48];
        xa_ascii_fold(k, sizeof(k), kind);
        set_err(e, HTTPS_EHTTP, "HTTP %d em %s (%s)", r->status, step, k);
    } else {
        set_err(e, HTTPS_EHTTP, "HTTP %d em %s", r->status, step);
    }
    cJSON_Delete(j);
}

static cJSON *parse_ok(const char *step, int rc, https_resp_t *r, xc_err_t *e) {
    if (rc != 0) return NULL;
    if (r->status < 200 || r->status > 299) { http_fail(e, step, r); return NULL; }
    cJSON *j = cJSON_Parse(r->body);
    if (!j) set_err(e, HTTPS_EPROTO, "JSON invalido em %s", step);
    return j;
}

/* ------------------------------------------------------------- MSA OAuth */

int xa_request_device_code(xa_devcode_t *dc, xc_err_t *e) {
    memset(dc, 0, sizeof(*dc));
    char *scope = xa_urlencode(XA_SCOPE);
    char body[256];
    snprintf(body, sizeof(body), "client_id=%s&scope=%s", XA_CLIENT_ID, scope ? scope : "");
    free(scope);
    https_resp_t r;
    int rc = https_request("devicecode", "POST", LOGIN_HOST, DEVCODE_PATH, H_FORM, body,
                           strlen(body), SMALL_BODY, &r, e);
    cJSON *j = parse_ok("devicecode", rc, &r, e);
    https_free(&r);
    if (!j) return XA_ERR;
    const char *uc = jstr(j, "user_code"), *dcode = jstr(j, "device_code"), *uri = jstr(j, "verification_uri");
    if (!uc || !dcode || !uri) {
        cJSON_Delete(j);
        set_err(e, HTTPS_EPROTO, "devicecode sem campos");
        return XA_ERR;
    }
    snprintf(dc->user_code, sizeof(dc->user_code), "%s", uc);
    snprintf(dc->uri, sizeof(dc->uri), "%s", uri);
    dc->device_code = xstrdup(dcode);
    dc->interval = jint(j, "interval", 5);
    dc->expires_in = jint(j, "expires_in", 900);
    if (dc->interval < 1) dc->interval = 5;
    cJSON_Delete(j);
    return dc->device_code ? XA_OK : XA_ERR;
}

static int oauth_error_kind(const https_resp_t *r) {
    cJSON *j = cJSON_Parse(r->body ? r->body : "");
    const char *k = j ? jstr(j, "error") : NULL;
    int res = XA_ERR;
    if (k) {
        if (!strcmp(k, "authorization_pending")) res = XA_PENDING;
        else if (!strcmp(k, "slow_down")) res = XA_SLOWDOWN;
        else if (!strcmp(k, "expired_token") || !strcmp(k, "bad_verification_code") ||
                 !strcmp(k, "code_expired")) res = XA_EXPIRED;
        else if (!strcmp(k, "authorization_declined") || !strcmp(k, "access_denied")) res = XA_DENIED;
        else if (!strcmp(k, "invalid_grant")) res = XA_INVALID_GRANT;
    }
    cJSON_Delete(j);
    return res;
}

int xa_poll_device_code(const xa_devcode_t *dc, char **refresh, xc_err_t *e) {
    *refresh = NULL;
    size_t bl = strlen(dc->device_code) + 160;
    char *body = (char *)malloc(bl);
    if (!body) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XA_ERR; }
    snprintf(body, bl, "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=%s&device_code=%s",
             XA_CLIENT_ID, dc->device_code);
    https_resp_t r;
    int rc = https_request("token", "POST", LOGIN_HOST, TOKEN_PATH, H_FORM, body, strlen(body),
                           SMALL_BODY, &r, e);
    xa_secret_free(&body);
    if (rc != 0) return XA_ERR;
    if (r.status < 200 || r.status > 299) {
        int k = oauth_error_kind(&r);
        if (k == XA_ERR || k == XA_INVALID_GRANT) { http_fail(e, "token", &r); k = XA_ERR; }
        https_free(&r);
        return k;
    }
    cJSON *j = cJSON_Parse(r.body);
    https_free(&r);
    const char *rt = j ? jstr(j, "refresh_token") : NULL;
    if (!rt) { cJSON_Delete(j); set_err(e, HTTPS_EPROTO, "token sem refresh_token"); return XA_ERR; }
    *refresh = xstrdup(rt);
    cJSON_Delete(j);   /* (o cJSON não zera; o heap é só nosso, aceitável) */
    return *refresh ? XA_OK : XA_ERR;
}

int xa_refresh(const char *refresh, char **refresh_new, char **access, xc_err_t *e) {
    *refresh_new = NULL;
    *access = NULL;
    char *rt = xa_urlencode(refresh), *scope = xa_urlencode(XA_SCOPE);
    size_t bl = (rt ? strlen(rt) : 0) + 256;
    char *body = (char *)malloc(bl);
    if (!rt || !scope || !body) {
        xa_secret_free(&rt); free(scope); free(body);
        set_err(e, HTTPS_ETOOBIG, "sem memoria");
        return XA_ERR;
    }
    snprintf(body, bl, "client_id=%s&grant_type=refresh_token&refresh_token=%s&scope=%s",
             XA_CLIENT_ID, rt, scope);
    xa_secret_free(&rt);
    free(scope);
    https_resp_t r;
    int rc = https_request("refresh", "POST", LOGIN_HOST, TOKEN_PATH, H_FORM, body, strlen(body),
                           SMALL_BODY, &r, e);
    xa_secret_free(&body);
    if (rc != 0) return XA_ERR;
    if (r.status < 200 || r.status > 299) {
        int k = oauth_error_kind(&r);
        http_fail(e, "refresh", &r);
        https_free(&r);
        return k == XA_INVALID_GRANT ? XA_INVALID_GRANT : XA_ERR;
    }
    cJSON *j = cJSON_Parse(r.body);
    https_free(&r);
    const char *at = j ? jstr(j, "access_token") : NULL, *nrt = j ? jstr(j, "refresh_token") : NULL;
    if (!at) { cJSON_Delete(j); set_err(e, HTTPS_EPROTO, "refresh sem access_token"); return XA_ERR; }
    *access = xstrdup(at);
    if (nrt) *refresh_new = xstrdup(nrt);
    cJSON_Delete(j);
    return *access ? XA_OK : XA_ERR;
}

/* ------------------------------------------------------------- Xbox Live */

static int post_json(const char *step, const char *host, const char *path, const char *hdr,
                     cJSON *req, cJSON **resp, xc_err_t *e) {
    char *body = cJSON_PrintUnformatted(req);
    if (!body) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XA_ERR; }
    https_resp_t r;
    int rc = https_request(step, "POST", host, path, hdr, body, strlen(body), SMALL_BODY, &r, e);
    xa_secret_free(&body);
    *resp = parse_ok(step, rc, &r, e);
    https_free(&r);
    return *resp ? XA_OK : XA_ERR;
}

int xa_user_authenticate(const char *access, char **user_token, xc_err_t *e) {
    *user_token = NULL;
    size_t tl = strlen(access) + 4;
    char *ticket = (char *)malloc(tl);
    if (!ticket) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XA_ERR; }
    snprintf(ticket, tl, "d=%s", access);
    cJSON *req = cJSON_CreateObject();
    cJSON *props = cJSON_AddObjectToObject(req, "Properties");
    cJSON_AddStringToObject(props, "AuthMethod", "RPS");
    cJSON_AddStringToObject(props, "RpsTicket", ticket);
    cJSON_AddStringToObject(props, "SiteName", "user.auth.xboxlive.com");
    cJSON_AddStringToObject(req, "RelyingParty", "http://auth.xboxlive.com");
    cJSON_AddStringToObject(req, "TokenType", "JWT");
    xa_secret_free(&ticket);
    cJSON *resp = NULL;
    int rc = post_json("user.auth", "user.auth.xboxlive.com", "/user/authenticate", H_XBL, req, &resp, e);
    cJSON_Delete(req);
    if (rc != XA_OK) return XA_ERR;
    const char *t = jstr(resp, "Token");
    if (t) *user_token = xstrdup(t);
    cJSON_Delete(resp);
    if (!*user_token) { set_err(e, HTTPS_EPROTO, "user.auth sem Token"); return XA_ERR; }
    return XA_OK;
}

int xa_xsts_authorize(const char *user_token, const char *relying_party, char **xsts, xc_err_t *e) {
    *xsts = NULL;
    cJSON *req = cJSON_CreateObject();
    cJSON *props = cJSON_AddObjectToObject(req, "Properties");
    cJSON_AddStringToObject(props, "SandboxId", "RETAIL");
    cJSON *ut = cJSON_AddArrayToObject(props, "UserTokens");
    cJSON_AddItemToArray(ut, cJSON_CreateString(user_token));
    cJSON_AddStringToObject(req, "RelyingParty", relying_party);
    cJSON_AddStringToObject(req, "TokenType", "JWT");
    cJSON *resp = NULL;
    int rc = post_json("xsts", "xsts.auth.xboxlive.com", "/xsts/authorize", H_XBL, req, &resp, e);
    cJSON_Delete(req);
    if (rc != XA_OK) return XA_ERR;
    const char *t = jstr(resp, "Token");
    if (t) *xsts = xstrdup(t);
    cJSON_Delete(resp);
    if (!*xsts) { set_err(e, HTTPS_EPROTO, "xsts sem Token"); return XA_ERR; }
    return XA_OK;
}

int xa_streaming_login(const char *offering, const char *gssv_token, xa_endpoint_t *ep, xc_err_t *e) {
    memset(ep, 0, sizeof(*ep));
    char host[96], step[40];
    snprintf(host, sizeof(host), "%s.gssv-play-prod.xboxlive.com", offering);
    snprintf(step, sizeof(step), "login %s", offering);
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "token", gssv_token);
    cJSON_AddStringToObject(req, "offeringId", offering);
    cJSON *resp = NULL;
    int rc = post_json(step, host, "/v2/login/user", H_GSSV, req, &resp, e);
    cJSON_Delete(req);
    if (rc != XA_OK) return XA_ERR;
    const char *tok = jstr(resp, "gsToken");
    const cJSON *os = cJSON_GetObjectItemCaseSensitive(resp, "offeringSettings");
    const cJSON *regions = os ? cJSON_GetObjectItemCaseSensitive(os, "regions") : NULL;
    const cJSON *rg;
    const char *base = NULL;
    cJSON_ArrayForEach(rg, regions) {
        if (jbool(rg, "isDefault")) { base = jstr(rg, "baseUri"); break; }
    }
    /* Some xHome offerings omit isDefault; keep cloud's existing contract. */
    if (!base && !strcmp(offering, "xhome")) {
        cJSON_ArrayForEach(rg, regions) {
            base = jstr(rg, "baseUri");
            if (base && base[0]) break;
        }
    }
    if (!tok || !base) {
        cJSON_Delete(resp);
        set_err(e, HTTPS_EPROTO, "%s sem gsToken/regiao", step);
        return XA_ERR;
    }
    /* baseUri = "https://brs.core.gssv-play-prod.xboxlive.com" */
    const char *h = strstr(base, "://");
    h = h ? h + 3 : base;
    snprintf(ep->host, sizeof(ep->host), "%s", h);
    char *slash = strchr(ep->host, '/');
    if (slash) *slash = 0;
    ep->token = xstrdup(tok);
    cJSON_Delete(resp);
    return ep->token ? XA_OK : XA_ERR;
}

/* -------------------------------------------------------------- catálogo */

static int json_str_in(const cJSON *arr, const char *s) {
    const cJSON *v;
    cJSON_ArrayForEach(v, arr) if (cJSON_IsString(v) && !strcmp(v->valuestring, s)) return 1;
    return 0;
}

int xa_parse_titles(const char *json, xa_title_t **out, int *n) {
    *out = NULL;
    *n = 0;
    cJSON *j = cJSON_Parse(json);
    if (!j) return -1;
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(j, "results");
    int cap = cJSON_GetArraySize(res);
    xa_title_t *t = (xa_title_t *)calloc((size_t)(cap > 0 ? cap : 1), sizeof(xa_title_t));
    if (!t) { cJSON_Delete(j); return -1; }
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, res) {
        const char *tid = jstr(it, "titleId");
        if (!tid || !tid[0] || strlen(tid) >= sizeof(t[0].id)) continue;
        /* dedupe (lista ordenada vem depois; aqui é linear, ~2400 itens) */
        int dup = 0;
        for (int i = 0; i < k; i++) if (!strcmp(t[i].id, tid)) { dup = 1; break; }
        if (dup) continue;
        xa_title_t *g = &t[k];
        snprintf(g->id, sizeof(g->id), "%s", tid);
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(it, "details");
        if (d) {
            const char *pid = jstr(d, "productId");
            if (pid) snprintf(g->product, sizeof(g->product), "%s", pid);
            if (jbool(d, "hasEntitlement")) g->flags |= XA_F_OWNED;
            const cJSON *programs = cJSON_GetObjectItemCaseSensitive(d, "programs");
            const cJSON *subs = cJSON_GetObjectItemCaseSensitive(d, "userSubscriptions");
            const cJSON *p;
            cJSON_ArrayForEach(p, programs) {
                if (!cJSON_IsString(p)) continue;
                const char *nm = p->valuestring;
                if (!strcmp(nm, "F2P")) g->flags |= XA_F_ADSUP;
                else if (strcmp(nm, "BYOG") && strcmp(nm, "EARLYACCESS")) g->flags |= XA_F_SUB;
                if (json_str_in(subs, nm)) g->flags |= XA_F_INSUB;
            }
            if (json_str_in(cJSON_GetObjectItemCaseSensitive(d, "userPrograms"), "F2P"))
                g->flags |= XA_F_ADGRANT;
        }
        k++;
    }
    cJSON_Delete(j);
    *out = t;
    *n = k;
    return 0;
}

int xa_playable(const xa_title_t *t) {
    return (t->flags & (XA_F_OWNED | XA_F_INSUB | XA_F_ADPLAY)) != 0;
}

int xa_merge_f2p(xa_title_t **mainp, int *nmain, const xa_title_t *ads, int nads) {
    int n = *nmain, extra = 0;
    for (int i = 0; i < nads; i++) if (ads[i].flags & (XA_F_OWNED | XA_F_INSUB | XA_F_ADGRANT)) extra++;
    xa_title_t *m = (xa_title_t *)realloc(*mainp, sizeof(xa_title_t) * (size_t)(n + extra + 1));
    if (!m) return -1;
    int base = n;
    for (int i = 0; i < nads; i++) {
        const xa_title_t *a = &ads[i];
        if (!(a->flags & (XA_F_OWNED | XA_F_INSUB | XA_F_ADGRANT))) continue;
        int found = -1;
        for (int k = 0; k < base; k++) if (!strcmp(m[k].id, a->id)) { found = k; break; }
        if (found < 0) {
            m[n] = *a;
            m[n].flags = (a->flags & ~(XA_F_OWNED | XA_F_INSUB)) | XA_F_ADSUP | XA_F_ADPLAY;
            n++;
        } else {
            m[found].flags |= XA_F_ADSUP | XA_F_ADPLAY;
            if (a->flags & XA_F_ADGRANT) m[found].flags |= XA_F_ADGRANT;
            else m[found].flags &= ~(unsigned)XA_F_ADGRANT;
        }
    }
    *mainp = m;
    *nmain = n;
    return 0;
}

static int cmp_title(const void *a, const void *b) {
    const xa_title_t *x = (const xa_title_t *)a, *y = (const xa_title_t *)b;
    const char *nx = x->name[0] ? x->name : x->id, *ny = y->name[0] ? y->name : y->id;
    int c = strcasecmp(nx, ny);
    return c ? c : strcmp(x->id, y->id);
}

int xa_finalize(xa_title_t *t, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) if (xa_playable(&t[i])) t[k++] = t[i];
    qsort(t, (size_t)k, sizeof(xa_title_t), cmp_title);
    return k;
}

/* GET autenticado no host da região que devolve {"results":[título...]}. */
static int fetch_title_list(const xa_endpoint_t *ep, const char *step, const char *path,
                            size_t max_body, xa_title_t **out, int *n, xc_err_t *e) {
    size_t hl = strlen(ep->token) + sizeof(DEVICE_INFO) + 256;
    char *hdr = (char *)malloc(hl);
    if (!hdr) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XA_ERR; }
    snprintf(hdr, hl, "Accept: application/json\r\nContent-Type: application/json\r\n"
             "X-Gssv-Client: XboxComBrowser\r\n" DEVICE_INFO "Authorization: Bearer %s\r\n", ep->token);
    https_resp_t r;
    int rc = https_request(step, "GET", ep->host, path, hdr, NULL, 0, max_body, &r, e);
    xa_secret_free(&hdr);
    if (rc != 0) return XA_ERR;
    if (r.status < 200 || r.status > 299) { http_fail(e, step, &r); https_free(&r); return XA_ERR; }
    rc = xa_parse_titles(r.body, out, n);
    xc_log("[cat] %s %u bytes, %d titulos", path, (unsigned)r.len, rc == 0 ? *n : -1);
    https_free(&r);
    if (rc != 0) { set_err(e, HTTPS_EPROTO, "JSON invalido em %s", step); return XA_ERR; }
    return XA_OK;
}

int xa_fetch_titles(const xa_endpoint_t *ep, xa_title_t **out, int *n, xc_err_t *e) {
    return fetch_title_list(ep, "titles", "/v2/titles", TITLES_BODY, out, n, e);
}

int xa_fetch_mru(const xa_endpoint_t *ep, int max, xa_title_t **out, int *n, xc_err_t *e) {
    char path[48];
    if (max < 1) max = 1;
    if (max > 100) max = 100;
    snprintf(path, sizeof(path), "/v2/titles/mru?mr=%d", max);
    return fetch_title_list(ep, "mru", path, SMALL_BODY * 4, out, n, e);
}

int xa_parse_names(const char *json, xa_title_t *t, int n) {
    cJSON *j = cJSON_Parse(json);
    if (!j) return -1;
    int got = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "Products")) {
        const char *pid = jstr(p, "ProductId");
        const cJSON *lp = cJSON_GetObjectItemCaseSensitive(p, "LocalizedProperties");
        const cJSON *first = cJSON_GetArrayItem(lp, 0);
        const char *title = first ? jstr(first, "ProductTitle") : NULL;
        if (!pid || !title) continue;
        for (int i = 0; i < n; i++) {
            if (t[i].name[0] || strcasecmp(t[i].product, pid)) continue;
            /* v0.20.0: UTF-8 até o desenho (o menu novo tem acentos) */
            xa_name_clean(t[i].name, 61 < sizeof(t[i].name) ? 61 : sizeof(t[i].name), title);
            got++;
        }
    }
    cJSON_Delete(j);
    return got;
}

int xa_fetch_names(xa_title_t *t, int n, void (*progress)(int, int), xc_err_t *e) {
    int *idx = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    if (!idx) return 0;
    /* v0.20.0: nomes em pt-BR (o mesmo mercado das capas, com acento); o
     * que o BR não tiver vai de novo em en-US. */
    static const char *const k_mkt[2] = { "market=BR&languages=pt-BR", "market=US&languages=en-US" };
    int resolved = 0;
    for (int pass = 0; pass < 2; pass++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (!xa_playable(&t[i]) || t[i].name[0] || !t[i].product[0]) continue;
            int dup = 0;   /* vários títulos podem ter o mesmo produto */
            for (int k = 0; k < m; k++) if (!strcmp(t[idx[k]].product, t[i].product)) { dup = 1; break; }
            if (!dup) idx[m++] = i;
        }
        if (!m) break;
        if (pass) xc_log("[cat] nomes: %d sem nome em pt-BR, tentando en-US", m);
        for (int s = 0; s < m; s += NAMES_BATCH) {
            if (xcn_cancelled()) break;
            char path[96 + NAMES_BATCH * 24];
            int pl = snprintf(path, sizeof(path), "/v7.0/products?bigIds=");
            for (int k = s; k < m && k < s + NAMES_BATCH; k++)
                pl += snprintf(path + pl, sizeof(path) - (size_t)pl, "%s%s", k > s ? "," : "", t[idx[k]].product);
            snprintf(path + pl, sizeof(path) - (size_t)pl, "&%s&fieldsTemplate=Browse", k_mkt[pass]);
            https_resp_t r;
            xc_err_t e2;
            int rc = https_request("nomes", "GET", "displaycatalog.mp.microsoft.com", path,
                                   "Accept: application/json\r\n", NULL, 0, NAMES_BODY, &r, &e2);
            if (rc == 0 && r.status == 200) {
                int g = xa_parse_names(r.body, t, n);
                if (g > 0) resolved += g;
            } else if (rc != 0) {
                xc_log("[cat] nomes: %s", e2.msg);
                *e = e2;
            } else {
                xc_log("[cat] nomes: HTTP %d", r.status);
            }
            https_free(&r);
            if (progress) progress(s + NAMES_BATCH < m ? s + NAMES_BATCH : m, m);
        }
        if (xcn_cancelled()) break;
    }
    free(idx);
    return resolved;
}

/* ----------------------------------------------------------- nome ASCII */

/* U+00C0..U+00FF -> letra base (0 = some), como NFKD + ignore do launcher. */
static const char k_latin1[64] = {
    'A','A','A','A','A','A', 0 ,'C','E','E','E','E','I','I','I','I',
     0 ,'N','O','O','O','O','O', 0 , 0 ,'U','U','U','U','Y', 0 , 0 ,
    'a','a','a','a','a','a', 0 ,'c','e','e','e','e','i','i','i','i',
     0 ,'n','o','o','o','o','o', 0 , 0 ,'u','u','u','u','y', 0 ,'y'
};

void xa_ascii_fold(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    char tmp[256];
    const unsigned char *s = (const unsigned char *)src;
    while (*s && o + 1 < sizeof(tmp)) {
        unsigned c = *s;
        if (c < 0x80) {
            tmp[o++] = (c < 32 || c == 127) ? ' ' : (char)c;
            s++;
            continue;
        }
        unsigned cp = 0, len = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { s++; continue; }
        unsigned i;
        for (i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80) break;
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        s += i;
        if (i < len) continue;
        if (cp >= 0xC0 && cp <= 0xFF) { if (k_latin1[cp - 0xC0]) tmp[o++] = k_latin1[cp - 0xC0]; }
        else if (cp == 0xA0 || cp == 0x2002 || cp == 0x2003 || cp == 0x2009) tmp[o++] = ' ';
        else if (cp == 0xB2) tmp[o++] = '2';
        else if (cp == 0xB3) tmp[o++] = '3';
        else if (cp == 0xB9) tmp[o++] = '1';
        else if (cp == 0x2024) tmp[o++] = '.';
        /* resto (TM, R, C, aspas curvas, travessões, CJK...) some */
    }
    tmp[o] = 0;
    /* espaços: colapsa e apara */
    size_t w = 0;
    for (size_t r = 0; tmp[r]; r++) {
        if (tmp[r] == ' ' && (w == 0 || tmp[w - 1] == ' ')) continue;
        tmp[w++] = tmp[r];
    }
    while (w > 0 && tmp[w - 1] == ' ') w--;
    tmp[w] = 0;
    if (cap == 0) return;
    if (w + 1 > cap) {   /* corta com "..." (cap-1 visíveis) */
        size_t keep = cap > 4 ? cap - 4 : 0;
        while (keep > 0 && tmp[keep - 1] == ' ') keep--;
        memcpy(dst, tmp, keep);
        if (cap > 4) { memcpy(dst + keep, "...", 3); keep += 3; }
        dst[keep] = 0;
    } else {
        memcpy(dst, tmp, w + 1);
    }
}

/* ----------------------------------------------------- nome UTF-8 */

/* v0.20.0: nome de jogo pro menu novo. UTF-8 válido passa (acentos etc.);
 * sequência inválida some; controle e espaços especiais viram espaço;
 * (TM), (R) e (C) somem (como no ASCII de antes); espaços colapsados e
 * aparados; corta em cap-1 bytes com "..." sem partir um caractere. */
void xa_name_clean(char *dst, size_t cap, const char *src) {
    char tmp[256];
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)(src ? src : "");
    while (*s && o + 5 < sizeof(tmp)) {
        unsigned c = *s;
        if (c < 0x80) {
            tmp[o++] = (c < 32 || c == 127) ? ' ' : (char)c;
            s++;
            continue;
        }
        unsigned cp = 0, len = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { s++; continue; }
        unsigned i;
        for (i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80) break;
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        if (i < len) { s += i; continue; }
        /* forma longa / surrogate / fora do Unicode = inválido */
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) { s += len; continue; }
        if (cp == 0xA0 || cp == 0x2002 || cp == 0x2003 || cp == 0x2009 || (cp >= 0x80 && cp < 0xA0)) tmp[o++] = ' ';
        else if (cp != 0x2122 && cp != 0xAE && cp != 0xA9)
            for (unsigned k = 0; k < len; k++) tmp[o++] = (char)s[k];
        s += len;
    }
    tmp[o] = 0;
    size_t w = 0;
    for (size_t r = 0; tmp[r]; r++) {
        if (tmp[r] == ' ' && (w == 0 || tmp[w - 1] == ' ')) continue;
        tmp[w++] = tmp[r];
    }
    while (w > 0 && tmp[w - 1] == ' ') w--;
    tmp[w] = 0;
    if (cap == 0) return;
    if (w + 1 > cap) {   /* corta com "..." (cap-1 bytes no total) */
        size_t keep = cap > 4 ? cap - 4 : 0;
        while (keep > 0 && ((unsigned char)tmp[keep] & 0xC0) == 0x80) keep--;   /* fronteira de caractere */
        while (keep > 0 && tmp[keep - 1] == ' ') keep--;
        memcpy(dst, tmp, keep);
        if (cap > 4) { memcpy(dst + keep, "...", 3); keep += 3; }
        dst[keep] = 0;
    } else {
        memcpy(dst, tmp, w + 1);
    }
}

/* ---------------------------------------------------------- cache nomes */

int xa_names_from_cache(const char *txt, xa_title_t *t, int n) {
    int got = 0;
    /* v0.20.0: o NAMES.TXT antigo (sem cabeçalho) tem os nomes em ASCII:
     * ignora inteiro, os nomes são buscados de novo (com acento) e o
     * arquivo é regravado no formato novo. */
    size_t hl = strlen(XA_NAMES_MAGIC);
    if (!txt || strncmp(txt, XA_NAMES_MAGIC, hl) || (txt[hl] != '\n' && txt[hl] != '\r')) return 0;
    const char *line = strchr(txt, '\n');
    line = line ? line + 1 : NULL;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        const char *tab = memchr(line, '\t', len);
        if (tab) {
            size_t pl = (size_t)(tab - line);
            size_t nlen = len - pl - 1;
            if (nlen > 0 && line[pl + 1 + nlen - 1] == '\r') nlen--;
            for (int i = 0; i < n; i++) {
                if (t[i].name[0] || strlen(t[i].product) != pl || strncmp(t[i].product, line, pl)) continue;
                size_t c = nlen < sizeof(t[i].name) - 1 ? nlen : sizeof(t[i].name) - 1;
                memcpy(t[i].name, tab + 1, c);
                t[i].name[c] = 0;
                got++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    return got;
}

char *xa_names_to_cache(const xa_title_t *t, int n) {
    size_t cap = (size_t)n * (sizeof(t[0].product) + sizeof(t[0].name) + 2) + sizeof(XA_NAMES_MAGIC) + 2;
    char *o = (char *)malloc(cap);
    if (!o) return NULL;
    size_t w = (size_t)snprintf(o, cap, "%s\n", XA_NAMES_MAGIC);
    for (int i = 0; i < n; i++) {
        if (!t[i].name[0] || !t[i].product[0]) continue;
        int dup = 0;
        for (int k = 0; k < i; k++) if (t[k].name[0] && !strcmp(t[k].product, t[i].product)) { dup = 1; break; }
        if (dup) continue;
        w += (size_t)snprintf(o + w, cap - w, "%s\t%s\n", t[i].product, t[i].name);
    }
    o[w] = 0;
    return o;
}
