/* XCloud360 — hosttest/ui: grava o canvas XRGB como PNG (zlib do sistema). */
#include "xui_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len) {
    unsigned char h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    be32(h, (uint32_t)crc);
    fwrite(h, 1, 4, f);
}

int xui_png_write(const char *path, const uint32_t *px, int w, int h, int stride) {
    size_t raw_n = (size_t)(w * 3 + 1) * h;
    unsigned char *raw = malloc(raw_n);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        unsigned char *r = raw + (size_t)y * (w * 3 + 1);
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * stride + x];
            r[1 + x * 3] = (unsigned char)(p >> 16);
            r[2 + x * 3] = (unsigned char)(p >> 8);
            r[3 + x * 3] = (unsigned char)p;
        }
    }
    uLongf zn = compressBound(raw_n);
    unsigned char *z = malloc(zn);
    if (!z || compress2(z, &zn, raw, raw_n, 6) != Z_OK) { free(raw); free(z); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}
