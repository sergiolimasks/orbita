/* XCloud360 — partes puras do xc_catalog (testadas no host). Ver xc_catalog.h. */
#include "xc_catalog.h"
#include "../login/cjson/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IMG_HOST "store-images.s-microsoft.com"

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static void copy_upper(char *dst, size_t cap, const char *s) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) {
        char c = s[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    dst[i] = 0;
}

/* bigId: 12 alfanuméricos hoje; aceita até 23, só [A-Za-z0-9]. */
static int valid_product(const char *s) {
    size_t n = strlen(s);
    if (n < 4 || n > 23) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

int xcc_parse_sigl(const char *json, char (**ids)[24], int *n, char *title, size_t tcap) {
    *ids = NULL;
    *n = 0;
    if (title && tcap) title[0] = 0;
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) return -1;
    const cJSON *arr = j;
    if (cJSON_IsObject(j)) {   /* forma alternativa {"products":[{"id":..}]} */
        arr = cJSON_GetObjectItemCaseSensitive(j, "products");
        if (!arr) arr = cJSON_GetObjectItemCaseSensitive(j, "items");
    }
    if (!cJSON_IsArray(arr)) { cJSON_Delete(j); return -1; }
    int cap = cJSON_GetArraySize(arr);
    char (*out)[24] = (char (*)[24])calloc((size_t)(cap > 0 ? cap : 1), 24);
    if (!out) { cJSON_Delete(j); return -1; }
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const char *id = jstr(it, "id");
        if (!id) {
            const char *t = jstr(it, "title");
            if (t && title && tcap && jstr(it, "siglId")) xa_ascii_fold(title, tcap, t);
            continue;
        }
        if (!valid_product(id)) continue;
        char up[24];
        copy_upper(up, sizeof(up), id);
        int dup = 0;   /* listas pequenas (<4000) e chamadas raras */
        for (int i = k - 1; i >= 0 && i >= k - 64; i--) if (!strcmp(out[i], up)) { dup = 1; break; }
        if (dup) continue;
        memcpy(out[k++], up, 24);
    }
    cJSON_Delete(j);
    *ids = out;
    *n = k;
    return 0;
}

/* "//store-images.s-microsoft.com/image/apps.1..." -> "/image/apps.1...".
 * Outro host (images-eds...) = não serve: só falamos com o store-images. */
static int img_path(char *dst, size_t cap, const char *uri) {
    const char *u = uri;
    if (!strncmp(u, "https:", 6)) u += 6;
    else if (!strncmp(u, "http:", 5)) u += 5;
    if (strncmp(u, "//", 2)) return -1;
    u += 2;
    size_t hl = strlen(IMG_HOST);
    if (strncasecmp(u, IMG_HOST, hl) || u[hl] != '/') return -1;
    u += hl;
    const char *q = strchr(u, '?');
    size_t n = q ? (size_t)(q - u) : strlen(u);
    if (n + 1 > cap) return -1;
    for (size_t i = 0; i < n; i++) if ((unsigned char)u[i] <= ' ' || u[i] == '\t') return -1;
    memcpy(dst, u, n);
    dst[n] = 0;
    return 0;
}

/* Preferência: capa retrato 2:3 primeiro; hero 16:9 sem texto primeiro (o
 * menu escreve o nome por cima). */
static int cover_rank(const char *p) {
    static const char *k[] = { "Poster", "BoxArt", "BrandedKeyArt", "FeaturePromotionalSquareArt" };
    for (int i = 0; i < 4; i++) if (!strcmp(p, k[i])) return i;
    return -1;
}
static int hero_rank(const char *p) {
    static const char *k[] = { "SuperHeroArt", "TitledHeroArt", "Screenshot" };
    for (int i = 0; i < 3; i++) if (!strcmp(p, k[i])) return i;
    return -1;
}

int xcc_parse_images(const char *json, xcc_imgref_t **out, int *n) {
    *out = NULL;
    *n = 0;
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) return -1;
    const cJSON *prods = cJSON_GetObjectItemCaseSensitive(j, "Products");
    int cap = cJSON_GetArraySize(prods);
    xcc_imgref_t *r = (xcc_imgref_t *)calloc((size_t)(cap > 0 ? cap : 1), sizeof(*r));
    if (!r) { cJSON_Delete(j); return -1; }
    int k = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, prods) {
        const char *pid = jstr(p, "ProductId");
        if (!pid || !valid_product(pid)) continue;
        xcc_imgref_t *o = &r[k];
        memset(o, 0, sizeof(*o));
        copy_upper(o->product, sizeof(o->product), pid);
        int best_c = 99, best_h = 99;
        const cJSON *lp = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "LocalizedProperties"), 0);
        const cJSON *im, *images = lp ? cJSON_GetObjectItemCaseSensitive(lp, "Images") : NULL;
        cJSON_ArrayForEach(im, images) {
            const char *purpose = jstr(im, "ImagePurpose"), *uri = jstr(im, "Uri");
            if (!purpose || !uri) continue;
            int rc = cover_rank(purpose), rh = hero_rank(purpose);
            if (rc >= 0 && rc < best_c && img_path(o->cover, sizeof(o->cover), uri) == 0) best_c = rc;
            if (rh >= 0 && rh < best_h && img_path(o->hero, sizeof(o->hero), uri) == 0) best_h = rh;
        }
        if (best_c == 99) o->cover[0] = 0;
        if (best_h == 99) o->hero[0] = 0;
        k++;
    }
    cJSON_Delete(j);
    *out = r;
    *n = k;
    return 0;
}

void xcc_image_path(char *dst, size_t cap, const char *path, int kind) {
    /* Confirmado no store-images (2026-10-25): w/h com mode=crop = tamanho
     * exato (preenche e corta no centro; sem mode = "cabe dentro", mantém a
     * proporção); q = qualidade JPEG; format=jpg força JPEG (sem ele uma
     * arte de origem PNG volta PNG RGBA de ~550 KB); sai baseline (stb lê).
     * format=jpeg / fmt= dão erro 400 (texto). */
    if (kind == XCC_HERO)
        snprintf(dst, cap, "%s?w=%d&h=%d&q=70&mode=crop&format=jpg", path, XCC_HERO_W, XCC_HERO_H);
    else
        snprintf(dst, cap, "%s?w=%d&h=%d&q=75&mode=crop&format=jpg", path, XCC_COVER_W, XCC_COVER_H);
}

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

void xcc_file_name(char *dst, size_t cap, const char *product, int kind) {
    /* FatFs sem LFN: 8.3. O bigId (12) não cabe; hash de 32 bits + trailer
     * com o bigId no arquivo (conferido na leitura) cobre colisões. */
    snprintf(dst, cap, "%s/%08X.JPG", kind == XCC_HERO ? "HERO" : "COVERS", (unsigned)fnv1a(product));
}
