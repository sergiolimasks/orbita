/*
 * XCloud360 v0.17 — testes de unidade do login (sem rede): parser HTTP
 * (Content-Length, chunked byte a byte, até EOF, erros), data HTTP, DNS,
 * JSON de /v2/titles e displaycatalog, f2p, ordenação, ASCII, cache.
 * Opcional: argv[1] = catalog.json do launcher (só ids/nomes) -> monta um
 * /v2/titles sintético com esses ids e confere a contagem.
 */
#include "../../source/login/http_parse.h"
#include "../../source/login/dns_msg.h"
#include "../../source/login/xbox_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int feed_all(http_parser_t *h, const char *s, size_t step) {
    size_t n = strlen(s);
    int r = HP_OK_MORE;
    for (size_t i = 0; i < n && r == HP_OK_MORE; i += step) {
        size_t k = n - i < step ? n - i : step;
        r = hp_feed(h, (const uint8_t *)s + i, k);
    }
    return r;
}

static void test_http(void) {
    http_parser_t *h = malloc(sizeof(*h));
    const char *cl = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nDate: Sat, 24 Oct 2026 22:30:00 GMT\r\n"
                     "Content-Length: 11\r\n\r\n{\"a\":\"xyz\"}EXTRA";
    for (size_t step = 1; step <= 7; step += 3) {
        hp_init(h, 1024);
        CHECK(feed_all(h, cl, step) == HP_DONE);
        CHECK(h->status == 200 && h->body_len == 11 && !strcmp((char *)h->body, "{\"a\":\"xyz\"}"));
        CHECK(!strcmp(h->date, "Sat, 24 Oct 2026 22:30:00 GMT"));
        CHECK(!strcmp(h->content_type, "application/json"));
        hp_free(h);
    }
    const char *ch = "HTTP/1.1 400 Bad Request\r\ntransfer-encoding: chunked\r\n\r\n"
                     "5\r\nhello\r\n1A;ext=1\r\n, chunked world 0123456789\r\n0\r\nX-Trailer: 1\r\n\r\n";
    for (size_t step = 1; step <= 64; step *= 2) {
        hp_init(h, 1024);
        CHECK(feed_all(h, ch, step) == HP_DONE);
        CHECK(h->status == 400);
        CHECK(h->body_len == 31 && !strcmp((char *)h->body, "hello, chunked world 0123456789"));
        hp_free(h);
    }
    /* sem trailer */
    hp_init(h, 1024);
    CHECK(feed_all(h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n", 5) == HP_DONE);
    CHECK(h->body_len == 3);
    hp_free(h);
    /* corpo até o EOF */
    hp_init(h, 1024);
    CHECK(feed_all(h, "HTTP/1.0 200 OK\r\n\r\nate o fim", 4) == HP_OK_MORE);
    CHECK(hp_eof(h) == HP_DONE && !strcmp((char *)h->body, "ate o fim"));
    hp_free(h);
    /* 204 / Content-Length 0 */
    hp_init(h, 1024);
    CHECK(feed_all(h, "HTTP/1.1 204 No Content\r\n\r\n", 3) == HP_DONE && h->body_len == 0);
    hp_free(h);
    hp_init(h, 1024);
    CHECK(feed_all(h, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 1) == HP_DONE && h->body && h->body[0] == 0);
    hp_free(h);
    /* 100 Continue antes */
    hp_init(h, 1024);
    CHECK(feed_all(h, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok", 2) == HP_DONE);
    CHECK(h->status == 201 && !strcmp((char *)h->body, "ok"));
    hp_free(h);
    /* erros */
    hp_init(h, 1024);
    CHECK(feed_all(h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n", 1) == HP_ERR);
    hp_free(h);
    hp_init(h, 8);
    CHECK(feed_all(h, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", 1) == HP_ERR_TOOBIG);
    hp_free(h);
    hp_init(h, 8);
    CHECK(feed_all(h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\n0123456789abcdef\r\n0\r\n\r\n", 3) == HP_ERR_TOOBIG);
    hp_free(h);
    hp_init(h, 1024);
    CHECK(feed_all(h, "garbage\r\n\r\n", 1) == HP_ERR);
    hp_free(h);
    hp_init(h, 1024);   /* truncado no meio do chunk: EOF é erro */
    CHECK(feed_all(h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nabc", 1) == HP_OK_MORE);
    CHECK(hp_eof(h) == HP_ERR);
    hp_free(h);
    free(h);

    CHECK(hp_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777LL);
    CHECK(hp_parse_http_date("Sat, 24 Oct 2026 22:30:00 GMT") == 1792881000LL);
    CHECK(hp_parse_http_date("Thu, 29 Feb 2024 00:00:00 GMT") == 1709164800LL);
    CHECK(hp_parse_http_date("lixo") == -1);
}

static void test_dns(void) {
    uint8_t q[512];
    int n = dns_build_query(q, sizeof(q), 0x1234, "xsts.auth.xboxlive.com");
    CHECK(n == 12 + 24 + 4);
    CHECK(q[12] == 4 && !memcmp(q + 13, "xsts", 4) && q[17] == 4);
    uint8_t q2[64];
    CHECK(dns_build_query(q2, sizeof(q2), 1, "a..b") < 0);
    /* resposta: pergunta + CNAME + A (com compressão) */
    uint8_t r[512];
    memcpy(r, q, (size_t)n);
    r[2] = 0x81; r[3] = 0x80; r[7] = 2;
    size_t o = (size_t)n;
    uint8_t cname[] = { 0xC0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 6, 3, 'f', 'o', 'o', 0xC0, 17 };
    memcpy(r + o, cname, sizeof(cname)); o += sizeof(cname);
    uint8_t a[] = { 0xC0, (uint8_t)(n + 12), 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 20, 190, 159, 1 };
    memcpy(r + o, a, sizeof(a)); o += sizeof(a);
    uint32_t ip = 0;
    int rc = -9;
    CHECK(dns_parse_response(r, o, 0x1234, &ip, &rc) == 0 && ip == 0x14BE9F01 && rc == 0);
    CHECK(dns_parse_response(r, o, 0x9999, &ip, &rc) == -1);
    CHECK(dns_parse_response(r, o - 3, 0x1234, &ip, &rc) == -1);
    r[3] = 0x83; r[7] = 0;   /* NXDOMAIN */
    CHECK(dns_parse_response(r, (size_t)n, 0x1234, &ip, &rc) == 1 && rc == 3);
}

static const char *TITLES =
    "{\"totalItems\":5,\"results\":["
    "{\"titleId\":\"FORZAHORIZON5\",\"details\":{\"productId\":\"9NKX70BBCDRN\",\"hasEntitlement\":false,"
      "\"programs\":[\"GPULTIMATE\",\"CALLISTO\"],\"userSubscriptions\":[\"GPULTIMATE\"],\"maxSessionLengthInSeconds\":0}},"
    "{\"titleId\":\"OWNEDGAME\",\"details\":{\"productId\":\"9AAAAAAAAAAA\",\"hasEntitlement\":true,\"programs\":[\"BYOG\"]}},"
    "{\"titleId\":\"NOTMINE\",\"details\":{\"productId\":\"9BBBBBBBBBBB\",\"hasEntitlement\":false,\"programs\":[\"GPULTIMATE\"],\"userSubscriptions\":[]}},"
    "{\"titleId\":\"FORTNITE\",\"details\":{\"productId\":\"BT5P2X999VH2\",\"programs\":[\"F2P\"],\"userPrograms\":[]}},"
    "{\"titleId\":\"FORZAHORIZON5\",\"details\":{\"productId\":\"9NKX70BBCDRN\"}},"
    "{\"details\":{}}"
    "]}";
static const char *TITLES_F2P =
    "{\"results\":["
    "{\"titleId\":\"FORTNITE\",\"details\":{\"productId\":\"BT5P2X999VH2\",\"programs\":[\"F2P\"],\"userPrograms\":[\"F2P\"],\"maxSessionLengthInSeconds\":3600}},"
    "{\"titleId\":\"ADONLY\",\"details\":{\"productId\":\"9CCCCCCCCCCC\",\"programs\":[\"F2P\"],\"userPrograms\":[\"F2P\"]}},"
    "{\"titleId\":\"ADNOTGRANTED\",\"details\":{\"productId\":\"9DDDDDDDDDDD\",\"programs\":[\"F2P\"],\"userPrograms\":[]}}"
    "]}";
static const char *NAMES =
    "{\"Products\":["
    "{\"ProductId\":\"9NKX70BBCDRN\",\"LocalizedProperties\":[{\"ProductTitle\":\"Forza Horizon 5 Standard Edition\"}]},"
    "{\"ProductId\":\"9AAAAAAAAAAA\",\"LocalizedProperties\":[{\"ProductTitle\":\"Pok\\u00e9mon\\u2122 Caf\\u00e9 \\u00ae  Edi\\u00e7\\u00e3o  \"}]},"
    "{\"ProductId\":\"bt5p2x999vh2\",\"LocalizedProperties\":[{\"ProductTitle\":\"Fortnite\"}]},"
    "{\"ProductId\":\"9CCCCCCCCCCC\",\"LocalizedProperties\":[]}"
    "]}";

static xa_title_t *find(xa_title_t *t, int n, const char *id) {
    for (int i = 0; i < n; i++) if (!strcmp(t[i].id, id)) return &t[i];
    return NULL;
}

static void test_json(void) {
    xa_title_t *t = NULL, *ads = NULL;
    int n = 0, na = 0;
    CHECK(xa_parse_titles(TITLES, &t, &n) == 0 && n == 4);
    xa_title_t *fh = find(t, n, "FORZAHORIZON5"), *ow = find(t, n, "OWNEDGAME"), *nm = find(t, n, "NOTMINE"),
               *fn = find(t, n, "FORTNITE");
    CHECK(fh && (fh->flags & XA_F_INSUB) && (fh->flags & XA_F_SUB) && !strcmp(fh->product, "9NKX70BBCDRN"));
    CHECK(ow && (ow->flags & XA_F_OWNED) && !(ow->flags & XA_F_SUB));
    CHECK(nm && !xa_playable(nm) && (nm->flags & XA_F_SUB));
    CHECK(fn && (fn->flags & XA_F_ADSUP) && !xa_playable(fn));
    CHECK(xa_parse_titles(TITLES_F2P, &ads, &na) == 0 && na == 3);
    CHECK(xa_merge_f2p(&t, &n, ads, na) == 0 && n == 5);
    fn = find(t, n, "FORTNITE");
    xa_title_t *ad = find(t, n, "ADONLY");
    CHECK(fn && xa_playable(fn) && (fn->flags & XA_F_ADGRANT));
    CHECK(ad && xa_playable(ad) && !(ad->flags & XA_F_OWNED));
    CHECK(!find(t, n, "ADNOTGRANTED"));
    CHECK(xa_parse_names(NAMES, t, n) == 3);
    fh = find(t, n, "FORZAHORIZON5");
    ow = find(t, n, "OWNEDGAME");
    fn = find(t, n, "FORTNITE");
    CHECK(fh && !strcmp(fh->name, "Forza Horizon 5 Standard Edition"));
    CHECK(ow && !strcmp(ow->name, "Pokemon Cafe Edicao"));
    CHECK(fn && !strcmp(fn->name, "Fortnite"));
    /* cache ida e volta */
    char *cache = xa_names_to_cache(t, n);
    CHECK(cache && strstr(cache, "9NKX70BBCDRN\tForza Horizon 5 Standard Edition\n"));
    xa_title_t *t2 = NULL;
    int n2 = 0;
    xa_parse_titles(TITLES, &t2, &n2);
    CHECK(xa_names_from_cache(cache, t2, n2) == 3);
    CHECK(!strcmp(find(t2, n2, "OWNEDGAME")->name, "Pokemon Cafe Edicao"));
    free(cache);
    free(t2);
    int k = xa_finalize(t, n);
    CHECK(k == 4);
    /* ordem por nome sem caixa; sem nome -> id */
    CHECK(!strcmp(t[0].id, "ADONLY") && !strcmp(t[1].id, "FORTNITE") && !strcmp(t[2].id, "FORZAHORIZON5") &&
          !strcmp(t[3].id, "OWNEDGAME"));
    free(t);
    free(ads);
    CHECK(xa_parse_titles("{nao json", &t, &n) == -1);
    CHECK(xa_parse_titles("{\"results\":[]}", &t, &n) == 0 && n == 0);
    free(t);
}

static void test_text(void) {
    char b[64];
    xa_ascii_fold(b, sizeof(b), "  Assassin\xe2\x80\x99s  Creed\xc2\xae Shadows\xe2\x84\xa2 ");
    CHECK(!strcmp(b, "Assassins Creed Shadows"));
    xa_ascii_fold(b, 11, "abcdefghijklmnop");
    CHECK(!strcmp(b, "abcdefg..."));
    xa_ascii_fold(b, sizeof(b), "\xc3\x80\xc3\xa7\xc3\xb1\xc3\xbf\xc3\x9f!");
    CHECK(!strcmp(b, "Acny!"));
    xa_ascii_fold(b, sizeof(b), "bad\xff\xc3utf8\xe2\x82");
    CHECK(!strcmp(b, "badutf8"));
    char *u = xa_urlencode("xboxlive.signin openid M.C5_B!*$~");
    CHECK(u && !strcmp(u, "xboxlive.signin%20openid%20M.C5_B%21%2A%24~"));
    free(u);
}

/* Monta um /v2/titles a partir do catalog.json do launcher (ids reais). */
static void test_catalog_json(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("SKIP catalog.json (%s)\n", path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)sz + 1);
    sz = (long)fread(s, 1, (size_t)sz, f);
    s[sz] = 0;
    fclose(f);
    size_t cap = (size_t)sz * 4 + 64, w = 0;
    char *body = malloc(cap);
    w += (size_t)snprintf(body + w, cap - w, "{\"results\":[");
    int ids = 0;
    for (char *p = strstr(s, "\"id\": \""); p; p = strstr(p, "\"id\": \"")) {
        p += 7;
        char *e = strchr(p, '"');
        if (!e) break;
        w += (size_t)snprintf(body + w, cap - w, "%s{\"titleId\":\"%.*s\",\"details\":{\"productId\":\"P%d\","
                              "\"hasEntitlement\":true}}", ids ? "," : "", (int)(e - p), p, ids);
        ids++;
        p = e;
    }
    snprintf(body + w, cap - w, "]}");
    xa_title_t *t = NULL;
    int n = 0;
    CHECK(xa_parse_titles(body, &t, &n) == 0);
    CHECK(n > 0 && n <= ids);   /* ids repetidos no arquivo viram 1 */
    CHECK(xa_finalize(t, n) == n);
    printf("catalog.json: %d ids do launcher -> %d titulos jogaveis (%u bytes de JSON sintetico)\n", ids, n,
           (unsigned)w);
    free(t);
    free(body);
    free(s);
}

int main(int argc, char **argv) {
    test_http();
    test_dns();
    test_json();
    test_text();
    if (argc > 1) test_catalog_json(argv[1]);
    printf("login_unit: %d checks, %d falhas\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
