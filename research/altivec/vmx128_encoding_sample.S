/*
 * vmx128_encoding_sample.S
 *
 * Sample VMX128 instructions encoded via .long (bypass GAS -mvmx128).
 * Necessario porque binutils 2.32 instalado (devkitxenon default) NAO tem
 * o patch VMX128 aplicado (patch existe apenas pra 2.28 no toolchain).
 *
 * Encodings derivados de:
 *   github.com/Free60Project/libxenon/blob/master/toolchain/binutils-2.28-vmx128.diff
 *   github.com/hayleyxyz/PPC-Altivec-IDA/blob/master/plugin.cpp
 *
 * Field packing (aplicado abaixo):
 *   VD/VS128 (7-bit): (v & 0x60) >> 3 | (v & 0x1f) << 21
 *   VA128    (7-bit): (v & 0x40) << 4 | (v & 0x20) | (v & 0x1f) << 16
 *   VB128    (7-bit): (v & 0x60) >> 5 | (v & 0x1f) << 11
 *   VC128    (3-bit): (v & 0x7)  << 6
 *
 * PPU AltiVec regs vr0..vr31 sao os mesmos VMX128 regs 0..31 - bits altos zero.
 * VMX128 estende para vr32..vr127 via bits VDh/VBh (nao usaveis por GCC).
 *
 * IMPORTANTE: usar VMX128 em runtime XeLL/libxenon requer:
 *   1. MSR[VEC] setado (libxenon startup_from_xell.S JA faz: oris r5,r5,0x200)
 *   2. Nao ha __savevmx128_XX no libgcc. Se GCC gerar codigo tocando vr32+ voce
 *      quebra a ABI e perde regs em interrupt. Como GCC NAO gera VMX128 nunca,
 *      seguro se voce mesmo controlar quais regs tocar (vr0..vr31 caller-saved,
 *      vr20..vr31 callee-saved via _savevr_XX ja no libgcc.a).
 *
 * PS: pra usar isso via GCC inline asm, os operandos precisam ser expressos
 * como registers e o encoding embutido literalmente. Exemplo abaixo mostra
 * ambos caminhos (asm file separado + inline asm com .long).
 */

	.text
	.section .text.vmx128_sample, "ax", @progbits
	.globl vmx128_sample_kernel
	.type vmx128_sample_kernel, @function

/*
 * void vmx128_sample_kernel(void);
 * Executa uma sequencia de VMX128 puras + retorna.
 * Nao referencia registers via alocacao GCC - operandos hard-coded.
 */
vmx128_sample_kernel:
	/* vaddfp128 vr0, vr1, vr2   -- primary 5, xop 16, VX128 form */
	.long 0x14011010
	/* vmulfp128 vr0, vr1, vr2   -- primary 5, xop 144 */
	.long 0x14011090
	/* vmaddfp128 vr0, vr1, vr2, vr0  -- primary 5, xop 208 (VD=VS=0) */
	.long 0x140110d0
	/* vperm128 vr0, vr1, vr2, vr3  -- primary 5, xop 0, VC128=3 */
	.long 0x140110c0
	/* vsldoi128 vr0, vr1, vr2, 4  -- primary 4, xop 16 (VX128_5), SHB=4 */
	.long 0x10011110
	/* lvx128 vr0, r3, r4  -- primary 4, xop 195 (VX128_1), load vec indexed */
	.long 0x100320c3
	/* stvx128 vr0, r3, r4  -- primary 4, xop 451 */
	.long 0x100321c3
	blr

	.size vmx128_sample_kernel, . - vmx128_sample_kernel

/*
 * Inline-asm variant (drop into C):
 *
 *   static inline void vaddfp128_vr0_vr1_vr2(void) {
 *       __asm__ volatile (".long 0x14011010" ::: "memory");
 *   }
 *
 * Limitacao: operandos sao imutaveis (regs hard-coded no encoding).
 * Nao ha maneira pratica de deixar GCC alocar regs quando ele nao
 * conhece o mnemonic. Solucoes:
 *
 *   A) Escrever hot-loops inteiros em .S file separado (recomendado)
 *   B) Rebuild binutils com patch VMX128 (permite -mvmx128 e mnemonic direto
 *      no asm inline) - viavel se voce refizer binutils-2.28 no toolchain
 *   C) Manter tudo em AltiVec padrao (vr0..vr31) via inline asm ou vec_*
 *      intrinsics — cobre ~85% do que precisa pra decode video, sem VMX128
 *
 * Recomendacao pra XCloud360:
 *   - Opcao (C) para BT.601 convert, dedeblock, DCT etc — cobre performance
 *     de decode/scale/convert. VMX128-exclusive (vperm128 com pool 128 regs,
 *     vpermwi128, vpkd3d128, vrlimi128) so relevante pra reverse-engineering
 *     de bytecode Xbox 360 nativo, nao pra pipeline decode que voce controla.
 */
