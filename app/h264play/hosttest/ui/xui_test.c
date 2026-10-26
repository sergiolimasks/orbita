/*
 * XCloud360 — testes de unidade do XUI no host: UTF-8 -> Latin-1, fonte
 * (acentos existem, kerning, reticências), primitivas com clip, cópia em
 * tiles == fórmula do console (pixel a pixel, inclusive a ordem de bytes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/ui/xui.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    printf("xui                                         ");
    char b[128];
    /* UTF-8 -> Latin-1 */
    int n = xui_utf8_to_latin1(b, sizeof(b), "Ação é Não");
    CHECK(n == 10 && !memcmp(b, "A\xe7\xe3o \xe9 N\xe3o", 10), "acentos pt-BR (n=%d)", n);
    xui_utf8_to_latin1(b, sizeof(b), "Halo\xe2\x84\xa2 \xe2\x80\x9cX\xe2\x80\x9d \xe2\x80\x93 It\xe2\x80\x99s\xe2\x80\xa6");
    CHECK(!strcmp(b, "Halo \"X\" - It's..."), "fold de pontuação (got '%s')", b);
    xui_utf8_to_latin1(b, sizeof(b), "a\xf0\x9f\x8e\xae" "b\xe4\xb8\xad" "c");
    CHECK(!strcmp(b, "abc"), "emoji/CJK somem (got '%s')", b);
    xui_utf8_to_latin1(b, sizeof(b), "Pok\xe9mon");   /* Latin-1 cru também passa */
    CHECK(!strcmp(b, "Pok\xe9mon"), "latin-1 cru");
    xui_utf8_to_latin1(b, 4, "abcdef");
    CHECK(!strcmp(b, "abc"), "cap respeitado");

    /* fonte */
    for (int f = 0; f < XUI_F_COUNT; f++) {
        const xui_face_t *fc = xui_face(f);
        CHECK(fc->map['A'] >= 0 && fc->map['0'] >= 0, "face %d tem A e 0", f);
        if (f != XUI_F_CODE) CHECK(fc->map[0xe3] >= 0 && fc->map[0xe7] >= 0 && fc->map[0xc9] >= 0, "face %d tem ã ç É", f);
    }
    CHECK(xui_text_width(XUI_F_ROW, "Ação", 0) > xui_text_width(XUI_F_ROW, "Aca", 0), "largura com acento");
    CHECK(xui_face(XUI_F_TITLE)->nkern > 0, "kerning gerado (%d pares)", xui_face(XUI_F_TITLE)->nkern);
    int wav = xui_text_width(XUI_F_TITLE, "AV", 0), wa = xui_text_width(XUI_F_TITLE, "A", 0),
        wv = xui_text_width(XUI_F_TITLE, "V", 0);
    CHECK(wav < wa + wv, "kerning AV aplicado (%d vs %d)", wav, wa + wv);

    /* canvas + clip */
    static uint32_t px[200 * 100];
    xui_canvas_t c;
    xui_canvas_init(&c, px, 200, 100, 200);
    xui_fill(&c, 0, 0, 200, 100, 0x111111);
    xui_rect_t cl = { 50, 20, 30, 30 };
    xui_set_clip(&c, cl);
    xui_fill(&c, -10, -10, 500, 500, 0xffffff);
    CHECK(px[20 * 200 + 50] == 0xffffff && px[19 * 200 + 50] == 0x111111 && px[20 * 200 + 80] == 0x111111 &&
          px[49 * 200 + 79] == 0xffffff, "fill respeita clip");
    xui_clip_all(&c);
    int end = xui_text(&c, XUI_F_BODY, 2, 90, 0xffffff, "Um nome muito comprido de jogo", 0, 80);
    CHECK(end <= 2 + 80 + 1, "reticências cabem em max_w (end %d)", end);
    xui_round_rect(&c, 100, 10, 40, 40, 8, 0xff0000, 255);
    CHECK(px[10 * 200 + 100] != 0xff0000 && px[30 * 200 + 120] == 0xff0000, "canto arredondado / miolo");
    CHECK(xui_mix(0, 0xffffff, 255) == 0xffffff && xui_mix(0xffffff, 0, 255) == 0 &&
          xui_mix(0x123456, 0xffffff, 0) == 0x123456, "mix extremos");

    /* tiles: cópia rápida == pset da fórmula, e bytes BGRX */
    int fw = 128, fh = 96, fs = 128;
    static uint32_t src[128 * 96], fb[128 * 96 + 4096], ref[128 * 96 + 4096];
    for (int i = 0; i < fw * fh; i++) src[i] = (uint32_t)(i * 2654435761u) & 0xffffff;
    xui_canvas_t sc;
    xui_canvas_init(&sc, src, fw, fh, fw);
    memset(fb, 0, sizeof(fb));
    memset(ref, 0, sizeof(ref));
    xui_rect_t r = { 5, 3, 70, 41 };
    xui_tile_copy(fb, fs, fw, fh, &sc, r);
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x & ~3; x < ((r.x + r.w + 3) & ~3); x++) {
            uint32_t p = src[y * fw + x];
            unsigned char bgrx[4] = { (unsigned char)p, (unsigned char)(p >> 8), (unsigned char)(p >> 16), 0 };
            memcpy(&ref[xui_tile_offset(x, y, fs)], bgrx, 4);
        }
    CHECK(!memcmp(fb, ref, sizeof(fb)), "tile copy == fórmula do console (e só o retângulo)");

    printf("%s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
