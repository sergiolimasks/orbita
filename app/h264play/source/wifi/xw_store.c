/* orbita v0.21.0 — WIFI.TXT. Ver xw_store.h. */
#include "xw_store.h"

#include <stdio.h>
#include <string.h>

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unhex(const char *h, size_t hl, uint8_t *o, size_t cap) {
    if (hl % 2 || hl / 2 > cap) return -1;
    for (size_t i = 0; i < hl / 2; i++) {
        int a = hexv(h[2 * i]), b = hexv(h[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        o[i] = (uint8_t)(a * 16 + b);
    }
    return (int)(hl / 2);
}

int xw_store_parse(const char *t, size_t n, xw_saved_t *o) {
    memset(o, 0, sizeof(*o));
    int have_hdr = 0, have_ssid = 0, have_pmk = 0;
    size_t p = 0;
    while (p < n) {
        size_t e = p;
        while (e < n && t[e] != '\n' && t[e] != '\r') e++;
        const char *l = t + p;
        size_t ll = e - p;
        if (ll == 7 && !memcmp(l, "XCWIFI1", 7)) have_hdr = 1;
        else if (ll > 5 && !memcmp(l, "ssid=", 5)) {
            int k = unhex(l + 5, ll - 5, (uint8_t *)o->ssid, 32);
            if (k >= 1) { o->ssid[k] = 0; o->ssid_len = k; have_ssid = 1; }
        } else if (ll == 8 && !memcmp(l, "pmk=open", 8)) {
            o->secure = 0;
            have_pmk = 1;
        } else if (ll == 4 + 64 && !memcmp(l, "pmk=", 4)) {
            if (unhex(l + 4, 64, o->pmk, 32) == 32) { o->secure = 1; have_pmk = 1; }
        }
        p = e + 1;
    }
    if (!have_hdr || !have_ssid || !have_pmk) {
        memset(o, 0, sizeof(*o));
        return -1;
    }
    return 0;
}

size_t xw_store_format(char *out, size_t cap, const xw_saved_t *s) {
    if (s->ssid_len < 1 || s->ssid_len > 32 || cap < 16 + 64 + 80) return 0;
    size_t n = (size_t)snprintf(out, cap, "XCWIFI1\nssid=");
    for (int i = 0; i < s->ssid_len; i++) n += (size_t)snprintf(out + n, cap - n, "%02x", (uint8_t)s->ssid[i]);
    n += (size_t)snprintf(out + n, cap - n, "\npmk=");
    if (s->secure)
        for (int i = 0; i < 32; i++) n += (size_t)snprintf(out + n, cap - n, "%02x", s->pmk[i]);
    else
        n += (size_t)snprintf(out + n, cap - n, "open");
    n += (size_t)snprintf(out + n, cap - n, "\n");
    return n < cap ? n : 0;
}
