/* orbita — ver xc_perf.h (v0.21.43). Só Xbox. */
#include "xc_perf.h"
#include "dec/xc_decdiag.h"

#include <stdio.h>
#include <string.h>

#ifdef XENON
#include <xenon_smc/xenon_smc.h>
#endif

xpf_t xpf = { -1, -1, -1, -1, -1, 0, 0, 0, 0, 0 };
int   xpf_overlay = 0;

void xpf_temps_poll(uint64_t now_us) {
    static uint64_t last = 0;
    if (last && now_us - last < 1000000ULL) return;
    last = now_us;
#ifdef XENON
    uint16_t s[4] = { 0, 0, 0, 0 };
    xenon_smc_query_sensors(s);   /* 8.8 fixo: CPU, GPU, EDRAM, placa */
    int c = (int)s[0] * 10 / 256, g = (int)s[1] * 10 / 256;
    xpf.cpu_c10 = c > 0 && c < 1500 ? c : -1;
    xpf.gpu_c10 = g > 0 && g < 1500 ? g : -1;
#endif
}

void xpf_game_sample(double fps, double dec_ms, unsigned gpu_busy_us, uint64_t dt_us) {
    xdd_load_t l;
    xdd_load_sample(&l);
    xpf.cpu_pct = l.cpu_pct;
    xpf.net_pct = l.net_pct;
    xpf.audio_ms10 = (int)(l.audio_ms_s * 10.0 + 0.5);
    xpf.pad_ms100 = (int)(l.pad_ms_s * 100.0 + 0.5);
    int gp = dt_us ? (int)((uint64_t)gpu_busy_us * 100ULL / dt_us) : -1;
    xpf.gpu_pct = gp > 100 ? 100 : gp;
    xpf.fps10 = (int)(fps * 10.0 + 0.5);
    xpf.dec_ms10 = (int)(dec_ms * 10.0 + 0.5);
    xpf.in_game = 1;
}

void xpf_game_end(void) { xpf.in_game = 0; }

static void tval(char *o, size_t n, int c10) {
    if (c10 < 0) snprintf(o, n, "--");
    else snprintf(o, n, "%d°C", (c10 + 5) / 10);
}

void xpf_overlay_text(char *o, size_t n) {
    char ct[16], gt[16];
    tval(ct, sizeof(ct), xpf.cpu_c10);
    tval(gt, sizeof(gt), xpf.gpu_c10);
    char fps[16], cp[16], gp[16];
    if (xpf.fps10 >= 0) snprintf(fps, sizeof(fps), "%d", (xpf.fps10 + 5) / 10); else snprintf(fps, sizeof(fps), "--");
    if (xpf.cpu_pct >= 0) snprintf(cp, sizeof(cp), "%d%%", xpf.cpu_pct); else snprintf(cp, sizeof(cp), "--");
    if (xpf.gpu_pct >= 0) snprintf(gp, sizeof(gp), "%d%%", xpf.gpu_pct); else snprintf(gp, sizeof(gp), "--");
    snprintf(o, n, "FPS %s\nCPU %s  %s\nGPU %s  %s", fps, cp, ct, gp, gt);
}
