/*
 * orbita — EDID -> resumo de modos (ver xc_edid.h, VIDEO-1080P.md).
 * Referências: VESA E-EDID 1.4 (bloco base, DTD, standard timings) e
 * CEA-861-F (extensão 0x02: Video Data Block, VSDB HDMI, DTDs extras).
 */
#include "xc_edid.h"
#include <stdio.h>
#include <string.h>

static int block_sum_ok(const uint8_t *b) {
    uint8_t s = 0;
    for (int i = 0; i < XC_EDID_BLOCK; i++) s = (uint8_t)(s + b[i]);
    return s == 0;
}

static int near60(unsigned hz_x100) { return hz_x100 >= 5900 && hz_x100 <= 6100; }

/* DTD de 18 bytes. 1 = era um modo; 0 = descritor (pclk 0) */
static int parse_dtd(const uint8_t *d, xc_edid_mode_t *m) {
    unsigned pclk10k = d[0] | (d[1] << 8);
    if (pclk10k == 0) return 0;
    unsigned hact = d[2] | ((d[4] & 0xF0) << 4);
    unsigned hbl  = d[3] | ((d[4] & 0x0F) << 8);
    unsigned vact = d[5] | ((d[7] & 0xF0) << 4);
    unsigned vbl  = d[6] | ((d[7] & 0x0F) << 8);
    memset(m, 0, sizeof(*m));
    m->pclk_khz = pclk10k * 10u;
    m->interlaced = (d[17] & 0x80) ? 1 : 0;
    m->w = (uint16_t)hact;
    m->h = (uint16_t)(m->interlaced ? vact * 2 : vact);
    unsigned long long tot = (unsigned long long)(hact + hbl) * (vact + vbl);
    if (tot) m->hz_x100 = (uint16_t)(((unsigned long long)m->pclk_khz * 100000ull + tot / 2) / tot);
    return 1;
}

static void add_dtd(xc_edid_info_t *e, const xc_edid_mode_t *m) {
    if (e->n_dtd < XC_EDID_MAX_DTD) e->dtd[e->n_dtd++] = *m;
}

static void add_vic(xc_edid_info_t *e, unsigned vic, int native) {
    if (vic == 0) return;
    for (int i = 0; i < e->n_vic; i++)
        if (e->vic[i] == vic) { if (native) e->vic_native[i] = 1; return; }
    if (e->n_vic < XC_EDID_MAX_VIC) {
        e->vic[e->n_vic] = (uint8_t)vic;
        e->vic_native[e->n_vic] = (uint8_t)(native ? 1 : 0);
        e->n_vic++;
    }
}

static void parse_cea(xc_edid_info_t *e, const uint8_t *b) {
    unsigned d = b[2];
    if (d >= 4 && d <= 127) {
        unsigned i = 4;
        while (i < d) {
            unsigned tag = b[i] >> 5, len = b[i] & 0x1F;
            if (i + 1 + len > d) break;   /* bloco passa do fim: para */
            if (tag == 2) {               /* Video Data Block: SVDs */
                for (unsigned k = 1; k <= len; k++) {
                    unsigned s = b[i + k];
                    if (s >= 129 && s <= 192) add_vic(e, s & 0x7F, 1);
                    else if ((s >= 1 && s <= 127) || (s >= 193 && s <= 253)) add_vic(e, s, 0);
                }
            } else if (tag == 3 && len >= 3) {   /* Vendor Specific */
                unsigned oui = b[i + 1] | (b[i + 2] << 8) | (b[i + 3] << 16);
                if (oui == 0x000C03) e->hdmi = 1;
            }
            i += 1 + len;
        }
    }
    if (d >= 4) {
        for (unsigned o = d; o + 18 <= 127; o += 18) {
            xc_edid_mode_t m;
            if (!parse_dtd(b + o, &m)) break;
            add_dtd(e, &m);
        }
    }
}

int xc_edid_parse(const uint8_t *buf, size_t len, xc_edid_info_t *e) {
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memset(e, 0, sizeof(*e));
    if (!buf || len < XC_EDID_BLOCK || memcmp(buf, hdr, 8) != 0 || !block_sum_ok(buf)) return -1;
    if (buf[18] != 1) return -1;
    e->valid = 1;
    e->version = buf[18];
    e->revision = buf[19];
    e->digital = (buf[20] & 0x80) ? 1 : 0;
    unsigned id = (buf[8] << 8) | buf[9];
    e->mfg[0] = (char)('A' - 1 + ((id >> 10) & 0x1F));
    e->mfg[1] = (char)('A' - 1 + ((id >> 5) & 0x1F));
    e->mfg[2] = (char)('A' - 1 + (id & 0x1F));
    e->product = (uint16_t)(buf[10] | (buf[11] << 8));
    /* standard timings */
    for (int i = 0; i < 8; i++) {
        uint8_t b1 = buf[38 + 2 * i], b2 = buf[39 + 2 * i];
        if (b1 <= 0x01) continue;   /* 01 01 = vazio; 00/01 xx = lixo comum (256 px) */
        xc_edid_mode_t m;
        memset(&m, 0, sizeof(m));
        m.w = (uint16_t)((b1 + 31) * 8);
        switch (b2 >> 6) {
        case 0: m.h = (uint16_t)(e->revision >= 3 ? m.w * 10 / 16 : m.w); break;
        case 1: m.h = (uint16_t)(m.w * 3 / 4); break;
        case 2: m.h = (uint16_t)(m.w * 4 / 5); break;
        default: m.h = (uint16_t)(m.w * 9 / 16); break;
        }
        m.hz_x100 = (uint16_t)(((b2 & 0x3F) + 60) * 100);
        if (e->n_std < XC_EDID_MAX_STD) e->std[e->n_std++] = m;
    }
    /* 4 descritores de 18 bytes: DTDs ou texto */
    for (int i = 0; i < 4; i++) {
        const uint8_t *d = buf + 54 + 18 * i;
        xc_edid_mode_t m;
        if (parse_dtd(d, &m)) { add_dtd(e, &m); continue; }
        if (d[3] == 0xFC) {
            int k = 0;
            for (int j = 5; j < 18 && d[j] != 0x0A && k < 13; j++)
                e->name[k++] = (d[j] >= 0x20 && d[j] < 0x7F) ? (char)d[j] : '?';
            while (k > 0 && e->name[k - 1] == ' ') k--;
            e->name[k] = 0;
        }
    }
    /* extensões */
    e->n_ext = buf[126];
    int next = e->n_ext > XC_EDID_MAX_EXT ? XC_EDID_MAX_EXT : e->n_ext;
    for (int x = 1; x <= next; x++) {
        if ((size_t)(x + 1) * XC_EDID_BLOCK > len) break;
        const uint8_t *b = buf + x * XC_EDID_BLOCK;
        if (b[0] != 0x02) continue;   /* só CEA */
        if (!block_sum_ok(b)) { e->cea_bad++; continue; }
        e->n_cea++;
        parse_cea(e, b);
    }
    /* o que a escolha do modo usa */
    for (int i = 0; i < e->n_vic; i++) {
        unsigned v = e->vic[i];
        if (v == 4 || v == 69) e->has_720p60 = 1;
        if (v == 16 || v == 76) e->has_1080p60 = 1;
        if (v == 5) e->has_1080i60 = 1;
    }
    for (int i = 0; i < e->n_dtd + e->n_std; i++) {
        const xc_edid_mode_t *m = i < e->n_dtd ? &e->dtd[i] : &e->std[i - e->n_dtd];
        if (!near60(m->hz_x100)) continue;
        if (m->w == 1280 && m->h == 720 && !m->interlaced) e->has_720p60 = 1;
        if (m->w == 1920 && m->h == 1080 && !m->interlaced) e->has_1080p60 = 1;
        if (m->w == 1920 && m->h == 1080 && m->interlaced) e->has_1080i60 = 1;
    }
    return 0;
}

void xc_edid_summary(const xc_edid_info_t *e, char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (!e || !e->valid) { snprintf(out, n, "EDID invalido/ausente"); return; }
    size_t k = 0;
#define ADD(...) do { if (k < n) { int r_ = snprintf(out + k, n - k, __VA_ARGS__); if (r_ > 0) k += (size_t)r_; } } while (0)
    ADD("%s%s%s%s %s v%d.%d ext=%d cea=%d%s", e->mfg, e->name[0] ? " '" : "", e->name, e->name[0] ? "'" : "",
        e->hdmi ? "hdmi" : (e->digital ? "dvi" : "analog"), e->version, e->revision, e->n_ext, e->n_cea,
        e->cea_bad ? " (cea checksum ruim)" : "");
    if (e->n_dtd)
        ADD(" pref %ux%u%c%u.%02u", e->dtd[0].w, e->dtd[0].h, e->dtd[0].interlaced ? 'i' : 'p',
            e->dtd[0].hz_x100 / 100, e->dtd[0].hz_x100 % 100);
    ADD(" 720p60=%d 1080p60=%d 1080i60=%d", e->has_720p60, e->has_1080p60, e->has_1080i60);
    if (e->n_vic) {
        ADD(" VIC");
        for (int i = 0; i < e->n_vic; i++) ADD("%s%u%s", i ? "," : " ", e->vic[i], e->vic_native[i] ? "*" : "");
    }
#undef ADD
    if (k >= n) out[n - 1] = 0;
}
