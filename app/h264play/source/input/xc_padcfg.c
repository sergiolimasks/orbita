/*
 * orbita — zona morta dos analógicos (v0.21.33). Ver xc_padcfg.h.
 */
#include "xc_padcfg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void xpc_default_stick(xpc_cfg_t *c, int stick) {
    c->dz[stick] = stick == XPC_RIGHT ? RSTICK_DZ : STICK_DZ;
    c->outer[stick] = STICK_OUTER;
}

void xpc_defaults(xpc_cfg_t *c) {
    xpc_default_stick(c, XPC_LEFT);
    xpc_default_stick(c, XPC_RIGHT);
}

int xpc_equal(const xpc_cfg_t *a, const xpc_cfg_t *b) {
    for (int s = 0; s < 2; s++)
        if (a->dz[s] != b->dz[s] || a->outer[s] != b->outer[s]) return 0;
    return 1;
}

int xpc_is_default(const xpc_cfg_t *c) {
    xpc_cfg_t d;
    xpc_defaults(&d);
    return xpc_equal(c, &d);
}

int xpc_pct(int32_t raw) {
    if (raw <= 0) return 0;
    return (int)(((int64_t)raw * 100 + XPC_FULL / 2) / XPC_FULL);
}

int32_t xpc_raw(int pct) {
    if (pct <= 0) return 0;
    if (pct >= 100) return XPC_FULL;
    return (int32_t)(((int64_t)pct * XPC_FULL + 50) / 100);
}

void xpc_clamp_stick(xpc_cfg_t *c, int stick, int moved) {
    int32_t *dz = &c->dz[stick], *ou = &c->outer[stick];
    if (*dz < xpc_raw(XPC_DZ_MIN_PCT)) *dz = xpc_raw(XPC_DZ_MIN_PCT);
    if (xpc_pct(*dz) > XPC_DZ_MAX_PCT) *dz = xpc_raw(XPC_DZ_MAX_PCT);
    if (xpc_pct(*ou) < XPC_OUT_MIN_PCT) *ou = xpc_raw(XPC_OUT_MIN_PCT);
    if (*ou > XPC_FULL) *ou = XPC_FULL;
    if (xpc_pct(*ou) < xpc_pct(*dz) + XPC_GAP_PCT) {
        if (moved == 0) {
            *dz = xpc_raw(xpc_pct(*ou) - XPC_GAP_PCT);
        } else {
            *ou = xpc_raw(xpc_pct(*dz) + XPC_GAP_PCT);
            if (*ou > XPC_FULL) *ou = XPC_FULL;
            if (xpc_pct(*ou) < xpc_pct(*dz) + XPC_GAP_PCT) *dz = xpc_raw(xpc_pct(*ou) - XPC_GAP_PCT);
        }
    }
}

int xpc_step(xpc_cfg_t *c, int stick, int what, int dpct) {
    int32_t *v = what ? &c->outer[stick] : &c->dz[stick];
    int32_t old_dz = c->dz[stick], old_ou = c->outer[stick];
    int p = xpc_pct(*v) + dpct;
    int lo = what ? XPC_OUT_MIN_PCT : XPC_DZ_MIN_PCT, hi = what ? XPC_OUT_MAX_PCT : XPC_DZ_MAX_PCT;
    if (p < lo) p = lo;
    if (p > hi) p = hi;
    *v = xpc_raw(p);
    xpc_clamp_stick(c, stick, what);
    /* a distância de 10% empurrou de volta: o passo não andou (não mexe no outro) */
    if (xpc_pct(*v) != p) { c->dz[stick] = old_dz; c->outer[stick] = old_ou; }
    return c->dz[stick] != old_dz || c->outer[stick] != old_ou;
}

/* "L 12000 31000" -> 1 */
static int parse_line(const char *p, const char *e, char tag, int32_t *a, int32_t *b) {
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    if (p >= e || (*p != tag && *p != tag + 32)) return 0;
    p++;
    long v[2];
    for (int i = 0; i < 2; i++) {
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        if (p >= e || *p < '0' || *p > '9') return 0;
        long x = 0;
        while (p < e && *p >= '0' && *p <= '9') {
            x = x * 10 + (*p - '0');
            if (x > 1000000) return 0;
            p++;
        }
        v[i] = x;
    }
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
    if (p != e) return 0;
    if (v[0] > XPC_FULL || v[1] > XPC_FULL) return 0;
    *a = (int32_t)v[0];
    *b = (int32_t)v[1];
    return 1;
}

int xpc_parse(const char *s, size_t n, xpc_cfg_t *c) {
    xpc_defaults(c);
    if (!s || n < 6 || strncmp(s, "XCPAD1", 6) != 0) return -1;
    xpc_cfg_t t;
    xpc_defaults(&t);
    int got = 0;
    const char *p = s, *end = s + n;
    int line = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *e = nl ? nl : end;
        if (line > 0 && e > p) {
            const char *q = p;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
            if (q < e) {
                if (parse_line(p, e, 'L', &t.dz[XPC_LEFT], &t.outer[XPC_LEFT])) got |= 1;
                else if (parse_line(p, e, 'R', &t.dz[XPC_RIGHT], &t.outer[XPC_RIGHT])) got |= 2;
                else return -1;
            }
        } else if (line == 0) {
            const char *q = p + 6;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
            if (q != e) return -1;
        }
        line++;
        p = nl ? nl + 1 : end;
    }
    if (got != 3) return -1;
    xpc_clamp_stick(&t, XPC_LEFT, 0);
    xpc_clamp_stick(&t, XPC_RIGHT, 0);
    *c = t;
    return 0;
}

int xpc_format(const xpc_cfg_t *c, char *out, size_t cap) {
    int n = snprintf(out, cap, "XCPAD1\nL %d %d\nR %d %d\n", (int)c->dz[XPC_LEFT], (int)c->outer[XPC_LEFT],
                     (int)c->dz[XPC_RIGHT], (int)c->outer[XPC_RIGHT]);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/* ---- a conta (cópia exata do main.c v0.15.1/v0.15.3, com dz/outer da config) ---- */
void xpc_deadzone(int16_t *x, int16_t *y, float dz, float outer) {
    float fx = *x, fy = *y;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag <= dz) { *x = 0; *y = 0; return; }
    float m = mag > outer ? outer : mag;
    float k = (m - dz) / (outer - dz) * 32767.0f / mag;
    float ox = fx * k, oy = fy * k;
    if (ox > 32767.0f) ox = 32767.0f; else if (ox < -32767.0f) ox = -32767.0f;
    if (oy > 32767.0f) oy = 32767.0f; else if (oy < -32767.0f) oy = -32767.0f;
    *x = (int16_t)ox;
    *y = (int16_t)oy;
}

void xpc_curve(int16_t *x, int16_t *y) {
    float fx = *x, fy = *y;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag < 1.0f) return;
    float m = mag / 32767.0f;
    if (m > 1.0f) m = 1.0f;
    float out = RSTICK_ADZ + (RSTICK_GAIN - RSTICK_ADZ) * powf(m, RSTICK_EXPO);
    float k = out * 32767.0f / mag;
    *x = (int16_t)(fx * k);
    *y = (int16_t)(fy * k);
}

void xpc_apply(int stick, int32_t dz, int32_t outer, int16_t *x, int16_t *y) {
    if (outer <= dz) outer = dz + 1;   /* nunca divide por zero (config publicada pela metade) */
    xpc_deadzone(x, y, (float)dz, (float)outer);
    if (stick == XPC_RIGHT) xpc_curve(x, y);
}

int xpc_mag_pct(int x, int y) {
    double m = sqrt((double)x * x + (double)y * y) * 100.0 / XPC_FULL;
    return (int)(m + 0.5);
}

int xpc_out_pct(int stick, int32_t dz, int32_t outer, int x, int y) {
    int16_t ox = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    int16_t oy = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    if (oy == -32768) oy = -32767;
    xpc_apply(stick, dz, outer, &ox, &oy);
    int p = xpc_mag_pct(ox, oy);
    return p > 100 ? 100 : p;
}

/* ---- Medir sozinho ---- */
void xpc_meas_start(xpc_meas_t *m, uint32_t now_ms) {
    memset(m, 0, sizeof(*m));
    m->stage = XPC_M_REST;
    m->t0 = now_ms;
}

int xpc_meas_sectors(const xpc_meas_t *m) {
    int k = 0;
    for (int i = 0; i < XPC_M_SECTORS; i++)
        if (m->sect[i] >= 5000) k++;
    return k;
}

int xpc_meas_left_s(const xpc_meas_t *m, uint32_t now_ms) {
    uint32_t len = m->stage == XPC_M_REST ? XPC_M_REST_MS : m->stage == XPC_M_SPIN ? XPC_M_SPIN_MS : 0;
    uint32_t el = now_ms - m->t0;
    if (!len || el >= len) return 0;
    return (int)((len - el + 999) / 1000);
}

int xpc_meas_feed(xpc_meas_t *m, uint32_t now_ms, int x, int y, xpc_cfg_t *c, int stick) {
    if (m->stage != XPC_M_REST && m->stage != XPC_M_SPIN) return m->stage;
    int r = (int)(sqrt((double)x * x + (double)y * y) * 10000.0 / XPC_FULL + 0.5);   /* % x 100 */
    uint32_t el = now_ms - m->t0;
    if (m->stage == XPC_M_REST) {
        if (el < XPC_M_REST_MS) {
            if (r > m->r0) m->r0 = r;
            return m->stage;
        }
        m->stage = XPC_M_SPIN;
        m->t0 = now_ms;
        el = 0;
    }
    if (el < XPC_M_SPIN_MS) {
        double a = atan2((double)y, (double)x);   /* -pi..pi */
        int s = (int)floor((a + M_PI) / (2.0 * M_PI) * XPC_M_SECTORS);
        if (s < 0) s = 0;
        if (s >= XPC_M_SECTORS) s = XPC_M_SECTORS - 1;
        if (r > m->sect[s]) m->sect[s] = r;
        return m->stage;
    }
    /* fim: mínimo = r0 * 1,25 + 2% (5..60%); máximo = menor setor * 0,97 (70..100%) */
    int dz = (m->r0 * 125 / 100 + 200 + 50) / 100;
    if (dz < 5) dz = 5;
    if (dz > XPC_DZ_MAX_PCT) dz = XPC_DZ_MAX_PCT;
    m->dz_pct = dz;
    m->spun = xpc_meas_sectors(m) >= XPC_M_NEED;
    m->out_pct = xpc_pct(c->outer[stick]);
    if (m->spun) {
        int mn = 1 << 30;
        for (int i = 0; i < XPC_M_SECTORS; i++)
            if (m->sect[i] >= 5000 && m->sect[i] < mn) mn = m->sect[i];
        int o = (mn * 97 / 100 + 50) / 100;
        if (o < 70) o = 70;
        if (o > 100) o = 100;
        m->out_pct = o;
        c->outer[stick] = xpc_raw(o);
    }
    c->dz[stick] = xpc_raw(dz);
    xpc_clamp_stick(c, stick, 0);
    m->dz_pct = xpc_pct(c->dz[stick]);
    m->out_pct = xpc_pct(c->outer[stick]);
    m->stage = XPC_M_DONE;
    return m->stage;
}
