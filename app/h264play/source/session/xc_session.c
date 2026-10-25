/* XCloud360 stage 3 — ver xc_session.h. Porte de green-nx/src/core/session.cpp
 * e auth.cpp:fetch_passport_token (GPL-3.0). */
#include "xc_session.h"
#include "../login/xc_port.h"
#include "../login/cjson/cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMALL_BODY (256 * 1024)
#define TOKEN_MAX  (16 * 1024)

#define PASSPORT_HOST  "login.live.com"
#define PASSPORT_PATH  "/oauth20_token.srf"
/* Escopo literal como o green-nx manda (auth.cpp, sem urlencode). */
#define PASSPORT_SCOPE "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN"
#define H_FORM "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n"

/* X-MS-Device-Info do GssvSession::device_info_header(P720) EXATAMENTE como
 * o nlohmann::json serializa (chaves em ordem alfabética). Conferido byte a
 * byte pelo hosttest/session (golden.h, gerado do próprio json.hpp). */
#define XS_DEVICE_INFO \
    "X-MS-Device-Info: {\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\"," \
    "\"clientAppType\":\"browser\",\"clientAppVersion\":\"26.1.97\"," \
    "\"clientSdkVersion\":\"10.3.7\",\"httpEnvironment\":\"prod\",\"sdkInstallId\":\"\"}}," \
    "\"dev\":{\"browser\":{\"browserName\":\"chrome\",\"browserVersion\":\"140.0.3485.54\"}," \
    "\"displayInfo\":{\"dimensions\":{\"heightInPixels\":720,\"widthInPixels\":1280}," \
    "\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}},\"hw\":{\"make\":\"Microsoft\"," \
    "\"model\":\"unknown\",\"sdktype\":\"web\"},\"os\":{\"name\":\"android\"," \
    "\"platform\":\"desktop\",\"ver\":\"22631.2715\"}}}"

/* Mesma ordem do GssvSession::headers(). */
#define XS_HDR_FMT \
    "Accept: application/json\r\nContent-Type: application/json\r\n" \
    "X-Gssv-Client: XboxComBrowser\r\n" XS_DEVICE_INFO "\r\nAuthorization: Bearer %s\r\n"

/* ------------------------------------------------------------------ util */

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
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

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

/* HTTP != 2xx -> "HTTP 400 em play (Code)" com code/error/XErr do corpo
 * (corpos de erro do GSSV/MS não têm token; mesmo assim só o código). */
static void http_fail(xc_err_t *e, const char *step, const https_resp_t *r) {
    cJSON *j = cJSON_Parse(r->body ? r->body : "");
    const char *kind = NULL;
    if (j) {
        kind = jstr(j, "code");
        if (!kind) kind = jstr(j, "error");
        if (!kind) {
            const cJSON *ed = cJSON_GetObjectItemCaseSensitive(j, "errorDetails");
            if (cJSON_IsObject(ed)) kind = jstr(ed, "code");
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

static int map_transport(int rc) {
    if (rc == HTTPS_ETIMEOUT) return XS_ETIMEOUT;
    if (rc == HTTPS_ECANCEL) return XS_ECANCEL;
    return XS_ENET;
}

static int is_2xx(int s) { return s >= 200 && s <= 299; }

static int body_empty(const https_resp_t *r) {
    if (!r->body) return 1;
    for (const char *p = r->body; *p; p++)
        if (*p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') return 0;
    return 1;
}

/* ------------------------------------------------------------ credenciais */

void xs_creds_init(xs_creds_t *c, const char *offering) {
    memset(c, 0, sizeof(*c));
    snprintf(c->offering, sizeof(c->offering), "%s", offering && offering[0] ? offering : "xgpuweb");
}

void xs_creds_free(xs_creds_t *c) {
    if (c->conn) { https_conn_free(c->conn); c->conn = NULL; }
    xa_secret_free(&c->gs_token);
    xa_secret_free(&c->refresh);
    xa_secret_free(&c->passport);
}

int xs_creds_set_refresh(xs_creds_t *c, const char *refresh) {
    xa_secret_free(&c->refresh);
    c->refresh = refresh ? xstrdup(refresh) : NULL;
    return c->refresh ? XS_OK : XS_ELOGIN;
}

int xs_creds_load(xs_creds_t *c, xc_err_t *e) {
    char *buf = (char *)malloc(TOKEN_MAX);
    if (!buf) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
    int n = xc_store_load(XS_TOKEN_FILE, buf, TOKEN_MAX - 1);
    if (n <= 0) {
        free(buf);
        set_err(e, HTTPS_EHTTP, "sem login salvo (TOKEN.TXT)");
        return XS_ELOGIN;
    }
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
    if (n < 20) {
        xa_secret_free(&buf);
        set_err(e, HTTPS_EHTTP, "TOKEN.TXT invalido");
        return XS_ELOGIN;
    }
    xa_secret_free(&c->refresh);
    c->refresh = buf;
    return XS_OK;
}

int xs_creds_login(xs_creds_t *c, xc_err_t *e) {
    if (!c->refresh) { set_err(e, HTTPS_EHTTP, "sem login salvo"); return XS_ELOGIN; }
    char *rnew = NULL, *access = NULL;
    int r = xa_refresh(c->refresh, &rnew, &access, e);
    if (r == XA_INVALID_GRANT) { xa_secret_free(&rnew); return XS_ELOGIN; }
    if (r != XA_OK) { xa_secret_free(&rnew); xa_secret_free(&access); return XS_ERR; }
    if (rnew && strcmp(rnew, c->refresh) != 0) {
        /* rotacionou: igual ao login_flow.c, o novo vai pro pendrive */
        if (!c->no_store) {
            if (xc_store_save(XS_TOKEN_FILE, rnew, strlen(rnew)) == 0)
                xc_log("[sess] refresh token rotacionado e salvo");
            else
                xc_log("[sess] AVISO: nao consegui salvar o refresh rotacionado");
        }
        xa_secret_free(&c->refresh);
        c->refresh = rnew;
        rnew = NULL;
    }
    xa_secret_free(&rnew);

    char *user = NULL, *gssv = NULL;
    r = xa_user_authenticate(access, &user, e);
    xa_secret_free(&access);
    if (r != XA_OK) return XS_ERR;
    r = xa_xsts_authorize(user, "http://gssv.xboxlive.com/", &gssv, e);
    xa_secret_free(&user);
    if (r != XA_OK) return XS_ERR;
    xa_endpoint_t ep;
    r = xa_streaming_login(c->offering, gssv, &ep, e);
    xa_secret_free(&gssv);
    if (r != XA_OK) return XS_ERR;
    xa_secret_free(&c->gs_token);
    c->gs_token = ep.token;          /* passa a ser nosso */
    ep.token = NULL;
    snprintf(c->host, sizeof(c->host), "%s", ep.host);
    xa_secret_free(&c->passport);    /* vinha do refresh anterior */
    xc_log("[sess] credenciais %s ok, regiao %s", c->offering, c->host);
    return XS_OK;
}

int xs_creds_passport(xs_creds_t *c, xc_err_t *e) {
    xa_secret_free(&c->passport);
    if (!c->refresh) { set_err(e, HTTPS_EHTTP, "sem login salvo"); return XS_ELOGIN; }
    char *rt = xa_urlencode(c->refresh);
    if (!rt) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
    size_t bl = strlen(rt) + sizeof(PASSPORT_SCOPE) + 128;
    char *body = (char *)malloc(bl);
    if (!body) { xa_secret_free(&rt); set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
    /* mesma ordem de campos do green-nx */
    snprintf(body, bl, "client_id=%s&scope=%s&grant_type=refresh_token&refresh_token=%s",
             XA_CLIENT_ID, PASSPORT_SCOPE, rt);
    xa_secret_free(&rt);
    https_resp_t r;
    int rc = https_request("passport", "POST", PASSPORT_HOST, PASSPORT_PATH, H_FORM, body,
                           strlen(body), SMALL_BODY, &r, e);
    xa_secret_free(&body);
    if (rc != 0) return map_transport(rc);
    if (!is_2xx(r.status)) {
        http_fail(e, "passport", &r);
        int invalid = r.body && strstr(r.body, "invalid_grant") != NULL;
        https_free(&r);
        return invalid ? XS_ELOGIN : XS_ERR;
    }
    cJSON *j = cJSON_Parse(r.body);
    https_free(&r);
    const char *at = j ? jstr(j, "access_token") : NULL;
    /* (um refresh_token que venha aqui é ignorado, como no green-nx) */
    if (at) c->passport = xstrdup(at);
    cJSON_Delete(j);
    if (!c->passport) { set_err(e, HTTPS_EPROTO, "passport sem access_token"); return XS_ERR; }
    return XS_OK;
}

/* ------------------------------------------------------- pedido ao GSSV */

char *xs_build_headers(const char *gs_token) {
    size_t n = sizeof(XS_HDR_FMT) + strlen(gs_token) + 8;
    char *h = (char *)malloc(n);
    if (h) snprintf(h, n, XS_HDR_FMT, gs_token);
    return h;
}

/* Um pedido com os cabeçalhos do GSSV. XS_OK = houve resposta HTTP (r
 * válido, qualquer status menos 401). 401: renova o login uma vez
 * (xs_creds_login) e repete; se ainda 401 -> XS_EAUTH (r já liberado). */
static int gssv_req(xs_creds_t *c, const char *step, const char *method, const char *path,
                    const char *body, https_resp_t *r, xc_err_t *e) {
    memset(r, 0, sizeof(*r));
    for (int pass = 0; pass < 2; pass++) {
        if (!c->gs_token || !c->host[0]) { set_err(e, HTTPS_EHTTP, "sem credenciais"); return XS_EAUTH; }
        char *hdr = xs_build_headers(c->gs_token);
        if (!hdr) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
        /* v0.19.2: keep-alive com o host da região (um handshake por sessão
         * em vez de um por pedido). Região nova (re-login) = conexão nova. */
        if (c->conn && strcmp(https_conn_host(c->conn), c->host) != 0) { https_conn_free(c->conn); c->conn = NULL; }
        if (!c->conn) c->conn = https_conn_new(c->host);
        int rc = c->conn ? https_conn_request(c->conn, step, method, path, hdr, body, body ? strlen(body) : 0,
                                              SMALL_BODY, r, e)
                         : https_request(step, method, c->host, path, hdr, body, body ? strlen(body) : 0,
                                         SMALL_BODY, r, e);
        xa_secret_free(&hdr);
        if (rc != 0) return map_transport(rc);
        if (r->status != 401) return XS_OK;
        http_fail(e, step, r);
        https_free(r);
        if (pass > 0 || !c->refresh || c->reauths >= XS_MAX_REAUTH) return XS_EAUTH;
        c->reauths++;
        xc_log("[sess] %s: HTTP 401, renovando o login (%d/%d)", step, c->reauths, XS_MAX_REAUTH);
        xc_err_t e2;
        memset(&e2, 0, sizeof(e2));
        int lr = xs_creds_login(c, &e2);
        if (lr != XS_OK) { *e = e2; return lr == XS_ELOGIN ? XS_ELOGIN : XS_EAUTH; }
    }
    return XS_EAUTH;
}

static int sess_path(const xs_session_t *s, const char *suffix, char *out, size_t cap, xc_err_t *e) {
    if (!s->path[0]) { set_err(e, HTTPS_EHTTP, "sem sessao"); return -1; }
    snprintf(out, cap, "%s%s", s->path, suffix);
    return 0;
}

/* ------------------------------------------------------------ builders */

char *xs_build_play_body(const char *title_id, const char *locale) {
    /* chaves em ordem alfabética = o que o nlohmann::json do green-nx manda */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "clientSessionId", "");
    cJSON_AddArrayToObject(o, "fallbackRegionNames");
    cJSON_AddStringToObject(o, "serverId", "");
    cJSON *st = cJSON_AddObjectToObject(o, "settings");
    cJSON_AddFalseToObject(st, "enableOptionalDataCollection");
    cJSON_AddFalseToObject(st, "enableTextToSpeech");
    cJSON_AddNumberToObject(st, "highContrast", 0);
    cJSON_AddStringToObject(st, "locale", locale && locale[0] ? locale : XS_LOCALE);
    cJSON_AddStringToObject(st, "nanoVersion", "V3;WebrtcTransport.dll");
    cJSON_AddStringToObject(st, "osName", "android");
    cJSON_AddStringToObject(st, "sdkType", "web");
    cJSON_AddNumberToObject(st, "timezoneOffsetMinutes", 120);
    cJSON_AddFalseToObject(st, "useIceConnection");
    cJSON_AddStringToObject(o, "systemUpdateGroup", "");
    cJSON_AddStringToObject(o, "titleId", title_id);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

char *xs_build_connect_body(const char *passport) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "userToken", passport);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static void add_ver(cJSON *cfg, const char *k, int maxv, int minv) {
    cJSON *v = cJSON_AddObjectToObject(cfg, k);
    cJSON_AddNumberToObject(v, "maxVersion", maxv);
    cJSON_AddNumberToObject(v, "minVersion", minv);
}

char *xs_build_sdp_body(const char *offer_sdp) {
    cJSON *o = cJSON_CreateObject();
    cJSON *cfg = cJSON_AddObjectToObject(o, "configuration");
    add_ver(cfg, "chat", 1, 1);
    cJSON *cc = cJSON_AddObjectToObject(cfg, "chatConfiguration");
    cJSON_AddNumberToObject(cc, "bytesPerSample", 2);
    cJSON_AddNumberToObject(cc, "expectedClipDurationMs", 20);
    cJSON *fmt = cJSON_AddObjectToObject(cc, "format");
    cJSON_AddStringToObject(fmt, "codec", "opus");
    cJSON_AddStringToObject(fmt, "container", "webm");
    cJSON_AddNumberToObject(cc, "numChannels", 1);
    cJSON_AddNumberToObject(cc, "sampleFrequencyHz", 24000);
    add_ver(cfg, "control", 3, 1);
    add_ver(cfg, "input", 9, 1);
    add_ver(cfg, "message", 1, 1);
    add_ver(cfg, "reliableinput", 9, 9);
    add_ver(cfg, "unreliableinput", 9, 9);
    cJSON_AddStringToObject(o, "messageType", "offer");
    cJSON_AddStringToObject(o, "requestId", "1");
    cJSON_AddStringToObject(o, "sdp", offer_sdp);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

/* Cada item é o JSON *stringificado* de {candidate, sdpMLineIndex, sdpMid,
 * usernameFragment} (formato do web client/greenlight; errar isto faz o
 * xCloud segurar o candidato Teredo dele), e a lista termina com
 * "a=end-of-candidates". */
static int add_ice_item(cJSON *list, const char *cand, const char *ufrag) {
    cJSON *it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "candidate", cand);
    cJSON_AddNumberToObject(it, "sdpMLineIndex", 0);
    cJSON_AddStringToObject(it, "sdpMid", "0");
    cJSON_AddStringToObject(it, "usernameFragment", ufrag ? ufrag : "");
    char *s = cJSON_PrintUnformatted(it);
    cJSON_Delete(it);
    if (!s) return -1;
    cJSON_AddItemToArray(list, cJSON_CreateString(s));
    cJSON_free(s);
    return 0;
}

char *xs_build_ice_body(const char *const *cands, int n, const char *ufrag) {
    cJSON *o = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(o, "candidate");
    for (int i = 0; i < n; i++) add_ice_item(list, cands[i], ufrag);
    add_ice_item(list, "a=end-of-candidates", ufrag);
    cJSON_AddStringToObject(o, "messageType", "iceCandidate");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

/* -------------------------------------------------------------- parsers */

static void norm_path(char *out, size_t cap, const char *p) {
    snprintf(out, cap, "%s%s", p[0] == '/' ? "" : "/", p);
}

int xs_parse_play(const char *json, char *path, size_t cap) {
    cJSON *j = cJSON_Parse(json ? json : "");
    const char *p = j ? jstr(j, "sessionPath") : NULL;
    int ok = p && p[0] && strlen(p) + 2 <= cap;
    if (ok) norm_path(path, cap, p);
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

/* Fila: como o queue_info() do launcher — procura em qualquer nível um
 * número cuja chave tenha "position"/"placeinqueue", e o tempo em
 * estimatedTotalWaitTimeInSeconds (ou qualquer "*wait*second*"). */
static int has_ci(const char *k, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = k; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && (p[i] | 0x20) == needle[i]) i++;
        if (i == nl) return 1;
    }
    return 0;
}

static int eq_ci(const char *a, const char *b) {
    while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void walk_queue(const cJSON *o, int *pos, int *eta, int *eta_any) {
    const cJSON *v;
    cJSON_ArrayForEach(v, o) {
        if (cJSON_IsNumber(v) && v->string) {
            if (*pos < 0 && (has_ci(v->string, "position") || has_ci(v->string, "placeinqueue")))
                *pos = v->valueint;
            else if (eq_ci(v->string, "estimatedtotalwaittimeinseconds"))
                *eta = v->valueint;
            else if (*eta_any < 0 && has_ci(v->string, "wait") && has_ci(v->string, "second"))
                *eta_any = v->valueint;
        } else if (cJSON_IsObject(v) || cJSON_IsArray(v)) {
            walk_queue(v, pos, eta, eta_any);
        }
    }
}

int xs_parse_state(const char *json, xs_status_t *st) {
    memset(st, 0, sizeof(*st));
    st->queue_pos = st->wait_s = -1;
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) { st->st = XS_SS_UNKNOWN; return -1; }
    const char *s = jstr(j, "state");
    snprintf(st->raw, sizeof(st->raw), "%s", s ? s : "");
    if (!s) st->st = XS_SS_UNKNOWN;
    else if (!strcmp(s, "New")) st->st = XS_SS_NEW;
    else if (!strcmp(s, "Provisioning")) st->st = XS_SS_PROVISIONING;
    else if (!strcmp(s, "WaitingForResources")) st->st = XS_SS_WAITING;
    else if (!strcmp(s, "ReadyToConnect")) st->st = XS_SS_READY;
    else if (!strcmp(s, "Provisioned")) st->st = XS_SS_PROVISIONED;
    else if (!strcmp(s, "Failed") || !strcmp(s, "Error")) st->st = XS_SS_FAILED;
    else st->st = XS_SS_UNKNOWN;
    if (st->st == XS_SS_FAILED) {
        const cJSON *ed = cJSON_GetObjectItemCaseSensitive(j, "errorDetails");
        if (cJSON_IsObject(ed)) {
            const char *c = jstr(ed, "code"), *m = jstr(ed, "message");
            if (c) xa_ascii_fold(st->err_code, sizeof(st->err_code), c);
            if (m) xa_ascii_fold(st->err_msg, sizeof(st->err_msg), m);
        } else if (cJSON_IsString(ed)) {
            xa_ascii_fold(st->err_msg, sizeof(st->err_msg), ed->valuestring);
        }
    }
    int eta = -1, eta_any = -1;
    walk_queue(j, &st->queue_pos, &eta, &eta_any);
    st->wait_s = eta >= 0 ? eta : eta_any;
    cJSON_Delete(j);
    return 0;
}

static int exchange_error_obj(const cJSON *j, char *msg, size_t cap) {
    const cJSON *ed = cJSON_GetObjectItemCaseSensitive(j, "errorDetails");
    if (!ed || cJSON_IsNull(ed)) return 0;
    if (cJSON_IsObject(ed)) {
        /* consoles/xCloud mandam errorDetails com tudo null em resposta boa */
        int real = 0;
        const cJSON *v;
        cJSON_ArrayForEach(v, ed) if (!cJSON_IsNull(v)) { real = 1; break; }
        if (!real) return 0;
        if (msg && cap) {
            const char *c = jstr(ed, "code"), *m = jstr(ed, "message");
            char a[48] = "", b[96] = "";
            if (c) xa_ascii_fold(a, sizeof(a), c);
            if (m) xa_ascii_fold(b, sizeof(b), m);
            snprintf(msg, cap, "%s%s%s", a, a[0] && b[0] ? ": " : "", b);
        }
        return 1;
    }
    if (msg && cap) snprintf(msg, cap, "errorDetails");
    return 1;
}

int xs_exchange_error(const char *json, char *msg, size_t cap) {
    if (msg && cap) msg[0] = 0;
    cJSON *j = cJSON_Parse(json ? json : "");
    int r = j ? exchange_error_obj(j, msg, cap) : 0;
    cJSON_Delete(j);
    return r;
}

int xs_parse_sdp_answer(const char *json, char **sdp, char *err, size_t cap) {
    *sdp = NULL;
    if (err && cap) err[0] = 0;
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) { if (err) snprintf(err, cap, "JSON invalido"); return XS_ERR; }
    if (exchange_error_obj(j, err, cap)) { cJSON_Delete(j); return XS_EFAILED; }
    const char *ex = jstr(j, "exchangeResponse");
    cJSON *a = ex ? cJSON_Parse(ex) : NULL;
    const char *s = a ? jstr(a, "sdp") : NULL;
    if (s) *sdp = xstrdup(s);
    else if (err) snprintf(err, cap, "answer sem sdp");
    cJSON_Delete(a);
    cJSON_Delete(j);
    return *sdp ? XS_OK : XS_ERR;
}

int xs_teredo_decode(const char *addr, char *ipv4, size_t cap, int *port) {
    char g[8][8];
    int ng = 0;
    const char *p = addr;
    for (;;) {
        const char *c = strchr(p, ':');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (ng >= 8 || l >= sizeof(g[0])) return 0;
        memcpy(g[ng], p, l);
        g[ng][l] = 0;
        ng++;
        if (!c) break;
        p = c + 1;
    }
    if (ng != 8 || strcmp(g[0], "2001") != 0) return 0;   /* Teredo 2001:0000::/32 */
    char iphex[16];
    snprintf(iphex, sizeof(iphex), "%s%s", g[6], g[7]);
    if (strlen(iphex) != 8) return 0;
    unsigned pf = (unsigned)strtoul(g[5], NULL, 16);
    unsigned b[4];
    for (int i = 0; i < 4; i++) {
        char h[3] = { iphex[i * 2], iphex[i * 2 + 1], 0 };
        b[i] = (~(unsigned)strtoul(h, NULL, 16)) & 0xFF;
    }
    *port = (int)((~pf) & 0xFFFF);
    snprintf(ipv4, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return 1;
}

static void push_cand(xs_cands_t *out, const char *s) {
    if (out->n >= XS_MAX_CANDS || strlen(s) >= XS_CAND_LEN) return;
    snprintf(out->c[out->n++], XS_CAND_LEN, "%s", s);
}

/* = GssvSession::receive_ice_candidates */
static void ice_item(xs_cands_t *out, const char *cand_in, int *foundation) {
    char cand[XS_CAND_LEN];
    if (!strncmp(cand_in, "a=", 2)) cand_in += 2;
    if (strstr(cand_in, "end-of-candidates")) { out->end = 1; return; }
    if (strncmp(cand_in, "candidate:", 10) != 0 || strlen(cand_in) >= sizeof(cand)) return;
    snprintf(cand, sizeof(cand), "%s", cand_in);
    size_t n = strlen(cand);
    while (n > 0 && (cand[n - 1] == ' ' || cand[n - 1] == '\r')) cand[--n] = 0;
    /* candidate:<foundation> <comp> <proto> <prio> <addr> <port> ... */
    char tmp[XS_CAND_LEN], *parts[8];
    int np = 0;
    snprintf(tmp, sizeof(tmp), "%s", cand);
    for (char *t = tmp; *t && np < 8;) {   /* split por espaço (sem strtok: reentrante) */
        while (*t == ' ' || *t == '\t') *t++ = 0;
        if (!*t) break;
        parts[np++] = t;
        while (*t && *t != ' ' && *t != '\t') t++;
    }
    if (np < 6) return;
    const char *addr = parts[4];
    if (!strchr(addr, ':')) { push_cand(out, cand); return; }   /* IPv4 */
    char ip[20];
    int tport = 0;
    if (!xs_teredo_decode(addr, ip, sizeof(ip), &tport)) return;  /* IPv6 que não é Teredo */
    /* o xCloud alcança este nó na porta decodificada e na UDP 9002 */
    int ports[2] = { tport, 9002 };
    for (int i = 0; i < 2; i++) {
        char line[96];
        snprintf(line, sizeof(line), "candidate:%d 1 UDP 1 %s %d typ host", (*foundation)++, ip, ports[i]);
        push_cand(out, line);
    }
}

int xs_parse_ice(const char *json, xs_cands_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) return XS_ERR;
    if (exchange_error_obj(j, NULL, 0)) { cJSON_Delete(j); return XS_EFAILED; }
    const char *ex = jstr(j, "exchangeResponse");
    cJSON *p = ex ? cJSON_Parse(ex) : NULL;
    cJSON_Delete(j);
    if (!p) return XS_OK;   /* nada utilizável (green-nx devolve vazio) */
    const cJSON *items = cJSON_IsArray(p) ? p : cJSON_GetObjectItemCaseSensitive(p, "candidates");
    int foundation = 20;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        cJSON *owned = NULL;
        const cJSON *entry = it;
        if (cJSON_IsString(it)) entry = owned = cJSON_Parse(it->valuestring);
        if (cJSON_IsObject(entry)) {
            const char *c = jstr(entry, "candidate");
            if (c) ice_item(out, c, &foundation);
        }
        cJSON_Delete(owned);
    }
    cJSON_Delete(p);
    return XS_OK;
}

/* = cleanup_stale_sessions: qualquer "sessionPath"/"path" string em qualquer
 * nível; titleId/state do mesmo objeto se houver. */
static void walk_active(const cJSON *o, xs_active_t *out, int max, int *n) {
    if (cJSON_IsObject(o)) {
        const char *p = jstr(o, "sessionPath");
        if (!p) p = jstr(o, "path");
        if (p && p[0] && *n < max) {
            char np[sizeof(out[0].path)];
            norm_path(np, sizeof(np), p);
            int dup = 0;
            for (int i = 0; i < *n; i++) if (!strcmp(out[i].path, np)) { dup = 1; break; }
            if (!dup) {
                xs_active_t *a = &out[(*n)++];
                memset(a, 0, sizeof(*a));
                snprintf(a->path, sizeof(a->path), "%s", np);
                const char *t = jstr(o, "titleId"), *s = jstr(o, "state");
                if (t) snprintf(a->title, sizeof(a->title), "%s", t);
                if (s) snprintf(a->state, sizeof(a->state), "%s", s);
            }
        }
    }
    const cJSON *v;
    if (cJSON_IsObject(o) || cJSON_IsArray(o)) cJSON_ArrayForEach(v, o) walk_active(v, out, max, n);
}

int xs_parse_active(const char *json, xs_active_t *out, int max) {
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) return -1;
    int n = 0;
    walk_active(j, out, max, &n);
    cJSON_Delete(j);
    return n;
}

int xs_parse_wait_time(const char *json) {
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) return -1;
    int r = -1;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "estimatedTotalWaitTimeInSeconds");
    if (cJSON_IsNumber(t)) r = t->valueint;
    else {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "estimatedAllocationTimeInSeconds");
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "estimatedProvisioningTimeInSeconds");
        if (cJSON_IsNumber(a) || cJSON_IsNumber(p))
            r = (cJSON_IsNumber(a) ? a->valueint : 0) + (cJSON_IsNumber(p) ? p->valueint : 0);
    }
    cJSON_Delete(j);
    return r;
}

void xs_sdp_ufrag(const char *sdp, char *out, size_t cap) {
    const char *p = sdp ? strstr(sdp, "a=ice-ufrag:") : NULL;
    if (!cap) return;
    out[0] = 0;
    if (!p) return;
    p += 12;
    size_t l = strcspn(p, "\r\n");
    if (l >= cap) l = cap - 1;
    memcpy(out, p, l);
    out[l] = 0;
}

/* ---------------------------------------------------------------- sessão */

void xs_init(xs_session_t *s, xs_creds_t *c) {
    memset(s, 0, sizeof(*s));
    s->creds = c;
    snprintf(s->locale, sizeof(s->locale), "%s", XS_LOCALE);
}

void xs_attach(xs_session_t *s, const char *path, const char *title_id) {
    norm_path(s->path, sizeof(s->path), path);
    snprintf(s->title, sizeof(s->title), "%s", title_id ? title_id : "");
}

int xs_start(xs_session_t *s, const char *title_id, xc_err_t *e) {
    s->path[0] = 0;
    snprintf(s->title, sizeof(s->title), "%s", title_id);
    char *body = xs_build_play_body(title_id, s->locale);
    if (!body) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
    https_resp_t r;
    int rc = gssv_req(s->creds, "play", "POST", "/v5/sessions/cloud/play", body, &r, e);
    cJSON_free(body);
    if (rc != XS_OK) return rc;
    if (!is_2xx(r.status)) { http_fail(e, "play", &r); https_free(&r); return XS_ERR; }
    rc = xs_parse_play(r.body, s->path, sizeof(s->path));
    https_free(&r);
    if (rc != 0) { set_err(e, HTTPS_EPROTO, "play sem sessionPath"); return XS_ERR; }
    xc_log("[sess] sessao criada (%s)", title_id);
    return XS_OK;
}

int xs_get_state(xs_session_t *s, xs_status_t *st, xc_err_t *e) {
    char path[224];
    memset(st, 0, sizeof(*st));
    st->queue_pos = st->wait_s = -1;
    if (sess_path(s, "/state", path, sizeof(path), e)) return XS_ERR;
    https_resp_t r;
    int rc = gssv_req(s->creds, "state", "GET", path, NULL, &r, e);
    if (rc != XS_OK) return rc;
    if (r.status == 404) { http_fail(e, "state", &r); https_free(&r); return XS_ENOTFOUND; }
    if (!is_2xx(r.status)) { http_fail(e, "state", &r); https_free(&r); return XS_ERR; }
    rc = xs_parse_state(r.body, st);
    https_free(&r);
    if (rc != 0) { set_err(e, HTTPS_EPROTO, "JSON invalido em state"); return XS_ERR; }
    return XS_OK;
}

int xs_wait_time(xs_session_t *s, int *sec, xc_err_t *e) {
    *sec = -1;
    char path[96];
    snprintf(path, sizeof(path), "/v1/waittime/%s", s->title);
    https_resp_t r;
    int rc = gssv_req(s->creds, "waittime", "GET", path, NULL, &r, e);
    if (rc != XS_OK) return rc;
    if (!is_2xx(r.status)) { http_fail(e, "waittime", &r); https_free(&r); return XS_ERR; }
    *sec = xs_parse_wait_time(r.body);
    https_free(&r);
    return *sec >= 0 ? XS_OK : XS_ERR;
}

int xs_connect(xs_session_t *s, xc_err_t *e) {
    char path[224];
    if (sess_path(s, "/connect", path, sizeof(path), e)) return XS_ERR;
    for (int pass = 0; pass < 2; pass++) {
        if (!s->creds->passport) {
            int pr = xs_creds_passport(s->creds, e);
            if (pr != XS_OK) return pr;
        }
        char *body = xs_build_connect_body(s->creds->passport);
        if (!body) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
        https_resp_t r;
        int rc = gssv_req(s->creds, "connect", "POST", path, body, &r, e);
        xa_secret_free(&body);
        if (rc != XS_OK) return rc;
        if (is_2xx(r.status)) { https_free(&r); return XS_OK; }
        http_fail(e, "connect", &r);
        https_free(&r);
        /* passport velho? busca outro e tenta uma vez mais */
        xa_secret_free(&s->creds->passport);
    }
    return XS_ERR;
}

int xs_sdp_offer(xs_session_t *s, const char *offer_sdp, xc_err_t *e) {
    char path[224];
    if (sess_path(s, "/sdp", path, sizeof(path), e)) return XS_ERR;
    char *body = xs_build_sdp_body(offer_sdp);
    if (!body) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
    https_resp_t r;
    int rc = gssv_req(s->creds, "sdp", "POST", path, body, &r, e);
    cJSON_free(body);
    if (rc != XS_OK) return rc;
    if (!is_2xx(r.status)) { http_fail(e, "sdp", &r); https_free(&r); return XS_ERR; }
    https_free(&r);
    return XS_OK;
}

int xs_sdp_poll(xs_session_t *s, char **answer, xc_err_t *e) {
    *answer = NULL;
    char path[224];
    if (sess_path(s, "/sdp", path, sizeof(path), e)) return XS_ERR;
    https_resp_t r;
    int rc = gssv_req(s->creds, "sdp poll", "GET", path, NULL, &r, e);
    if (rc != XS_OK) return rc;
    if (r.status == 204 || (is_2xx(r.status) && body_empty(&r))) { https_free(&r); return XS_PENDING; }
    if (!is_2xx(r.status)) { http_fail(e, "sdp poll", &r); https_free(&r); return XS_ERR; }
    char why[120];
    rc = xs_parse_sdp_answer(r.body, answer, why, sizeof(why));
    https_free(&r);
    if (rc != XS_OK) set_err(e, HTTPS_EPROTO, "sdp: %s", why);
    return rc;
}

int xs_ice_send(xs_session_t *s, const char *const *cands, int n, const char *ufrag, xc_err_t *e) {
    char path[224];
    if (sess_path(s, "/ice", path, sizeof(path), e)) return XS_ERR;
    char *body = xs_build_ice_body(cands, n, ufrag);
    if (!body) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
    https_resp_t r;
    int rc = gssv_req(s->creds, "ice", "POST", path, body, &r, e);
    cJSON_free(body);
    if (rc != XS_OK) return rc;
    if (!is_2xx(r.status)) { http_fail(e, "ice", &r); https_free(&r); return XS_ERR; }
    char why[120];
    int bad = !body_empty(&r) && xs_exchange_error(r.body, why, sizeof(why));
    https_free(&r);
    if (bad) { set_err(e, HTTPS_EPROTO, "ice: %s", why); return XS_EFAILED; }
    return XS_OK;
}

int xs_ice_poll(xs_session_t *s, xs_cands_t *out, xc_err_t *e) {
    memset(out, 0, sizeof(*out));
    char path[224];
    if (sess_path(s, "/ice", path, sizeof(path), e)) return XS_ERR;
    https_resp_t r;
    int rc = gssv_req(s->creds, "ice poll", "GET", path, NULL, &r, e);
    if (rc != XS_OK) return rc;
    if (r.status == 204 || (is_2xx(r.status) && body_empty(&r))) { https_free(&r); return XS_PENDING; }
    if (!is_2xx(r.status)) { http_fail(e, "ice poll", &r); https_free(&r); return XS_ERR; }
    rc = xs_parse_ice(r.body, out);
    https_free(&r);
    if (rc == XS_EFAILED) set_err(e, HTTPS_EPROTO, "ice poll: errorDetails");
    else if (rc != XS_OK) set_err(e, HTTPS_EPROTO, "JSON invalido em ice poll");
    return rc;
}

int xs_keepalive(xs_session_t *s, xc_err_t *e) {
    char path[224];
    if (sess_path(s, "/keepalive", path, sizeof(path), e)) return XS_ERR;
    https_resp_t r;
    int rc = gssv_req(s->creds, "keepalive", "POST", path, "", &r, e);
    if (rc != XS_OK) return rc;
    int st = r.status;
    if (!is_2xx(st)) http_fail(e, "keepalive", &r);
    https_free(&r);
    if (st == 404) return XS_ENOTFOUND;
    return is_2xx(st) ? XS_OK : XS_ERR;
}

int xs_delete_path(xs_creds_t *c, const char *path, xc_err_t *e) {
    char p[sizeof(((xs_active_t *)0)->path)];
    norm_path(p, sizeof(p), path);
    https_resp_t r;
    int rc = gssv_req(c, "delete", "DELETE", p, NULL, &r, e);
    if (rc != XS_OK) return rc;
    int ok = is_2xx(r.status) || r.status == 404;
    if (!ok) http_fail(e, "delete", &r);
    https_free(&r);
    return ok ? XS_OK : XS_ERR;
}

void xs_stop(xs_session_t *s) {
    if (!s->path[0]) return;
    xc_err_t e;
    memset(&e, 0, sizeof(e));
    if (xs_delete_path(s->creds, s->path, &e) != XS_OK) xc_log("[sess] DELETE falhou: %s", e.msg);
    else xc_log("[sess] sessao encerrada (DELETE)");
    s->path[0] = 0;
}

int xs_active_list(xs_creds_t *c, xs_active_t *out, int max, int *n, xc_err_t *e) {
    *n = 0;
    https_resp_t r;
    int rc = gssv_req(c, "active", "GET", "/v5/sessions/cloud/active", NULL, &r, e);
    if (rc != XS_OK) return rc;
    if (r.status == 404 || r.status == 204 || (is_2xx(r.status) && body_empty(&r))) {
        https_free(&r);
        return XS_OK;
    }
    if (!is_2xx(r.status)) { http_fail(e, "active", &r); https_free(&r); return XS_ERR; }
    int k = xs_parse_active(r.body, out, max);
    https_free(&r);
    if (k < 0) { set_err(e, HTTPS_EPROTO, "JSON invalido em active"); return XS_ERR; }
    *n = k;
    return XS_OK;
}

/* ------------------------------------------------- v0.19.2: assíncrono */

struct https_conn *xs_conn(const xs_creds_t *c) { return c ? c->conn : NULL; }

void xs_async_abort(xs_session_t *s) {
    if (s->aq && s->creds->conn) https_conn_abort(s->creds->conn);
    s->aq = XS_AQ_NONE;
}

int xs_async_start(xs_session_t *s, int kind, const char *const *cands, int n, const char *ufrag, xc_err_t *e) {
    xs_creds_t *c = s->creds;
    if (s->aq) { set_err(e, HTTPS_EBUSY, "pedido em voo"); return XS_ERR; }
    if (!c->gs_token || !c->host[0]) { set_err(e, HTTPS_EHTTP, "sem credenciais"); return XS_ENEEDCPU; }
    /* sem conexão (nunca conectou / região mudou): a thread abre */
    if (!c->conn || strcmp(https_conn_host(c->conn), c->host) != 0) {
        set_err(e, HTTPS_ENEEDCPU, "sem conexao com %s", c->host);
        return XS_ENEEDCPU;
    }
    char path[224];
    const char *method = "POST", *step = "keepalive";
    char *body = NULL;
    int body_owned = 0;
    if (kind == XS_AQ_KEEPALIVE) {
        if (sess_path(s, "/keepalive", path, sizeof(path), e)) return XS_ERR;
        body = (char *)"";
    } else if (kind == XS_AQ_ICE_SEND) {
        if (sess_path(s, "/ice", path, sizeof(path), e)) return XS_ERR;
        body = xs_build_ice_body(cands, n, ufrag);
        if (!body) { set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
        body_owned = 1;
        step = "ice";
    } else if (kind == XS_AQ_ICE_POLL) {
        if (sess_path(s, "/ice", path, sizeof(path), e)) return XS_ERR;
        method = "GET";
        step = "ice poll";
    } else {
        set_err(e, HTTPS_EPROTO, "pedido desconhecido");
        return XS_ERR;
    }
    char *hdr = xs_build_headers(c->gs_token);
    if (!hdr) { if (body_owned) cJSON_free(body); set_err(e, HTTPS_ETOOBIG, "sem memoria"); return XS_ERR; }
    int rc = https_conn_start(c->conn, step, method, path, hdr, body, body ? strlen(body) : 0, SMALL_BODY, e);
    xa_secret_free(&hdr);
    if (body_owned) cJSON_free(body);
    if (rc != 0) return rc == HTTPS_ENEEDCPU ? XS_ENEEDCPU : map_transport(rc);
    s->aq = kind;
    return XS_OK;
}

int xs_async_poll(xs_session_t *s, xs_cands_t *out, xc_err_t *e) {
    if (!s->aq) { set_err(e, HTTPS_EPROTO, "nenhum pedido em voo"); return XS_ERR; }
    https_resp_t r;
    int rc = https_conn_poll(s->creds->conn, &r, e);
    if (rc == HTTPS_PENDING) return XS_INPROGRESS;
    int kind = s->aq;
    s->aq = XS_AQ_NONE;
    if (rc == HTTPS_ENEEDCPU) return XS_ENEEDCPU;
    if (rc != 0) return map_transport(rc);
    const char *step = kind == XS_AQ_KEEPALIVE ? "keepalive" : kind == XS_AQ_ICE_SEND ? "ice" : "ice poll";
    if (r.status == 401) {   /* re-login: bloqueante (4 hosts, handshakes completos, pendrive) */
        http_fail(e, step, &r);
        https_free(&r);
        return XS_ENEEDCPU;
    }
    if (kind == XS_AQ_KEEPALIVE) {
        int st = r.status;
        if (!is_2xx(st)) http_fail(e, "keepalive", &r);
        https_free(&r);
        if (st == 404) return XS_ENOTFOUND;
        return is_2xx(st) ? XS_OK : XS_ERR;
    }
    if (kind == XS_AQ_ICE_SEND) {
        if (!is_2xx(r.status)) { http_fail(e, "ice", &r); https_free(&r); return XS_ERR; }
        char why[120];
        int bad = !body_empty(&r) && xs_exchange_error(r.body, why, sizeof(why));
        https_free(&r);
        if (bad) { set_err(e, HTTPS_EPROTO, "ice: %s", why); return XS_EFAILED; }
        return XS_OK;
    }
    /* ICE poll */
    memset(out, 0, sizeof(*out));
    if (r.status == 204 || (is_2xx(r.status) && body_empty(&r))) { https_free(&r); return XS_PENDING; }
    if (!is_2xx(r.status)) { http_fail(e, "ice poll", &r); https_free(&r); return XS_ERR; }
    rc = xs_parse_ice(r.body, out);
    https_free(&r);
    if (rc == XS_EFAILED) set_err(e, HTTPS_EPROTO, "ice poll: errorDetails");
    else if (rc != XS_OK) set_err(e, HTTPS_EPROTO, "JSON invalido em ice poll");
    return rc;
}
