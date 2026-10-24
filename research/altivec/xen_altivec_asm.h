/*
 * xen_altivec_asm.h — AltiVec inline-asm wrappers for xenon-gcc 9.2 (devkitxenon)
 *
 * PROBLEM
 *   GCC 9.2 pra target `xenon` (PowerPC BE 32b, -mcpu=cell) NAO expoe alguns
 *   builtins integer AltiVec:
 *     - __builtin_vec_msum, __builtin_vec_msums
 *     - __builtin_vec_mladd
 *     - __builtin_vec_sum4s, vec_sum2s, vec_sums
 *     - __builtin_vec_mule, __builtin_vec_mulo (integer overloads)
 *   Erro: "builtin function '__builtin_vec_XXX' not supported in this
 *          compiler configuration"
 *   Ocorre em qualquer -mcpu (cell/970/7400/power7). Bug conhecido de rs6000.md
 *   pra Cell PPU sem VSX.
 *
 * QUE FUNCIONA vanilla (SEM esta lib)
 *   vec_add, vec_sub, vec_madd (float FMA), vec_perm, vec_ld, vec_st,
 *   vec_sr, vec_sl, vec_and, vec_or, vec_xor, vec_min, vec_max,
 *   vec_splat_*, vec_cmp*, vec_re, vec_rsqrte,
 *   vec_pack, vec_packs, vec_packsu, vec_avg, vec_unpackh, vec_unpackl,
 *   vec_adds, vec_subs, vec_sra, vec_sld, vec_sel, vec_mergeh, vec_mergel
 *
 * QUE PRECISA WRAPPER (aqui)
 *   vmladduhm, vmsumshm, vmsumshs, vmsumuhm, vmsummbm, vmsumubm,
 *   vsum4sbs, vsum4shs, vsum4ubs, vsum2sws, vsumsws,
 *   vmuleub, vmuloub, vmulesh, vmulosh, vmuleuh, vmulouh,
 *   vpkshus, vpkswus (packing sat — nao usados por ffmpeg mas expostos)
 *
 * MACROS de dispatch por tipo
 *   Definidas no final do header — substituem `vec_msum`, `vec_mladd`,
 *   `vec_sum4s`, `vec_sums`, `vec_mule`, `vec_mulo` por wrappers com
 *   dispatch estatico via __builtin_types_compatible_p + __builtin_choose_expr.
 *   Ativar com `#define XEN_ALTIVEC_MACRO_DISPATCH` ANTES de incluir este header.
 *
 * FLAGS OBRIGATORIAS
 *   -mcpu=cell -mtune=cell -maltivec -mabi=altivec -m32 -mpowerpc64 -fno-pic
 *
 * LINK
 *   Sem changes. libgcc.a (32b) ja tem _savevr_20 .. _savevr_31 + _restvr_XX.
 *   GCC gera bl automatico se usa vr20+ nao-volatile.
 *
 * COMPILADO E VERIFICADO em xenon-gcc 9.2.0, binutils 2.32 (devkitxenon local)
 */
#ifndef XEN_ALTIVEC_ASM_H
#define XEN_ALTIVEC_ASM_H

#include <altivec.h>

typedef vector signed char    v16s8;
typedef vector unsigned char  v16u8;
typedef vector signed short   v8s16;
typedef vector unsigned short v8u16;
typedef vector signed int     v4s32;
typedef vector unsigned int   v4u32;
typedef vector float          v4f32;

/* ==========================================================================
 * MLADD — Multiply-add unsigned halfword modulo: A*B + C (low 16b)
 * ==========================================================================*/
static inline v8s16 xen_vmladduhm(v8s16 a, v8s16 b, v8s16 c) {
    v8s16 r;
    __asm__("vmladduhm %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
    return r;
}
static inline v8u16 xen_vmladduhm_u(v8u16 a, v8u16 b, v8u16 c) {
    v8u16 r;
    __asm__("vmladduhm %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
    return r;
}

/* ==========================================================================
 * MSUM — Multiply-sum (mac horizontal, 2 half-per-word ou 4 byte-per-word)
 * ==========================================================================*/
/* signed halfword modulo */
static inline v4s32 xen_vmsumshm(v8s16 a, v8s16 b, v4s32 c) {
    v4s32 r;
    __asm__("vmsumshm %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
    return r;
}
/* signed halfword saturating */
static inline v4s32 xen_vmsumshs(v8s16 a, v8s16 b, v4s32 c) {
    v4s32 r;
    __asm__("vmsumshs %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
    return r;
}
/* unsigned halfword modulo */
static inline v4u32 xen_vmsumuhm(v8u16 a, v8u16 b, v4u32 c) {
    v4u32 r;
    __asm__("vmsumuhm %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
    return r;
}
/* unsigned byte modulo → u32 (SUMSQ pattern do ffmpeg) */
static inline v4u32 xen_vmsumubm(v16u8 a, v16u8 b, v4u32 c) {
    v4u32 r;
    __asm__("vmsumubm %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
    return r;
}
/* mixed signed byte * unsigned byte → s32 (vp8dsp filter) */
static inline v4s32 xen_vmsummbm(v16s8 a, v16u8 b, v4s32 c) {
    v4s32 r;
    __asm__("vmsummbm %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
    return r;
}

/* ==========================================================================
 * MHRADDSHS — Vector Multiply-High Round-Add Saturate Signed Halfword
 * result[i] = sat_s16( ((a[i]*b[i] + 0x4000) >> 15) + c[i] )
 * Usado por idct_altivec (butterflies IDCT).
 * ==========================================================================*/
static inline v8s16 xen_vmhraddshs(v8s16 a, v8s16 b, v8s16 c) {
    v8s16 r;
    __asm__("vmhraddshs %0,%1,%2,%3" : "=v"(r) : "v"(a), "v"(b), "v"(c));
    return r;
}

/* ==========================================================================
 * SUM4/SUMS — sum-across saturating
 * ==========================================================================*/
static inline v4s32 xen_vsum4sbs(v16s8 a, v4s32 c) {
    v4s32 r;
    __asm__("vsum4sbs %0,%1,%2" : "=v"(r) : "v"(a), "v"(c));
    return r;
}
static inline v4s32 xen_vsum4shs(v8s16 a, v4s32 c) {
    v4s32 r;
    __asm__("vsum4shs %0,%1,%2" : "=v"(r) : "v"(a), "v"(c));
    return r;
}
static inline v4u32 xen_vsum4ubs(v16u8 a, v4u32 c) {
    v4u32 r;
    __asm__("vsum4ubs %0,%1,%2" : "=v"(r) : "v"(a), "v"(c));
    return r;
}
/* sum-across signed word saturating (colapsa 4 words num scalar no slot 3) */
static inline v4s32 xen_vsumsws(v4s32 a, v4s32 c) {
    v4s32 r;
    __asm__("vsumsws %0,%1,%2" : "=v"(r) : "v"(a), "v"(c));
    return r;
}
/* sum-across signed doubleword (2 pares → 2 words nos slots 1/3) */
static inline v4s32 xen_vsum2sws(v4s32 a, v4s32 c) {
    v4s32 r;
    __asm__("vsum2sws %0,%1,%2" : "=v"(r) : "v"(a), "v"(c));
    return r;
}

/* ==========================================================================
 * MULE / MULO — multiply even/odd (half-lane widening)
 * ==========================================================================*/
/* unsigned byte  → unsigned half */
static inline v8u16 xen_vmuleub(v16u8 a, v16u8 b) {
    v8u16 r;
    __asm__("vmuleub %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
static inline v8u16 xen_vmuloub(v16u8 a, v16u8 b) {
    v8u16 r;
    __asm__("vmuloub %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
/* signed byte    → signed half */
static inline v8s16 xen_vmulesb(v16s8 a, v16s8 b) {
    v8s16 r;
    __asm__("vmulesb %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
static inline v8s16 xen_vmulosb(v16s8 a, v16s8 b) {
    v8s16 r;
    __asm__("vmulosb %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
/* unsigned half  → unsigned word */
static inline v4u32 xen_vmuleuh(v8u16 a, v8u16 b) {
    v4u32 r;
    __asm__("vmuleuh %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
static inline v4u32 xen_vmulouh(v8u16 a, v8u16 b) {
    v4u32 r;
    __asm__("vmulouh %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
/* signed half    → signed word */
static inline v4s32 xen_vmulesh(v8s16 a, v8s16 b) {
    v4s32 r;
    __asm__("vmulesh %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
static inline v4s32 xen_vmulosh(v8s16 a, v8s16 b) {
    v4s32 r;
    __asm__("vmulosh %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}

/* ==========================================================================
 * PACK — signed→unsigned saturated (nao usados pelo ffmpeg vanilla, expostos)
 * ==========================================================================*/
static inline v16u8 xen_vpkshus(v8s16 a, v8s16 b) {
    v16u8 r;
    __asm__("vpkshus %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
static inline v8u16 xen_vpkswus(v4s32 a, v4s32 b) {
    v8u16 r;
    __asm__("vpkswus %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}

/* ==========================================================================
 * MERGE — interleave (nao usados pelo ffmpeg, expostos)
 * ==========================================================================*/
static inline v8s16 xen_vmrghh(v8s16 a, v8s16 b) {
    v8s16 r;
    __asm__("vmrghh %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
static inline v16u8 xen_vmrghb(v16u8 a, v16u8 b) {
    v16u8 r;
    __asm__("vmrghb %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}
static inline v16u8 xen_vmrglb(v16u8 a, v16u8 b) {
    v16u8 r;
    __asm__("vmrglb %0,%1,%2" : "=v"(r) : "v"(a), "v"(b));
    return r;
}

/* ==========================================================================
 * MACRO DISPATCH — substitui vec_* builtins por chamada correta via
 * __builtin_choose_expr + __builtin_types_compatible_p (compile-time overload)
 * Ativar com `#define XEN_ALTIVEC_MACRO_DISPATCH` ANTES do include.
 * ==========================================================================*/
#ifdef XEN_ALTIVEC_MACRO_DISPATCH

#define __XEN_TC(x,T) __builtin_types_compatible_p(__typeof__(x), T)

/* vec_msum(a,b,c) — despachado pelo tipo de A */
#undef  vec_msum
#define vec_msum(a, b, c) \
    __builtin_choose_expr(__XEN_TC(a, vector unsigned char), \
        xen_vmsumubm((vector unsigned char)(a), (vector unsigned char)(b), (vector unsigned int)(c)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed char), \
        xen_vmsummbm((vector signed char)(a), (vector unsigned char)(b), (vector signed int)(c)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed short), \
        xen_vmsumshm((vector signed short)(a), (vector signed short)(b), (vector signed int)(c)), \
    __builtin_choose_expr(__XEN_TC(a, vector unsigned short), \
        xen_vmsumuhm((vector unsigned short)(a), (vector unsigned short)(b), (vector unsigned int)(c)), \
        (void)0))))

/* vec_mradds(a,b,c) — signed halfword only (vmhraddshs) */
#undef  vec_mradds
#define vec_mradds(a, b, c) \
    xen_vmhraddshs((vector signed short)(a), (vector signed short)(b), (vector signed short)(c))

/* vec_mladd(a,b,c) — s16 ou u16 (a*b+c low 16b) */
#undef  vec_mladd
#define vec_mladd(a, b, c) \
    __builtin_choose_expr(__XEN_TC(a, vector unsigned short), \
        xen_vmladduhm_u((vector unsigned short)(a), (vector unsigned short)(b), (vector unsigned short)(c)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed short), \
        xen_vmladduhm((vector signed short)(a), (vector signed short)(b), (vector signed short)(c)), \
        (void)0))

/* vec_sum4s(a,c) — s8/u8/s16 → s32 */
#undef  vec_sum4s
#define vec_sum4s(a, c) \
    __builtin_choose_expr(__XEN_TC(a, vector unsigned char), \
        xen_vsum4ubs((vector unsigned char)(a), (vector unsigned int)(c)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed char), \
        xen_vsum4sbs((vector signed char)(a), (vector signed int)(c)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed short), \
        xen_vsum4shs((vector signed short)(a), (vector signed int)(c)), \
        (void)0)))

/* vec_sums(a,c) — s32 → s32 (colapsa em word3) */
#undef  vec_sums
#define vec_sums(a, c) \
    xen_vsumsws((vector signed int)(a), (vector signed int)(c))

/* vec_sum2s(a,c) — s32 → s32 (par de sum2) */
#undef  vec_sum2s
#define vec_sum2s(a, c) \
    xen_vsum2sws((vector signed int)(a), (vector signed int)(c))

/* vec_mule / vec_mulo — dispatch por tipo, retorno com dobro de largura */
#undef  vec_mule
#define vec_mule(a, b) \
    __builtin_choose_expr(__XEN_TC(a, vector unsigned char), \
        xen_vmuleub((vector unsigned char)(a), (vector unsigned char)(b)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed char), \
        xen_vmulesb((vector signed char)(a), (vector signed char)(b)), \
    __builtin_choose_expr(__XEN_TC(a, vector unsigned short), \
        xen_vmuleuh((vector unsigned short)(a), (vector unsigned short)(b)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed short), \
        xen_vmulesh((vector signed short)(a), (vector signed short)(b)), \
        (void)0))))

#undef  vec_mulo
#define vec_mulo(a, b) \
    __builtin_choose_expr(__XEN_TC(a, vector unsigned char), \
        xen_vmuloub((vector unsigned char)(a), (vector unsigned char)(b)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed char), \
        xen_vmulosb((vector signed char)(a), (vector signed char)(b)), \
    __builtin_choose_expr(__XEN_TC(a, vector unsigned short), \
        xen_vmulouh((vector unsigned short)(a), (vector unsigned short)(b)), \
    __builtin_choose_expr(__XEN_TC(a, vector signed short), \
        xen_vmulosh((vector signed short)(a), (vector signed short)(b)), \
        (void)0))))

#endif /* XEN_ALTIVEC_MACRO_DISPATCH */

#endif /* XEN_ALTIVEC_ASM_H */
