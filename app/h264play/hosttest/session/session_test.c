/*
 * XCloud360 stage 3 — testes do host da sessão xCloud (xc_session.c +
 * xs_machine.c) contra um GSSV FALSO (mock_port.c). Nenhum pedido sai da
 * máquina, nenhum token real é lido. Os corpos/cabeçalhos são comparados
 * byte a byte com o que o green-nx manda (golden.h, gerado do json.hpp dele).
 *
 * Formatos das respostas: /state = linhas [XCLOUD360-STATE] reais dos logs do
 * launcher (/tmp/xcloud-launcher/capture-*.log, só state/errorDetails/
 * serverPlatform); /play, /sdp, /ice, /login/user = os campos que o green-nx
 * lê (session.cpp/auth.cpp) no formato do greenlight; região brs.core = a do
 * session-meta.json gravado. Ids de sessão e tokens são inventados.
 */
#include "mock_port.h"
#include "golden.h"
#include "../../source/session/xc_session.h"
#include "../../source/session/xs_machine.h"
#include "../../source/login/cjson/cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("  FALHOU %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define REGION "brs.core.gssv-play-prod.xboxlive.com"
#define HOME_REGION "brs.core.gssv-play-prodxhome.xboxlive.com"
#define TITLE  "FORZAHORIZON5"
#define NAME   "Forza Horizon 5"
#define SESS_FMT "v5/sessions/cloud/1A2B3C4D-5E6F-4A7B-8C9D-00000000000%d"
#define OLD_PATH "v5/sessions/cloud/0F0F0F0F-1111-4222-8333-444444444444"
#define OFFER  "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\na=ice-ufrag:abCD\r\na=setup:actpass\r\n"
#define LOCAL1 "candidate:1 1 UDP 2122317823 192.168.15.60 50000 typ host"
#define LOCAL2 "candidate:2 1 UDP 1686052607 200.100.50.25 50000 typ srflx raddr 192.168.15.60 rport 50000"
#define ANSWER "v=0\r\no=- 99 2 IN IP4 0.0.0.0\r\na=ice-ufrag:Tb/f\r\na=setup:active\r\n"

#define ST(s)  "{\"errorDetails\":null,\"serverPlatform\":\"XBOX\",\"state\":\"" s "\"}"
#define ST_PROV  ST("Provisioning")
#define ST_WAIT  ST("WaitingForResources")
#define ST_READY ST("ReadyToConnect")
#define ST_DONE  ST("Provisioned")
#define ST_FAIL  "{\"errorDetails\":{\"code\":\"SessionProvisioningFailed\",\"message\":\"Nao foi possivel alocar\"},\"serverPlatform\":\"XBOX\",\"state\":\"Failed\"}"
/* hipotético (o /state real da fila não trouxe posição): testa o walker */
#define ST_WAIT_POS "{\"errorDetails\":null,\"queueInfo\":{\"positionInQueue\":3,\"estimatedTotalWaitTimeInSeconds\":45},\"state\":\"WaitingForResources\"}"

/* ------------------------------------------------------- servidor falso */
typedef struct {
    int home;
    const char *states[2][24];   /* por sessão criada (0 = 1ª, 1 = 2ª+); o último repete */
    int   nstates[2];
    int   active_kind;           /* 0 = 404, 1 = OLD do mesmo título, 2 = OLD de outro título */
    int   sdp_204s;              /* -1 = nunca responde */
    int   ice_204s;
    int   revoke_at_state;       /* nº do /state em que o gsToken atual "expira" (0 = nunca) */
    int   gs_always_bad;
    int   invalid_grant;
    int   state_timeout;         /* todo /state = timeout de rede */
    int   waittime_sec;          /* -1 = 404 */
    int   revoke_at_ka;          /* v0.19.2: nº do /keepalive em que o gsToken expira */
    int   late_local, late_sent; /* v0.19.2: candidato local depois do answer (trickle) */
    /* WebRTC falso */
    int   rtc_dead_attempts;     /* nº de PCs que ficam "ICE ok, DTLS mudo" */
    int   rtc_drop_once;         /* cai 1x depois de STREAMING */
    int   ask_answer;            /* -1 = não responde; 0/1 */
    uint64_t stop_after_ms;      /* depois de STREAMING (0 = 12s) */
    /* contadores */
    int   n_refresh, n_play, n_delete, n_state, n_connect, n_passport, n_ka, n_wait, n_sdp, n_ice, n_active, n_login;
    int   gs_valid, gs_issued, refresh_ver;
    int   nsess;
    char  paths[8][128];
    int   sidx[8], sdp_polls[8], ice_polls[8];
    /* WebRTC */
    unsigned rtc_gen;
    int   rtc_pcs, rtc_has_answer;
    uint64_t rtc_t_answer;
    char  remotes[64][XS_CAND_LEN];
    int   nremotes;
    uint64_t t_streaming;
    int   dropped;
    char  answers_ok;
} srv_t;

static char *sdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }
static char *fmt(const char *f, ...) {
    char b[4096];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return sdup(b);
}

static int bearer_ok(srv_t *s, const mock_req_t *q) {
    const char *a = strstr(q->headers, "Authorization: Bearer ");
    if (!a) return 0;
    int v = -1;
    if (sscanf(a, "Authorization: Bearer GS-%d\r\n", &v) != 1) return 0;
    return !s->gs_always_bad && v == s->gs_valid;
}

static char *exchange(cJSON *inner) {
    char *in = cJSON_PrintUnformatted(inner);
    cJSON_Delete(inner);
    cJSON *o = cJSON_CreateObject();
    cJSON *ed = cJSON_AddObjectToObject(o, "errorDetails");   /* tudo null = sucesso */
    cJSON_AddNullToObject(ed, "code");
    cJSON_AddNullToObject(ed, "message");
    cJSON_AddStringToObject(o, "exchangeResponse", in);
    free(in);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static char *sdp_answer_body(void) {
    cJSON *a = cJSON_CreateObject();
    cJSON_AddNumberToObject(a, "chat", 1);
    cJSON_AddNumberToObject(a, "control", 2);
    cJSON_AddNumberToObject(a, "input", 9);
    cJSON_AddNumberToObject(a, "message", 1);
    cJSON_AddStringToObject(a, "messageType", "answer");
    cJSON_AddStringToObject(a, "sdp", ANSWER);
    cJSON_AddStringToObject(a, "status", "success");
    return exchange(a);
}

static void ice_obj(cJSON *arr, const char *cand) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "candidate", cand);
    cJSON_AddStringToObject(c, "messageType", "iceCandidate");
    cJSON_AddStringToObject(c, "sdpMLineIndex", "0");
    cJSON_AddStringToObject(c, "sdpMid", "0");
    cJSON_AddItemToArray(arr, c);
}

/* Teredo: 1.2.3.4:3074 (bits invertidos), IPv4 comum, IPv6 local (descartado), fim. */
static char *ice_answer_body(void) {
    cJSON *arr = cJSON_CreateArray();
    ice_obj(arr, "a=candidate:1 1 UDP 100 2001:0000:4136:e378:8000:f3fd:fefd:fcfb 3074 typ host ");
    ice_obj(arr, "a=candidate:2 1 UDP 1 20.30.40.50 9002 typ host");
    ice_obj(arr, "a=candidate:3 1 UDP 1 fe80:0000:0000:0000:0000:0000:0000:0001 9002 typ host");
    ice_obj(arr, "a=end-of-candidates");
    return exchange(arr);
}

static int sess_index(srv_t *s, const char *path, const char **suffix) {
    for (int i = 0; i < s->nsess; i++) {
        size_t l = strlen(s->paths[i]);
        if (!strncmp(path, s->paths[i], l) && (path[l] == 0 || path[l] == '/')) { *suffix = path + l; return i; }
    }
    return -1;
}

static int handler(void *ctx, const mock_req_t *q, int *status, char **body) {
    srv_t *s = (srv_t *)ctx;
    *status = 404;
    *body = NULL;
    if (!strcmp(q->host, "login.microsoftonline.com") && !strcmp(q->path, "/consumers/oauth2/v2.0/token")) {
        s->n_refresh++;
        if (s->invalid_grant) { *status = 400; *body = sdup("{\"error\":\"invalid_grant\",\"error_description\":\"AADSTS70000\"}"); return 0; }
        int v = -1;
        const char *rt = q->body ? strstr(q->body, "refresh_token=FAKE-REFRESH-") : NULL;
        if (!rt || !strstr(q->body, "grant_type=refresh_token") || !strstr(q->body, "client_id=1f907974-")) { *status = 400; return 0; }
        sscanf(rt, "refresh_token=FAKE-REFRESH-%d", &v);
        s->refresh_ver = v + 1;
        *status = 200;
        *body = fmt("{\"token_type\":\"Bearer\",\"scope\":\"XboxLive.signin\",\"expires_in\":3600,"
                    "\"access_token\":\"FAKE-ACCESS\",\"refresh_token\":\"FAKE-REFRESH-%d\"}", v + 1);
        return 0;
    }
    if (!strcmp(q->host, "user.auth.xboxlive.com")) {
        *status = 200;
        *body = sdup("{\"IssueInstant\":\"2026-10-25T10:00:00Z\",\"Token\":\"FAKE-USERTOKEN\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"1234\"}]}}");
        return 0;
    }
    if (!strcmp(q->host, "xsts.auth.xboxlive.com")) {
        *status = 200;
        *body = sdup("{\"Token\":\"FAKE-XSTS-GSSV\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"1234\"}]}}");
        return 0;
    }
    if (strstr(q->host, ".gssv-play-prod.xboxlive.com") && !strcmp(q->path, "/v2/login/user")) {
        s->n_login++;
        s->gs_issued++;
        s->gs_valid = s->gs_issued;
        *status = 200;
        *body = fmt("{\"offeringSettings\":{\"allowRegionSelection\":false,\"regions\":["
                    "{\"name\":\"WestEurope\",\"baseUri\":\"https://weu.core.gssv-play-prod.xboxlive.com\",\"isDefault\":false},"
                    "{\"name\":\"BrazilSouth\",\"baseUri\":\"https://" REGION "\",\"isDefault\":true}]},"
                    "\"market\":\"BR\",\"gsToken\":\"GS-%d\",\"tokenType\":\"bearer\",\"durationInSeconds\":14400}",
                    s->gs_issued);
        if (s->home) {
            free(*body);
            *body = fmt("{\"gsToken\":\"GS-%d\",\"offeringSettings\":{\"regions\":["
                        "{\"baseUri\":\"https://" HOME_REGION "\"}]}}", s->gs_issued);
        }
        return 0;
    }
    if (!strcmp(q->host, "login.live.com") && !strcmp(q->path, "/oauth20_token.srf")) {
        s->n_passport++;
        if (!q->body || !strstr(q->body, "scope=service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN&")) {
            *status = 400;
            return 0;
        }
        *status = 200;
        *body = sdup("{\"token_type\":\"bearer\",\"expires_in\":86400,\"access_token\":\"PASSPORT-FAKE\"}");
        return 0;
    }
    if (strcmp(q->host, s->home ? HOME_REGION : REGION) != 0) return 0;

    if (!bearer_ok(s, q)) { *status = 401; *body = sdup(""); return 0; }

    if (!strcmp(q->path, "/v5/sessions/cloud/active")) {
        s->n_active++;
        if (s->active_kind == 0) { *status = 404; return 0; }
        *status = 200;
        *body = fmt("[{\"sessionId\":\"0F0F0F0F-1111-4222-8333-444444444444\",\"sessionPath\":\"" OLD_PATH "\","
                    "\"titleId\":\"%s\",\"state\":\"Provisioned\"}]", s->active_kind == 1 ? TITLE : "HALO5");
        return 0;
    }
    if (s->home && !strcmp(q->path, "/v6/servers/home?mr=50")) {
        *status = 200;
        *body = sdup("{\"results\":[{\"serverId\":\"F400TEST001\",\"deviceName\":\"Sala\","
                     "\"consoleType\":\"XboxSeriesX\",\"powerState\":\"ConnectedStandby\"}]}");
        return 0;
    }
    if (!strcmp(q->path, s->home ? "/v5/sessions/home/play" : "/v5/sessions/cloud/play") && !strcmp(q->method, "POST")) {
        s->n_play++;
        int i = s->nsess++;
        if (s->home) snprintf(s->paths[i], sizeof(s->paths[i]), "/v5/sessions/home/test-%d", i + 1);
        else snprintf(s->paths[i], sizeof(s->paths[i]), "/" SESS_FMT, i + 1);
        *status = 200;
        *body = fmt("{\"sessionPath\":\"%s\",\"sessionId\":\"%s\",\"state\":\"Provisioning\"}", s->paths[i] + 1,
                    strrchr(s->paths[i], '/') + 1);
        return 0;
    }
    if (!strncmp(q->path, "/v1/waittime/", 13)) {
        s->n_wait++;
        if (s->waittime_sec < 0) return 0;
        *status = 200;
        *body = fmt("{\"estimatedProvisioningTimeInSeconds\":30,\"estimatedAllocationTimeInSeconds\":%d,"
                    "\"estimatedTotalWaitTimeInSeconds\":%d}", s->waittime_sec - 30, s->waittime_sec);
        return 0;
    }
    const char *suf = NULL;
    int si = sess_index(s, q->path, &suf);
    if (si < 0) return 0;
    if (!strcmp(q->method, "DELETE") && !*suf) { s->n_delete++; *status = 202; return 0; }
    if (!strcmp(suf, "/state")) {
        s->n_state++;
        if (s->state_timeout) return HTTPS_ETIMEOUT;
        if (s->revoke_at_state && s->n_state == s->revoke_at_state) {
            s->revoke_at_state = 0;
            s->gs_valid = -1;           /* token expirou no servidor */
            *status = 401;
            return 0;
        }
        int set = si == 0 ? 0 : 1;
        int k = s->sidx[si]++;
        if (k >= s->nstates[set]) k = s->nstates[set] - 1;
        *status = 200;
        *body = sdup(s->states[set][k]);
        return 0;
    }
    if (!strcmp(suf, "/connect")) {
        s->n_connect++;
        *status = q->body && !strcmp(q->body, GOLDEN_CONNECT) ? 200 : 400;
        return 0;
    }
    if (!strcmp(suf, "/sdp") && !strcmp(q->method, "POST")) { s->n_sdp++; *status = 202; return 0; }
    if (!strcmp(suf, "/sdp")) {
        int k = s->sdp_polls[si]++;
        if (s->sdp_204s < 0 || k < s->sdp_204s) { *status = 204; return 0; }
        *status = 200;
        *body = sdp_answer_body();
        return 0;
    }
    if (!strcmp(suf, "/ice") && !strcmp(q->method, "POST")) {
        s->n_ice++;
        *status = 200;
        *body = sdup("{\"errorDetails\":{\"code\":null,\"message\":null},\"exchangeResponse\":null}");
        return 0;
    }
    if (!strcmp(suf, "/ice")) {
        int k = s->ice_polls[si]++;
        if (k < s->ice_204s || k > s->ice_204s) { *status = 204; return 0; }   /* entrega 1x */
        *status = 200;
        *body = ice_answer_body();
        return 0;
    }
    if (!strcmp(suf, "/keepalive")) {
        s->n_ka++;
        if (s->revoke_at_ka && s->n_ka == s->revoke_at_ka) {
            s->revoke_at_ka = 0;
            s->gs_valid = -1;
            *status = 401;
            return 0;
        }
        *status = 200;
        *body = sdup("{\"alertTimeInMs\":600000,\"keepAliveInSeconds\":300}");
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------- WebRTC falso */
static void rtc_tick(xsm_t *m, srv_t *s) {
    unsigned g = xsm_rtc_gen(m);
    if (g != s->rtc_gen) {   /* PC novo */
        s->rtc_gen = g;
        s->rtc_has_answer = 0;
    }
    unsigned wg;
    if (xsm_rtc_want_offer(m, &wg)) {
        s->rtc_pcs++;
        xsm_rtc_add_local(m, wg, LOCAL1);
        xsm_rtc_add_local(m, wg, LOCAL2 "\r\n");
        xsm_rtc_set_offer(m, wg, OFFER);
    }
    if (!s->rtc_has_answer) {
        char *a = xsm_rtc_take_answer(m, g);
        if (a) {
            s->rtc_has_answer = 1;
            s->rtc_t_answer = g_now_ms;
            if (strcmp(a, ANSWER) != 0) s->answers_ok = 2;
            else if (!s->answers_ok) s->answers_ok = 1;
            free(a);
        }
    }
    if (s->rtc_has_answer && s->late_local && !s->late_sent) {
        s->late_sent = 1;
        xsm_rtc_add_local(m, g, "candidate:3 1 UDP 1686052606 200.100.50.26 50001 typ srflx raddr 192.168.15.60 rport 50001");
    }
    if (s->rtc_has_answer) {
        char buf[8][XS_CAND_LEN];
        int n;
        while ((n = xsm_rtc_take_remote(m, g, buf, 8)) > 0)
            for (int i = 0; i < n && s->nremotes < 64; i++) memcpy(s->remotes[s->nremotes++], buf[i], XS_CAND_LEN);
        uint64_t dt = g_now_ms - s->rtc_t_answer;
        if (dt >= 300) xsm_rtc_report(m, g, XSM_RTC_ICE_UP);
        if (s->rtc_pcs > s->rtc_dead_attempts) {
            if (dt >= 600) xsm_rtc_report(m, g, XSM_RTC_MEDIA_UP);
            if (dt >= 900) xsm_rtc_report(m, g, XSM_RTC_VIDEO);
        }
    }
    if (xsm_phase(m) == XSM_P_STREAMING) {
        if (!s->t_streaming) s->t_streaming = g_now_ms;
        if (s->rtc_drop_once && !s->dropped && g_now_ms - s->t_streaming > 3000) {
            s->dropped = 1;
            s->t_streaming = 0;
            xsm_rtc_report(m, g, XSM_RTC_DROPPED);
        } else if (s->t_streaming && g_now_ms - s->t_streaming >= (s->stop_after_ms ? s->stop_after_ms : 12000)) {
            xsm_request_stop(m);
        }
    }
    if (xsm_phase(m) == XSM_P_ASK && s->ask_answer >= 0) xsm_answer_active(m, s->ask_answer);
}

/* --------------------------------------------------------------- driver */
static char g_lines[256][200];
static int  g_nlines;

static int has_line(const char *l) {
    for (int i = 0; i < g_nlines; i++) if (!strcmp(g_lines[i], l)) return 1;
    return 0;
}

static int line_index(const char *l) {
    for (int i = 0; i < g_nlines; i++) if (!strcmp(g_lines[i], l)) return i;
    return -1;
}

static int count_req(const char *method, const char *path_sub) {
    int n = 0;
    for (int i = 0; i < g_nreq; i++)
        if ((!method || !strcmp(g_reqs[i].method, method)) && strstr(g_reqs[i].path, path_sub)) n++;
    return n;
}

static const mock_req_t *find_req(const char *method, const char *path_sub, int nth) {
    for (int i = 0; i < g_nreq; i++)
        if (!strcmp(g_reqs[i].method, method) && strstr(g_reqs[i].path, path_sub) && nth-- == 0) return &g_reqs[i];
    return NULL;
}

static xsm_phase_t run(xsm_t *m, srv_t *s, uint64_t limit_ms) {
    uint64_t t0 = g_now_ms;
    unsigned seq = 0;
    for (int it = 0; it < 200000; it++) {
        int d = xsm_step(m);
        char line[200];
        unsigned sq = xsm_state_line(m, line, sizeof(line));
        if (sq != seq && g_nlines < 256) { seq = sq; snprintf(g_lines[g_nlines++], sizeof(g_lines[0]), "%s", line); }
        if (d < 0) break;
        rtc_tick(m, s);
        if (g_now_ms - t0 > limit_ms) { printf("  (limite de tempo virtual)\n"); break; }
        g_now_ms += (uint64_t)(d > 0 ? d : 0);
    }
    return xsm_phase(m);
}

static void setup(srv_t *s, xsm_t *m, xsm_cfg_t *cfg) {
    mock_reset();
    memset(s, 0, sizeof(*s));
    s->waittime_sec = -1;
    s->ask_answer = -1;
    g_mock_handler = handler;
    g_mock_ctx = s;
    g_nlines = 0;
    mock_set_token("FAKE-REFRESH-0-xxxxxxxxxxxxxxxxxxxx\n");
    xsm_cfg_defaults(cfg, TITLE, NAME);
    (void)m;
}

static void no_secrets_in_logs(void) {
    const char *bad[] = { "GS-", "FAKE-", "PASSPORT-FAKE", "Bearer" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!g_logbuf || !strstr(g_logbuf, bad[i]), "token no log: %s", bad[i]);
}

static void dump_lines(void) {
    for (int i = 0; i < g_nlines; i++) {
        char t[200];
        snprintf(t, sizeof(t), "%s", g_lines[i]);
        for (char *p = t; *p; p++) if (*p == '\t') *p = '|';
        printf("    %s\n", t);
    }
}

/* ================================================================ testes */

/* Upstream 0.21.38 uses Brasilia's offset and a configurable play locale.
 * Keep comparing all other fields with the independent green-nx fixture. */
static int play_matches_golden(const char *body, const char *locale) {
    cJSON *j = cJSON_Parse(GOLDEN_PLAY);
    cJSON *settings = cJSON_GetObjectItemCaseSensitive(j, "settings");
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(settings, "timezoneOffsetMinutes"), 180);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(settings, "locale"), locale);
    char *expected = cJSON_PrintUnformatted(j);
    int equal = body && expected && !strcmp(body, expected);
    free(expected);
    cJSON_Delete(j);
    return equal;
}

static void test_pure(void) {
    printf("[puro] builders/parsers\n");
    char *b = xs_build_play_body(TITLE, "en-US");
    CHECK(play_matches_golden(b, "en-US"), "play body != green-nx with upstream timezone:\n%s", b);
    free(b);
    b = xs_build_connect_body("PASSPORT-FAKE");
    CHECK(!strcmp(b, GOLDEN_CONNECT), "connect body");
    free(b);
    b = xs_build_sdp_body(OFFER);
    CHECK(!strcmp(b, GOLDEN_SDP), "sdp body != green-nx:\n%s", b);
    free(b);
    const char *c[2] = { LOCAL1, LOCAL2 };
    b = xs_build_ice_body(c, 2, "abCD");
    CHECK(!strcmp(b, GOLDEN_ICE), "ice body != green-nx:\n%s", b);
    free(b);
    b = xs_build_headers("GS-1");
    CHECK(!strcmp(b, "Accept: application/json\r\nContent-Type: application/json\r\nX-Gssv-Client: XboxComBrowser\r\n"
                     GOLDEN_DEVICE_INFO "\r\nAuthorization: Bearer GS-1\r\n"), "headers != green-nx:\n%s", b);
    free(b);

    char ip[20];
    int port = 0;
    CHECK(xs_teredo_decode("2001:0000:4136:e378:8000:f3fd:fefd:fcfb", ip, sizeof(ip), &port) == 1 &&
          !strcmp(ip, "1.2.3.4") && port == 3074, "teredo %s:%d", ip, port);
    CHECK(xs_teredo_decode("2001:0:4136:e378:8000:f3fd:fefd:fcfb", ip, sizeof(ip), &port) == 1, "teredo grupo curto");
    CHECK(xs_teredo_decode("2001::fefd:fcfb", ip, sizeof(ip), &port) == 0, "teredo comprimido deveria falhar (= green-nx)");
    CHECK(xs_teredo_decode("fe80:0:0:0:0:0:0:1", ip, sizeof(ip), &port) == 0, "nao-teredo");

    xs_cands_t cs;
    char *ice = ice_answer_body();
    CHECK(xs_parse_ice(ice, &cs) == XS_OK && cs.n == 3 && cs.end == 1, "ice n=%d end=%d", cs.n, cs.end);
    CHECK(cs.n == 3 && !strcmp(cs.c[0], "candidate:20 1 UDP 1 1.2.3.4 3074 typ host"), "teredo cand: %s", cs.c[0]);
    CHECK(cs.n == 3 && !strcmp(cs.c[1], "candidate:21 1 UDP 1 1.2.3.4 9002 typ host"), "teredo 9002: %s", cs.c[1]);
    CHECK(cs.n == 3 && !strcmp(cs.c[2], "candidate:2 1 UDP 1 20.30.40.50 9002 typ host"), "ipv4: %s", cs.c[2]);
    free(ice);
    /* forma {"candidates":[ "<json string>" ]} */
    CHECK(xs_parse_ice("{\"exchangeResponse\":\"{\\\"candidates\\\":[\\\"{\\\\\\\"candidate\\\\\\\":\\\\\\\"candidate:9 1 UDP 5 9.9.9.9 1 typ host\\\\\\\"}\\\"]}\"}", &cs) == XS_OK &&
          cs.n == 1 && !strcmp(cs.c[0], "candidate:9 1 UDP 5 9.9.9.9 1 typ host"), "ice candidates{} n=%d", cs.n);
    CHECK(xs_parse_ice("{\"errorDetails\":{\"code\":\"X\",\"message\":null},\"exchangeResponse\":null}", &cs) == XS_EFAILED, "ice erro");

    char msg[96];
    CHECK(xs_exchange_error("{\"errorDetails\":{\"code\":null,\"message\":null}}", msg, sizeof(msg)) == 0, "all-null nao e erro");
    CHECK(xs_exchange_error("{\"errorDetails\":null}", msg, sizeof(msg)) == 0, "null nao e erro");
    CHECK(xs_exchange_error("{\"errorDetails\":{\"code\":\"AgentCommandError\",\"message\":\"x\"}}", msg, sizeof(msg)) == 1 &&
          !strcmp(msg, "AgentCommandError: x"), "erro real: %s", msg);

    char *sdp = NULL, err[64];
    char *sb = sdp_answer_body();
    CHECK(xs_parse_sdp_answer(sb, &sdp, err, sizeof(err)) == XS_OK && sdp && !strcmp(sdp, ANSWER), "answer");
    free(sdp);
    free(sb);
    CHECK(xs_parse_sdp_answer("{\"errorDetails\":{\"code\":\"OfferRejected\",\"message\":null},\"exchangeResponse\":null}",
                              &sdp, err, sizeof(err)) == XS_EFAILED && !strcmp(err, "OfferRejected"), "answer erro: %s", err);

    xs_status_t st;
    CHECK(xs_parse_state(ST_FAIL, &st) == 0 && st.st == XS_SS_FAILED && !strcmp(st.err_code, "SessionProvisioningFailed"), "failed");
    CHECK(xs_parse_state(ST_WAIT_POS, &st) == 0 && st.st == XS_SS_WAITING && st.queue_pos == 3 && st.wait_s == 45, "fila pos=%d eta=%d", st.queue_pos, st.wait_s);
    CHECK(xs_parse_state(ST_WAIT, &st) == 0 && st.queue_pos == -1 && st.wait_s == -1, "fila sem info");
    CHECK(xs_parse_state(ST("Banana"), &st) == 0 && st.st == XS_SS_UNKNOWN, "estado desconhecido");

    xs_active_t a[4];
    CHECK(xs_parse_active("{\"sessions\":[{\"sessionPath\":\"v5/sessions/cloud/A\",\"titleId\":\"T1\"},{\"path\":\"/v5/sessions/cloud/B\"},{\"sessionPath\":\"v5/sessions/cloud/A\"}]}", a, 4) == 2 &&
          !strcmp(a[0].path, "/v5/sessions/cloud/A") && !strcmp(a[0].title, "T1") && !strcmp(a[1].path, "/v5/sessions/cloud/B"), "active walker");
    CHECK(xs_parse_wait_time("{\"estimatedTotalWaitTimeInSeconds\":125}") == 125, "waittime");
    char uf[16];
    xs_sdp_ufrag(OFFER, uf, sizeof(uf));
    CHECK(!strcmp(uf, "abCD"), "ufrag %s", uf);
}

static void test_happy(void) {
    printf("[fluxo] caminho feliz\n");
    srv_t s;
    xsm_t m;
    xsm_cfg_t cfg;
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_PROV; s.states[0][1] = ST_PROV; s.states[0][2] = ST_READY; s.states[0][3] = ST_READY; s.states[0][4] = ST_DONE;
    s.nstates[0] = 5;
    s.sdp_204s = 2;
    s.ice_204s = 1;
    xsm_init(&m, &cfg, NULL);
    xsm_phase_t p = run(&m, &s, 120000);
    CHECK(p == XSM_P_DONE, "fase final %s", xsm_phase_name(p));
    CHECK(s.n_play == 1 && s.n_connect == 1 && s.n_passport == 1 && s.n_sdp == 1, "play=%d connect=%d passport=%d sdp=%d", s.n_play, s.n_connect, s.n_passport, s.n_sdp);
    CHECK(s.n_active == 1 && s.n_delete == 1, "active=%d delete=%d", s.n_active, s.n_delete);
    CHECK(s.n_ka >= 2, "keepalives=%d", s.n_ka);
    CHECK(s.answers_ok == 1, "answer chegou intacto no WebRTC");
    int t20 = 0, t21 = 0, v4 = 0;
    for (int i = 0; i < s.nremotes; i++) {
        t20 += !strcmp(s.remotes[i], "candidate:20 1 UDP 1 1.2.3.4 3074 typ host");
        t21 += !strcmp(s.remotes[i], "candidate:21 1 UDP 1 1.2.3.4 9002 typ host");
        v4 += !strcmp(s.remotes[i], "candidate:2 1 UDP 1 20.30.40.50 9002 typ host");
    }
    CHECK(s.nremotes == 3 && t20 && t21 && v4, "remotos=%d", s.nremotes);
    const mock_req_t *q = find_req("POST", "/v5/sessions/cloud/play", 0);
    CHECK(q && play_matches_golden(q->body, "pt-BR"), "play body with upstream locale/timezone");
    CHECK(q && strstr(q->headers, "X-Gssv-Client: XboxComBrowser\r\n") && strstr(q->headers, GOLDEN_DEVICE_INFO "\r\n") &&
          strstr(q->headers, "Authorization: Bearer GS-1\r\n"), "headers do play");
    q = find_req("POST", "/sdp", 0);
    CHECK(q && q->body && !strcmp(q->body, GOLDEN_SDP), "sdp body");
    q = find_req("POST", "/ice", 0);
    CHECK(q && q->body && !strcmp(q->body, GOLDEN_ICE), "ice body (locais + end-of-candidates + ufrag do offer)");
    CHECK(count_req("POST", "/ice") == 1, "locais enviados 1x (%d)", count_req("POST", "/ice"));
    q = find_req("POST", "/keepalive", 0);
    CHECK(q && q->body && q->body[0] == 0 && q->has_body, "keepalive com corpo vazio (Content-Length: 0)");
    q = find_req("DELETE", "/v5/sessions/cloud/1A2B3C4D", 0);
    CHECK(q && !strcmp(q->path, "/" "v5/sessions/cloud/1A2B3C4D-5E6F-4A7B-8C9D-000000000001") && !q->body, "DELETE da sessao");
    /* ordem: connect só depois do ReadyToConnect, sdp só depois do Provisioned */
    int i_conn = -1, i_sdp = -1, i_prov = -1, nst = 0;
    for (int i = 0; i < g_nreq; i++) {
        if (strstr(g_reqs[i].path, "/state")) { nst++; if (nst == 5) i_prov = i; }
        if (strstr(g_reqs[i].path, "/connect") && i_conn < 0) i_conn = i;
        if (strstr(g_reqs[i].path, "/sdp") && i_sdp < 0) i_sdp = i;
    }
    CHECK(i_conn > 0 && i_prov > i_conn && i_sdp > i_prov, "ordem connect=%d prov=%d sdp=%d", i_conn, i_prov, i_sdp);
    /* pre-SRD: o 1º POST /ice vem antes do answer ser publicado (antes de qualquer keepalive) */
    int i_ice = -1, i_ka = -1;
    for (int i = 0; i < g_nreq; i++) {
        if (strstr(g_reqs[i].path, "/ice") && !strcmp(g_reqs[i].method, "POST") && i_ice < 0) i_ice = i;
        if (strstr(g_reqs[i].path, "/keepalive") && i_ka < 0) i_ka = i;
    }
    CHECK(i_ice > i_sdp && i_ka > i_ice, "ordem ice=%d ka=%d", i_ice, i_ka);
    CHECK(has_line("STATE\tSTARTING\tAutenticando na Microsoft..."), "linha auth");
    CHECK(has_line("STATE\tSTARTING\tIniciando " NAME "..."), "linha iniciando");
    CHECK(has_line("STATE\tSTARTING\tPreparando servidor..."), "linha preparando");
    CHECK(has_line("STATE\tSTARTING\tConectando video..."), "linha conectando");
    CHECK(has_line("STATE\tSTARTING\tAguardando video..."), "linha aguardando");
    CHECK(has_line("STATE\tSTREAMING\t" NAME), "linha streaming");
    CHECK(g_nlines > 0 && !strcmp(g_lines[g_nlines - 1], "STATE\tIDLE\tSessao encerrada"), "ultima linha IDLE");
    CHECK(line_index("STATE\tSTARTING\tPreparando servidor...") < line_index("STATE\tSTREAMING\t" NAME), "ordem das linhas");
    CHECK(mock_get_token() && !strcmp(mock_get_token(), "FAKE-REFRESH-1"), "refresh rotacionado salvo: %s", mock_get_token() ? "sim" : "nao");
    no_secrets_in_logs();
    if (g_fail) dump_lines();
    xsm_free(&m);
}

static void test_queue(void) {
    printf("[fluxo] fila com posicao/tempo\n");
    srv_t s;
    xsm_t m;
    xsm_cfg_t cfg;
    setup(&s, &m, &cfg);
    int k = 0;
    for (int i = 0; i < 4; i++) s.states[0][k++] = ST_WAIT;
    for (int i = 0; i < 3; i++) s.states[0][k++] = ST_WAIT_POS;
    s.states[0][k++] = ST_PROV;
    s.states[0][k++] = ST_READY;
    s.states[0][k++] = ST_DONE;
    s.nstates[0] = k;
    s.waittime_sec = 600;
    xsm_init(&m, &cfg, NULL);
    xsm_phase_t p = run(&m, &s, 120000);
    CHECK(p == XSM_P_DONE, "fase final %s", xsm_phase_name(p));
    CHECK(has_line("STATE\tQUEUE\tNa fila do xCloud há 0 s · estimativa ~10 min. Aguarde, o jogo abre sozinho."), "fila com ETA do /v1/waittime");
    CHECK(has_line("STATE\tQUEUE\tNa fila do xCloud há 4 s · posição 3 · estimativa ~10 min. Aguarde, o jogo abre sozinho."), "fila com posicao do /state");
    CHECK(s.n_wait == 1, "waittime 1x em 4s (a cada 30s): %d", s.n_wait);
    const mock_req_t *q = find_req("GET", "/v1/waittime/", 0);
    CHECK(q && !strcmp(q->path, "/v1/waittime/" TITLE), "path do waittime");
    CHECK(has_line("STATE\tSTREAMING\t" NAME), "chegou no streaming");
    no_secrets_in_logs();
    if (g_fail) dump_lines();
    xsm_free(&m);
}

static void test_failed(void) {
    printf("[fluxo] Failed (2 tentativas, depois ERROR)\n");
    srv_t s;
    xsm_t m;
    xsm_cfg_t cfg;
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_PROV; s.states[0][1] = ST_FAIL; s.nstates[0] = 2;
    s.states[1][0] = ST_FAIL; s.nstates[1] = 1;
    xsm_init(&m, &cfg, NULL);
    uint64_t t0 = g_now_ms;
    xsm_phase_t p = run(&m, &s, 120000);
    CHECK(p == XSM_P_ERROR, "fase final %s", xsm_phase_name(p));
    CHECK(s.n_play == 2 && s.n_delete == 2, "play=%d delete=%d", s.n_play, s.n_delete);
    CHECK(g_now_ms - t0 >= 3000, "cooldown de 3s entre tentativas");
    const mock_req_t *a = find_req("POST", "/play", 0), *b = find_req("POST", "/play", 1);
    CHECK(a && b && b->t - a->t >= 3000, "intervalo entre os /play");
    CHECK(g_nlines > 0 && !strcmp(g_lines[g_nlines - 1], "STATE\tERROR\tSessao xCloud falhou SessionProvisioningFailed"),
          "linha ERROR: %s", g_nlines ? g_lines[g_nlines - 1] : "");
    no_secrets_in_logs();
    if (g_fail) dump_lines();
    xsm_free(&m);
}

static void test_401(void) {
    printf("[fluxo] 401 -> renova login e repete\n");
    srv_t s;
    xsm_t m;
    xsm_cfg_t cfg;
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_PROV; s.states[0][1] = ST_READY; s.states[0][2] = ST_DONE; s.nstates[0] = 3;
    s.revoke_at_state = 2;
    xsm_init(&m, &cfg, NULL);
    xsm_phase_t p = run(&m, &s, 120000);
    CHECK(p == XSM_P_DONE, "fase final %s", xsm_phase_name(p));
    CHECK(s.n_refresh == 2 && s.n_login == 2, "refresh=%d login=%d", s.n_refresh, s.n_login);
    CHECK(mock_get_token() && !strcmp(mock_get_token(), "FAKE-REFRESH-2"), "refresh rotacionado 2x");
    int last401 = -1, after = 0;
    for (int i = 0; i < g_nreq; i++) if (g_reqs[i].status == 401) last401 = i;
    for (int i = last401 + 1; i < g_nreq; i++)
        if (!strcmp(g_reqs[i].host, REGION)) after += strstr(g_reqs[i].headers, "Bearer GS-2\r\n") != NULL ? 0 : 1;
    CHECK(last401 > 0 && after == 0, "depois do 401 tudo com o gsToken novo (%d errados)", after);
    CHECK(last401 > 0 && strstr(g_reqs[last401 + 1].host, "login.microsoftonline.com"), "401 -> refresh logo em seguida");
    CHECK(has_line("STATE\tSTREAMING\t" NAME), "chegou no streaming");
    no_secrets_in_logs();
    xsm_free(&m);

    printf("[fluxo] 401 persistente -> ERROR de login\n");
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_PROV; s.nstates[0] = 1;
    s.gs_always_bad = 1;
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 120000);
    CHECK(p == XSM_P_ERROR, "fase final %s", xsm_phase_name(p));
    CHECK(g_nlines > 0 && !strncmp(g_lines[g_nlines - 1], "STATE\tERROR\tLogin falhou: HTTP 401", 33), "linha: %s", g_nlines ? g_lines[g_nlines - 1] : "");
    CHECK(s.n_play == 0, "nao pediu play sem credencial valida");
    xsm_free(&m);

    printf("[fluxo] refresh invalid_grant -> ERROR login expirou\n");
    setup(&s, &m, &cfg);
    s.invalid_grant = 1;
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 120000);
    CHECK(p == XSM_P_ERROR && g_nlines > 0 && !strcmp(g_lines[g_nlines - 1], "STATE\tERROR\tLogin expirou: entre de novo no Xbox"),
          "linha: %s", g_nlines ? g_lines[g_nlines - 1] : "");
    xsm_free(&m);

    printf("[fluxo] sem TOKEN.TXT\n");
    setup(&s, &m, &cfg);
    mock_set_token(NULL);
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 120000);
    CHECK(p == XSM_P_ERROR && g_nreq == 0, "sem token nao sai pedido (%d)", g_nreq);
    xsm_free(&m);
}

static void test_timeouts(void) {
    printf("[fluxo] timeout: Provisioning sem fim\n");
    srv_t s;
    xsm_t m;
    xsm_cfg_t cfg;
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_PROV; s.nstates[0] = 1;
    s.states[1][0] = ST_PROV; s.nstates[1] = 1;
    xsm_init(&m, &cfg, NULL);
    uint64_t t0 = g_now_ms;
    xsm_phase_t p = run(&m, &s, 3600000);
    CHECK(p == XSM_P_ERROR, "fase final %s", xsm_phase_name(p));
    CHECK(g_now_ms - t0 >= 2 * 210000 + 3000 && g_now_ms - t0 < 2 * 240000, "tempo %llus",
          (unsigned long long)(g_now_ms - t0) / 1000);
    CHECK(s.n_play == 2 && s.n_delete == 2, "play=%d delete=%d", s.n_play, s.n_delete);
    CHECK(g_nlines > 0 && !strcmp(g_lines[g_nlines - 1], "STATE\tERROR\tTempo esgotado esperando o xCloud (Provisioning)"),
          "linha: %s", g_nlines ? g_lines[g_nlines - 1] : "");
    xsm_free(&m);

    printf("[fluxo] timeout: fila longa nao corta antes de 30 min\n");
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_WAIT; s.nstates[0] = 1;
    cfg.max_attempts = 1;
    xsm_init(&m, &cfg, NULL);
    t0 = g_now_ms;
    p = run(&m, &s, 4000000);
    CHECK(p == XSM_P_ERROR && g_now_ms - t0 >= 1800000, "fila: %llus", (unsigned long long)(g_now_ms - t0) / 1000);
    xsm_free(&m);

    printf("[fluxo] timeout: SDP sem answer\n");
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_READY; s.states[0][1] = ST_DONE; s.nstates[0] = 2;
    s.states[1][0] = ST_READY; s.states[1][1] = ST_DONE; s.nstates[1] = 2;
    s.sdp_204s = -1;
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 600000);
    CHECK(p == XSM_P_ERROR && g_nlines > 0 && !strcmp(g_lines[g_nlines - 1], "STATE\tERROR\tFalha na negociacao SDP: sem answer"),
          "linha: %s", g_nlines ? g_lines[g_nlines - 1] : "");
    CHECK(s.rtc_pcs == 2 && s.n_sdp == 2, "um PC/offer por tentativa (pcs=%d sdp=%d)", s.rtc_pcs, s.n_sdp);
    xsm_free(&m);

    printf("[fluxo] v0.21.10: prazo total do Conectando video (75 s)\n");
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_READY; s.states[0][1] = ST_DONE; s.nstates[0] = 2;
    s.sdp_204s = -1;
    cfg.sdp_timeout_s = 600;   /* so o prazo total vale */
    cfg.max_attempts = 1;
    xsm_init(&m, &cfg, NULL);
    t0 = g_now_ms;
    p = run(&m, &s, 900000);
    CHECK(p == XSM_P_ERROR && g_nlines > 0 &&
          !strncmp(g_lines[g_nlines - 1], "STATE\tERROR\tTempo esgotado conectando o video (SDP", 47),
          "linha: %s", g_nlines ? g_lines[g_nlines - 1] : "");
    CHECK(g_now_ms - t0 < 120000, "desistiu em %llus", (unsigned long long)(g_now_ms - t0) / 1000);
    xsm_free(&m);

    printf("[fluxo] timeout de rede no /state\n");
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_PROV; s.nstates[0] = 1;
    s.state_timeout = 1;
    cfg.max_attempts = 1;
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 600000);
    CHECK(p == XSM_P_ERROR && s.n_state == 5 && g_nlines > 0 &&
          !strcmp(g_lines[g_nlines - 1], "STATE\tERROR\tSem resposta do xCloud: timeout em state"),
          "n_state=%d linha: %s", s.n_state, g_nlines ? g_lines[g_nlines - 1] : "");
    xsm_free(&m);

    printf("[fluxo] midia morta (ICE ok, DTLS mudo) -> sessao nova\n");
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_READY; s.states[0][1] = ST_DONE; s.nstates[0] = 2;
    s.states[1][0] = ST_READY; s.states[1][1] = ST_DONE; s.nstates[1] = 2;
    s.rtc_dead_attempts = 1;
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 600000);
    CHECK(p == XSM_P_DONE && s.n_play == 2 && s.rtc_pcs == 2 && s.n_delete == 2, "fase %s play=%d pcs=%d delete=%d",
          xsm_phase_name(p), s.n_play, s.rtc_pcs, s.n_delete);
    CHECK(has_line("STATE\tSTREAMING\t" NAME), "streaming na 2a");
    xsm_free(&m);
}

static void test_active(void) {
    printf("[fluxo] sessao ativa: cancelar (padrao)\n");
    srv_t s;
    xsm_t m;
    xsm_cfg_t cfg;
    setup(&s, &m, &cfg);
    s.active_kind = 1;
    s.nsess = 1;   /* OLD vira a sessão 0 do servidor */
    snprintf(s.paths[0], sizeof(s.paths[0]), "/" OLD_PATH);
    s.states[0][0] = ST_DONE; s.nstates[0] = 1;
    s.states[1][0] = ST_READY; s.states[1][1] = ST_DONE; s.nstates[1] = 2;
    xsm_init(&m, &cfg, NULL);
    xsm_phase_t p = run(&m, &s, 120000);
    const mock_req_t *d = find_req("DELETE", OLD_PATH, 0);
    const mock_req_t *pl = find_req("POST", "/play", 0);
    CHECK(d && pl && d < pl, "DELETE da antiga antes do play");
    CHECK(p == XSM_P_DONE && s.n_play == 1 && has_line("STATE\tSTREAMING\t" NAME), "fase %s", xsm_phase_name(p));
    xsm_free(&m);

    printf("[fluxo] sessao ativa: retomar\n");
    setup(&s, &m, &cfg);
    s.active_kind = 1;
    s.nsess = 1;
    snprintf(s.paths[0], sizeof(s.paths[0]), "/" OLD_PATH);
    s.states[0][0] = ST_READY; s.states[0][1] = ST_DONE; s.nstates[0] = 2;
    cfg.active_policy = XSM_ACTIVE_RESUME;
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 120000);
    CHECK(p == XSM_P_DONE && s.n_play == 0 && s.n_connect == 1, "retomou sem play (play=%d connect=%d)", s.n_play, s.n_connect);
    const mock_req_t *st = find_req("GET", "/state", 0);
    CHECK(st && strstr(st->path, OLD_PATH), "poll na sessao antiga");
    CHECK(has_line("STATE\tSTARTING\tRetomando a sessao..."), "linha retomando");
    CHECK(has_line("STATE\tSTREAMING\t" NAME), "streaming");
    xsm_free(&m);

    printf("[fluxo] sessao ativa de OUTRO jogo com politica retomar -> cancela\n");
    setup(&s, &m, &cfg);
    s.active_kind = 2;
    s.nsess = 1;
    snprintf(s.paths[0], sizeof(s.paths[0]), "/" OLD_PATH);
    s.states[0][0] = ST_DONE; s.nstates[0] = 1;
    s.states[1][0] = ST_DONE; s.nstates[1] = 1;
    cfg.active_policy = XSM_ACTIVE_RESUME;
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 120000);
    CHECK(p == XSM_P_DONE && s.n_play == 1 && find_req("DELETE", OLD_PATH, 0), "cancelou e criou nova");
    xsm_free(&m);

    printf("[fluxo] sessao ativa: perguntar -> cancelar\n");
    setup(&s, &m, &cfg);
    s.active_kind = 1;
    s.nsess = 1;
    snprintf(s.paths[0], sizeof(s.paths[0]), "/" OLD_PATH);
    s.states[0][0] = ST_DONE; s.nstates[0] = 1;
    s.states[1][0] = ST_DONE; s.nstates[1] = 1;
    s.ask_answer = 0;
    cfg.active_policy = XSM_ACTIVE_ASK;
    xsm_init(&m, &cfg, NULL);
    p = run(&m, &s, 120000);
    CHECK(has_line("STATE\tSTARTING\tSessao ja ativa: A retoma, B cancela"), "perguntou");
    CHECK(p == XSM_P_DONE && s.n_play == 1 && find_req("DELETE", OLD_PATH, 0), "cancelou e criou nova");
    xsm_free(&m);

    printf("[fluxo] sessao ativa: perguntar e o usuario sai (B no menu = stop)\n");
    setup(&s, &m, &cfg);
    s.active_kind = 1;
    s.nsess = 1;
    snprintf(s.paths[0], sizeof(s.paths[0]), "/" OLD_PATH);
    cfg.active_policy = XSM_ACTIVE_ASK;
    xsm_init(&m, &cfg, NULL);
    for (int i = 0; i < 20; i++) { int d = xsm_step(&m); g_now_ms += (uint64_t)(d > 0 ? d : 0); }
    CHECK(xsm_phase(&m) == XSM_P_ASK, "esperando resposta");
    xsm_request_stop(&m);
    p = run(&m, &s, 10000);
    CHECK(p == XSM_P_DONE && s.n_play == 0 && s.n_delete == 0, "saiu sem mexer na sessao antiga");
    xsm_free(&m);
}

static void test_resignal(void) {
    printf("[fluxo] queda no meio do jogo -> re-sinaliza a mesma sessao\n");
    srv_t s;
    xsm_t m;
    xsm_cfg_t cfg;
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_READY; s.states[0][1] = ST_DONE; s.states[0][2] = ST_READY; s.states[0][3] = ST_DONE;
    s.nstates[0] = 4;
    s.rtc_drop_once = 1;
    xsm_init(&m, &cfg, NULL);
    xsm_phase_t p = run(&m, &s, 300000);
    CHECK(p == XSM_P_DONE, "fase %s", xsm_phase_name(p));
    CHECK(s.n_play == 1 && s.n_connect == 2 && s.n_sdp == 2 && s.rtc_pcs == 2, "play=%d connect=%d sdp=%d pcs=%d",
          s.n_play, s.n_connect, s.n_sdp, s.rtc_pcs);
    CHECK(has_line("STATE\tRECONNECTING\tReconectando: Preparando servidor..."), "linha RECONNECTING");
    CHECK(line_index("STATE\tRECONNECTING\tReconectando: Preparando servidor...") < g_nlines - 2 &&
          !strcmp(g_lines[g_nlines - 2], "STATE\tSTREAMING\t" NAME), "voltou pro STREAMING");
    if (g_fail) dump_lines();
    xsm_free(&m);
}

/* v0.19.2: net_live — a thread (aqui: xsm_step + xsm_try_handoff, como o
 * xsm_thread_run) vai até o answer e entrega; a "hw1" (xsm_net_step a cada
 * 5 ms virtuais, nunca avança o relógio sozinha) faz keepalive/ICE pelo
 * assíncrono; 401 no meio do STREAMING e o STOP devolvem pra thread. */
static xsm_phase_t run_net(xsm_t *m, srv_t *s, uint64_t limit_ms, int *thread_steps_live, int *net_steps) {
    uint64_t t0 = g_now_ms;
    unsigned seq = 0;
    for (int it = 0; it < 400000; it++) {
        int own = xsm_owner(m);
        int d = 5;
        if (own == XSM_OWN_DONE) break;
        if (own == XSM_OWN_WANT_THREAD) { xsm_set_owner(m, XSM_OWN_THREAD); own = XSM_OWN_THREAD; }
        if (own == XSM_OWN_THREAD) {
            d = xsm_step(m);
            if (xsm_live_phase(xsm_phase(m))) (*thread_steps_live)++;
            if (d < 0) { xs_creds_free(&m->creds); xsm_set_owner(m, XSM_OWN_DONE); }
            else if (xsm_try_handoff(m, g_now_ms)) d = 0;
        } else {
            CHECK(xsm_live_phase(xsm_phase(m)), "hw1 so dirige fase ao vivo (%s)", xsm_phase_name(xsm_phase(m)));
            uint64_t before = g_now_ms;
            xsm_net_step(m);
            CHECK(g_now_ms == before, "xsm_net_step nao espera");
            (*net_steps)++;
        }
        char line[200];
        unsigned sq = xsm_state_line(m, line, sizeof(line));
        if (sq != seq && g_nlines < 256) { seq = sq; snprintf(g_lines[g_nlines++], sizeof(g_lines[0]), "%s", line); }
        rtc_tick(m, s);
        if (g_now_ms - t0 > limit_ms) { printf("  (limite de tempo virtual)\n"); break; }
        g_now_ms += (uint64_t)(d > 0 ? d : (own == XSM_OWN_NET ? 5 : 0));
    }
    return xsm_phase(m);
}

static void test_net_live(void) {
    printf("[v0.19.2] fases ao vivo na hw1: keepalive assincrono, 401 no jogo, STOP\n");
    srv_t s;
    xsm_t m;
    xsm_cfg_t cfg;
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_PROV; s.states[0][1] = ST_READY; s.states[0][2] = ST_DONE; s.nstates[0] = 3;
    s.ice_204s = 1;
    s.stop_after_ms = 40000;
    s.revoke_at_ka = 4;
    s.late_local = 1;
    cfg.net_live = 1;
    xsm_init(&m, &cfg, NULL);
    int tl = 0, ns = 0;
    xsm_phase_t p = run_net(&m, &s, 300000, &tl, &ns);
    CHECK(p == XSM_P_DONE, "fase final %s", xsm_phase_name(p));
    CHECK(xsm_owner(&m) == XSM_OWN_DONE, "owner DONE");
    CHECK(has_line("STATE\tSTREAMING\t" NAME), "chegou no streaming");
    CHECK(m.handoffs == 2, "entregou 2x pra hw1 (answer, depois do re-login): %u", m.handoffs);
    CHECK(m.escalations == 2, "devolveu 2x (401, stop): %u", m.escalations);
    CHECK(s.n_login == 2 && s.n_refresh == 2, "re-login pelo 401 do keepalive (login=%d refresh=%d)", s.n_login, s.n_refresh);
    CHECK(s.n_delete == 1, "DELETE 1x (%d)", s.n_delete);
    int ka_nb = 0, ka_thr = 0, ice_nb = 0, conn_ids_ok = 1, bad_tok = 0, seen401 = 0;
    for (int i = 0; i < g_nreq; i++) {
        const mock_req_t *q = &g_reqs[i];
        if (!strcmp(q->host, REGION) && q->conn_id != 1) conn_ids_ok = 0;
        if (strstr(q->path, "/keepalive")) { if (q->nb) ka_nb++; else ka_thr++; }
        if (strstr(q->path, "/ice") && q->nb) ice_nb++;
        if (q->status == 401) seen401 = 1;
        else if (seen401 && !strcmp(q->host, REGION) && !strstr(q->headers, "Bearer GS-2\r\n")) bad_tok++;
    }
    CHECK(ka_nb >= 6, "keepalives pela hw1: %d", ka_nb);
    CHECK(ka_thr == 2, "keepalive refeito pela thread (401 de novo -> re-login -> repete): %d", ka_thr);
    CHECK(ice_nb >= 1, "ICE (candidato tardio) enviado pela hw1 no CONNECTING: %d", ice_nb);
    CHECK(conn_ids_ok && g_mock_conns == 1, "uma conexao keep-alive pra regiao a sessao toda (%d)", g_mock_conns);
    CHECK(bad_tok == 0, "depois do 401 tudo com GS-2 (%d errados)", bad_tok);
    CHECK(tl <= 4, "thread quase nao roda fase ao vivo (%d passos)", tl);
    CHECK(ns > 1000, "hw1 dirigiu (%d passos)", ns);
    no_secrets_in_logs();
    if (g_fail) dump_lines();
    xsm_free(&m);

    printf("[v0.19.2] queda no meio do jogo com a hw1 dirigindo -> thread re-sinaliza e devolve\n");
    setup(&s, &m, &cfg);
    s.states[0][0] = ST_READY; s.states[0][1] = ST_DONE; s.states[0][2] = ST_READY; s.states[0][3] = ST_DONE;
    s.nstates[0] = 4;
    s.rtc_drop_once = 1;
    cfg.net_live = 1;
    xsm_init(&m, &cfg, NULL);
    tl = ns = 0;
    p = run_net(&m, &s, 300000, &tl, &ns);
    CHECK(p == XSM_P_DONE, "fase %s", xsm_phase_name(p));
    CHECK(s.n_play == 1 && s.n_connect == 2 && s.n_sdp == 2 && s.rtc_pcs == 2, "play=%d connect=%d sdp=%d pcs=%d",
          s.n_play, s.n_connect, s.n_sdp, s.rtc_pcs);
    CHECK(m.handoffs == 2 && m.escalations == 2, "handoffs %u escalations %u", m.handoffs, m.escalations);
    CHECK(has_line("STATE\tRECONNECTING\tReconectando: Preparando servidor..."), "linha RECONNECTING");
    CHECK(!strcmp(g_lines[g_nlines - 2], "STATE\tSTREAMING\t" NAME), "voltou pro STREAMING");
    if (g_fail) dump_lines();
    xsm_free(&m);
}

static void test_home(void) {
    printf("[remote play] consoles, xhome auth, SDP/ICE, keepalive, reauth and DELETE\n");
    xs_console_t consoles[XS_MAX_CONSOLES];
    CHECK(xs_parse_consoles("{\"results\":[]}", consoles, 16) == 0, "empty console list");
    CHECK(xs_parse_consoles("{}", consoles, 16) == -1, "missing results is an error");
    CHECK(xs_parse_consoles("bad json", consoles, 16) == -1, "invalid JSON");
    const char *list = "{\"results\":[null,{},"
        "{\"serverId\":\"../bad\"},{\"serverId\":\"F1\",\"deviceName\":\"Sala\"},"
        "{\"serverId\":\"F1\"},{\"serverId\":\"F2\"}]}";
    CHECK(xs_parse_consoles(list, consoles, 16) == 2, "skip malformed/duplicate console IDs");
    CHECK(!strcmp(consoles[0].name, "Sala") && !strcmp(consoles[1].name, "Meu Xbox"), "console name fallback");
    CHECK(xs_parse_consoles(list, consoles, 1) == 1, "console count capped");
    char *b = xs_build_home_body("F400TEST001", "pt-BR");
    cJSON *j = cJSON_Parse(b);
    CHECK(j && !strcmp(cJSON_GetObjectItem(j, "serverId")->valuestring, "F400TEST001"), "home play sends console ID");
    CHECK(j && !strcmp(cJSON_GetObjectItem(j, "titleId")->valuestring, ""), "home play has no cloud title");
    cJSON_Delete(j); free(b);

    srv_t s; xsm_t m; xsm_cfg_t cfg;
    setup(&s, &m, &cfg); s.home = 1;
    xs_creds_t c;
    xs_creds_init(&c, "xhome");
    xc_err_t e = {0};
    CHECK(xs_creds_load(&c, &e) == XS_OK && xs_creds_login(&c, &e) == XS_OK, "xhome credentials");
    int n = -1;
    CHECK(xs_home_list(&c, consoles, 16, &n, &e) == XS_OK && n == 1, "discover console over HTTPS");
    CHECK(!strcmp(consoles[0].id, "F400TEST001") && !strcmp(c.host, HOME_REGION), "xhome region without isDefault");
    xs_session_t session; xs_init(&session, &c);
    CHECK(xs_home_start(&session, "../invalid", &e) == XS_ERR && s.n_play == 0, "unsafe ID never sent");
    xs_creds_free(&c);

    setup(&s, &m, &cfg); s.home = 1;
    cfg.home = 1; cfg.waittime = 0;
    snprintf(cfg.title_id, sizeof(cfg.title_id), "F400TEST001");
    s.states[0][0] = ST_PROV; s.states[0][1] = ST_READY; s.states[0][2] = ST_DONE;
    s.nstates[0] = 3;
    s.revoke_at_state = 1;
    xsm_init(&m, &cfg, NULL);
    CHECK(run(&m, &s, 120000) == XSM_P_DONE, "remote play completes lifecycle");
    CHECK(s.n_play == 1 && s.n_sdp == 1 && s.n_ice > 0 && s.n_ka > 0 && s.n_delete == 1, "home streaming and teardown");
    CHECK(s.n_login == 2 && s.n_connect == 0 && s.n_passport == 0 && s.n_active == 0, "home reauth; no cloud transfer or stale-session cleanup");
    CHECK(s.answers_ok == 1 && s.nremotes > 0, "home SDP and ICE reach existing RTC transport");
    for (int i = 0; i < g_nreq; i++) {
        CHECK(!strstr(g_reqs[i].path, "/sessions/cloud/"), "no cloud session requests");
        if (!strcmp(g_reqs[i].path, "/v2/login/user"))
            CHECK(!strcmp(g_reqs[i].host, "xhome.gssv-play-prod.xboxlive.com") &&
                  strstr(g_reqs[i].body, "\"offeringId\":\"xhome\""), "home reauth preserves offering");
    }
    no_secrets_in_logs();
    xsm_free(&m);

    setup(&s, &m, &cfg); s.home = 1; cfg.home = 1; cfg.waittime = 0; cfg.max_attempts = 1;
    s.states[0][0] = "{\"state\":\"Failed\",\"errorDetails\":{\"code\":\"ConsoleNotReachable\"}}";
    s.nstates[0] = 1;
    xsm_init(&m, &cfg, NULL);
    CHECK(run(&m, &s, 120000) == XSM_P_ERROR && strstr(m.last_err, "ConsoleNotReachable"), "offline console surfaces error");
    CHECK(s.n_delete == 1 && s.n_passport == 0, "failed home session cleaned up");
    xsm_free(&m);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_verbose = getenv("XS_VERBOSE") != NULL;
    test_pure();
    test_home();
    test_happy();
    test_queue();
    test_failed();
    test_401();
    test_timeouts();
    test_active();
    test_resignal();
    test_net_live();
    mock_reset();
    printf("%d checagens, %d falhas\n", g_checks, g_fail);
    printf(g_fail ? "SESSION HOST TESTS FAILED\n" : "SESSION HOST TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
