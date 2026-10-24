/*
 * bt601_yuv2rgb_altivec.c
 *
 * BT.601 limited-range 8-bit YUV -> 8-bit RGB, AltiVec vetorial,
 * compilando com xenon-gcc 9.2 (devkitxenon, PowerPC BE 32b, Cell PPU).
 *
 * Verificado: compila O3, gera vmladduhm/vpkshus/vsrh/vaddhm/stvx sem escalar.
 *
 * Compile:
 *   xenon-gcc -O3 -maltivec -mabi=altivec -mcpu=cell -mtune=cell \
 *             -mpowerpc64 -m32 -fno-pic -c bt601_yuv2rgb_altivec.c \
 *             -o bt601_yuv2rgb_altivec.o
 *
 * BT.601 limited-range matrix (Q6 fixed point => *64):
 *   R = 1.164*(Y-16)                 + 1.596*(V-128)
 *   G = 1.164*(Y-16) - 0.391*(U-128) - 0.813*(V-128)
 *   B = 1.164*(Y-16) + 2.018*(U-128)
 *
 * Q6 coeffs:
 *   Yc =  74,  Vr = 102, Ug = -25, Vg = -52, Ub = 129
 */

#include <altivec.h>
#include <stdint.h>
#include "xen_altivec_asm.h"

/* Convert 8 pixels YUV (planar 4:4:4, 16-bit input for headroom) -> RGB.
 * Input: 3 aligned 16-byte vectors of 8 int16.
 * Output: 3 aligned vectors, 8 bytes each, R-plane G-plane B-plane
 *         (final RGB interleave = separate step).
 */
void bt601_yuv444_to_rgb_planar_8px(
    const int16_t *y8,   /* aligned 16b */
    const int16_t *u8,   /* aligned 16b, unsigned in but signed math */
    const int16_t *v8,
    uint8_t *rgb_out)    /* aligned 16b, layout: R[0..7] pad G[0..7] pad B[0..7] pad */
{
    v8s16 y = vec_ld(0, y8);
    v8s16 u = vec_ld(0, u8);
    v8s16 v = vec_ld(0, v8);

    const v8s16 c16   = {16,16,16,16,16,16,16,16};
    const v8s16 c128  = {128,128,128,128,128,128,128,128};
    const v8s16 c74   = {74,74,74,74,74,74,74,74};       /* Y * 1.164 Q6 */
    const v8s16 c102  = {102,102,102,102,102,102,102,102}; /* V for R */
    const v8s16 c_25  = {-25,-25,-25,-25,-25,-25,-25,-25}; /* U for G */
    const v8s16 c_52  = {-52,-52,-52,-52,-52,-52,-52,-52}; /* V for G */
    const v8s16 c129  = {129,129,129,129,129,129,129,129}; /* U for B */
    const v8s16 zero  = {0};

    y = vec_sub(y, c16);
    u = vec_sub(u, c128);
    v = vec_sub(v, c128);

    /* base = 74 * Y (Q6) */
    v8s16 base = xen_vmladduhm(y, c74, zero);

    /* R = base + 102*V     */
    v8s16 r = xen_vmladduhm(v, c102, base);
    /* G = base + (-25)*U + (-52)*V */
    v8s16 g = xen_vmladduhm(u, c_25, base);
             g = xen_vmladduhm(v, c_52, g);
    /* B = base + 129*U     */
    v8s16 b = xen_vmladduhm(u, c129, base);

    /* De-Q6: >> 6, then saturate to unsigned char via vpkshus */
    const v8u16 shift6 = {6,6,6,6,6,6,6,6};
    r = vec_sr(r, shift6);
    g = vec_sr(g, shift6);
    b = vec_sr(b, shift6);

    /* Pack 8 int16 -> 8 uint8 (only 8 bytes valid per vector) */
    v16u8 rp = xen_vpkshus(r, r);
    v16u8 gp = xen_vpkshus(g, g);
    v16u8 bp = xen_vpkshus(b, b);

    vec_st((v16s8)rp, 0,  (signed char*)rgb_out);
    vec_st((v16s8)gp, 16, (signed char*)rgb_out);
    vec_st((v16s8)bp, 32, (signed char*)rgb_out);
}

/* Version taking 8-bit input direct, expanding to 16-bit internally.
 * Better throughput per input byte. */
void bt601_yuv444_to_rgb_planar_16px_u8(
    const uint8_t *y16, /* 16 bytes aligned */
    const uint8_t *u16,
    const uint8_t *v16,
    uint8_t *r_out,     /* 16 bytes each, planar output */
    uint8_t *g_out,
    uint8_t *b_out)
{
    v16u8 vy = vec_ld(0, y16);
    v16u8 vu = vec_ld(0, u16);
    v16u8 vv = vec_ld(0, v16);

    /* Unpack unsigned byte -> unsigned short (2 halves each) */
    const v16u8 zero8 = {0};
    v8u16 y_h = (v8u16)xen_vmrghb(zero8, vy);
    v8u16 y_l = (v8u16)xen_vmrglb(zero8, vy);
    v8u16 u_h = (v8u16)xen_vmrghb(zero8, vu);
    v8u16 u_l = (v8u16)xen_vmrglb(zero8, vu);
    v8u16 v_h = (v8u16)xen_vmrghb(zero8, vv);
    v8u16 v_l = (v8u16)xen_vmrglb(zero8, vv);

    const v8s16 c16   = {16,16,16,16,16,16,16,16};
    const v8s16 c128  = {128,128,128,128,128,128,128,128};
    const v8s16 c74   = {74,74,74,74,74,74,74,74};
    const v8s16 c102  = {102,102,102,102,102,102,102,102};
    const v8s16 c_25  = {-25,-25,-25,-25,-25,-25,-25,-25};
    const v8s16 c_52  = {-52,-52,-52,-52,-52,-52,-52,-52};
    const v8s16 c129  = {129,129,129,129,129,129,129,129};
    const v8s16 zero  = {0};
    const v8u16 shift6 = {6,6,6,6,6,6,6,6};

    /* --- high half (pixels 0..7) --- */
    v8s16 yhs = vec_sub((v8s16)y_h, c16);
    v8s16 uhs = vec_sub((v8s16)u_h, c128);
    v8s16 vhs = vec_sub((v8s16)v_h, c128);
    v8s16 base_h = xen_vmladduhm(yhs, c74, zero);
    v8s16 r_h = xen_vmladduhm(vhs, c102, base_h);
    v8s16 g_h = xen_vmladduhm(uhs, c_25, base_h);
    g_h      = xen_vmladduhm(vhs, c_52, g_h);
    v8s16 b_h = xen_vmladduhm(uhs, c129, base_h);
    r_h = vec_sr(r_h, shift6);
    g_h = vec_sr(g_h, shift6);
    b_h = vec_sr(b_h, shift6);

    /* --- low half (pixels 8..15) --- */
    v8s16 yls = vec_sub((v8s16)y_l, c16);
    v8s16 uls = vec_sub((v8s16)u_l, c128);
    v8s16 vls = vec_sub((v8s16)v_l, c128);
    v8s16 base_l = xen_vmladduhm(yls, c74, zero);
    v8s16 r_l = xen_vmladduhm(vls, c102, base_l);
    v8s16 g_l = xen_vmladduhm(uls, c_25, base_l);
    g_l      = xen_vmladduhm(vls, c_52, g_l);
    v8s16 b_l = xen_vmladduhm(uls, c129, base_l);
    r_l = vec_sr(r_l, shift6);
    g_l = vec_sr(g_l, shift6);
    b_l = vec_sr(b_l, shift6);

    /* Pack 2 int16 vectors (8+8) -> 1 uint8 vector (16 bytes sat) */
    v16u8 rp = xen_vpkshus(r_h, r_l);
    v16u8 gp = xen_vpkshus(g_h, g_l);
    v16u8 bp = xen_vpkshus(b_h, b_l);

    vec_st((v16s8)rp, 0, (signed char*)r_out);
    vec_st((v16s8)gp, 0, (signed char*)g_out);
    vec_st((v16s8)bp, 0, (signed char*)b_out);
}
