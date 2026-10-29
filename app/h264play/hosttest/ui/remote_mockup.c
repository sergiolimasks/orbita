/* Render the actual native Remote Play screen without an Xbox or network.
 * Usage: remote_mockup output.ppm [empty|busy|error|scroll|home] */
#include "../../source/ui/xui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    uint32_t *px = calloc(1280 * 720, sizeof(*px));
    xui_view_t *v = xui_view_new(1280, 720);
    if (!px || !v) return 1;
    xui_canvas_t c;
    xui_canvas_init(&c, px, 1280, 720, 1280);
    xui_home_data_t d = {0};
    d.row = -1; d.col = XUI_HB_REMOTE; d.remote_tab = 1;
    d.version = "v0.21.35";
    d.settings_avail = 1; d.net_kind = 1;
    xui_remote_t r = {0};
    r.focus = 0; r.n = 2;
    r.consoles[0] = (xui_console_t){"Xbox da sala", "Xbox Series X", "ConnectedStandby"};
    r.consoles[1] = (xui_console_t){"Xbox do quarto", "Xbox One", "On"};
    const char *mode = argc > 2 ? argv[2] : "list";
    if (!strcmp(mode, "empty")) { r.n = 0; r.focus = -1; }
    if (!strcmp(mode, "busy")) { r.n = 0; r.focus = -1; r.busy = 1; }
    if (!strcmp(mode, "error")) { r.n = 0; r.focus = -1; r.err = "Não foi possível buscar os consoles. Verifique a conexão e tente novamente."; }
    if (!strcmp(mode, "scroll")) {
        r.n = 6; r.focus = 5;
        for (int i = 2; i < r.n; i++) r.consoles[i] = r.consoles[i % 2];
    }
    int anim;
    if (!strcmp(mode, "home")) {
        d.version = "v0.21.35"; d.remote_tab = 0;
        d.hint = "Y:Atualizar";
        xui_home_frame(v, &c, &d, NULL, 0, &anim);
    } else xui_remote_frame(v, &c, &r, &d, NULL, 0, &anim);
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 1;
    fprintf(f, "P6\n1280 720\n255\n");
    for (int i = 0; i < 1280 * 720; i++) {
        unsigned char rgb[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    xui_view_free(v); free(px);
    return 0;
}
