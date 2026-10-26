/* XCloud360 — JPEG (stb_image, só JPEG, C puro) -> pixels XCC_PIX. */
#include "xc_catalog.h"

#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD          /* nada de SSE/NEON; no PPC é C puro de qualquer jeito */
#define STBI_NO_THREAD_LOCALS /* newlib do libxenon: sem TLS; só o worker decodifica */
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
/* (STB_IMAGE_STATIC: as não usadas viram "static sem uso" no fim do arquivo;
 * por isso o -Wunused-function fica desligado até o fim, sem pop.) */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "stb_image.h"
#pragma GCC diagnostic pop

uint32_t *xcc_decode_jpeg(const uint8_t *jpg, size_t len, int w, int h) {
    if (!jpg || len < 4 || len > (64u << 20) || w <= 0 || h <= 0) return NULL;
    int sw = 0, sh = 0, comp = 0;
    unsigned char *rgb = stbi_load_from_memory(jpg, (int)len, &sw, &sh, &comp, 3);
    if (!rgb) return NULL;
    uint32_t *out = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!out) { stbi_image_free(rgb); return NULL; }
    if (sw == w && sh == h) {
        const unsigned char *s = rgb;
        for (int i = 0; i < w * h; i++, s += 3) out[i] = XCC_PIX(s[0], s[1], s[2]);
    } else {
        /* Preenche e corta no centro (como o mode=crop do servidor), média
         * da área de origem de cada pixel (reduzir sem serrilhado). */
        long long cw = sw, ch = sh;   /* janela de origem com a proporção de w:h */
        if (cw * h > ch * w) cw = ch * w / h;
        else ch = cw * h / w;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        int ox = (int)((sw - cw) / 2), oy = (int)((sh - ch) / 2);
        for (int y = 0; y < h; y++) {
            int y0 = oy + (int)(ch * y / h), y1 = oy + (int)(ch * (y + 1) / h);
            if (y1 <= y0) y1 = y0 + 1;
            if (y1 > sh) y1 = sh;
            for (int x = 0; x < w; x++) {
                int x0 = ox + (int)(cw * x / w), x1 = ox + (int)(cw * (x + 1) / w);
                if (x1 <= x0) x1 = x0 + 1;
                if (x1 > sw) x1 = sw;
                unsigned r = 0, g = 0, b = 0, cnt = 0;
                for (int yy = y0; yy < y1; yy++) {
                    const unsigned char *s = rgb + ((size_t)yy * sw + x0) * 3;
                    for (int xx = x0; xx < x1; xx++, s += 3) { r += s[0]; g += s[1]; b += s[2]; cnt++; }
                }
                if (!cnt) cnt = 1;
                out[(size_t)y * w + x] = XCC_PIX(r / cnt, g / cnt, b / cnt);
            }
        }
    }
    stbi_image_free(rgb);
    return out;
}
