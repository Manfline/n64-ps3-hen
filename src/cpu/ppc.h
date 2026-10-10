/*
 * Salsa64 — codificación de instrucciones PowerPC64 para el recompilador.
 *
 * Solo lo que usa rec_emit.c y solo instrucciones que tiene el PPE del Cell
 * (Power ISA 2.02): nada de isel, popcntd, ldbrx ni cmpb. Cada función
 * devuelve la palabra de la instrucción (big-endian al escribirla en la
 * PS3). Escrito desde el manual de la arquitectura (Power ISA, libro I).
 *
 * Convenciones del ensamblador: los operandos van en el orden de la
 * mnemónica (`ppc_addi(rt, ra, si)` es `addi rt, ra, si`).
 */
#ifndef SALSA64_CPU_PPC_H
#define SALSA64_CPU_PPC_H

#include <stdint.h>

#define PPC_INLINE static inline __attribute__((unused))

/* Campos de condición: bit dentro de un campo de CR. */
enum { PPC_LT = 0, PPC_GT = 1, PPC_EQ = 2 };
#define PPC_BI(cr, bit) ((cr) * 4 + (bit))

/* BO con predicción estática (bits "at" de Power ISA 2.02). */
enum {
	PPC_BO_TRUE = 12,          /* saltar si el bit vale 1 */
	PPC_BO_FALSE = 4,          /* saltar si vale 0 */
	PPC_BO_TRUE_UNLIKELY = 14,
	PPC_BO_FALSE_UNLIKELY = 6,
	PPC_BO_TRUE_LIKELY = 15,
	PPC_BO_FALSE_LIKELY = 7,
	PPC_BO_ALWAYS = 20,
};

#define PPC_NOP   0x60000000u
#define PPC_BLR   0x4E800020u
#define PPC_BCTR  0x4E800420u
#define PPC_BCTRL 0x4E800421u
#define PPC_SYNC  0x7C0004ACu
#define PPC_ISYNC 0x4C00012Cu

PPC_INLINE uint32_t ppc_d(unsigned op, unsigned rt, unsigned ra, uint32_t imm)
{
	return (uint32_t)op << 26 | (uint32_t)rt << 21 | (uint32_t)ra << 16 | (imm & 0xFFFF);
}

PPC_INLINE uint32_t ppc_x(unsigned rt, unsigned ra, unsigned rb, unsigned xo)
{
	return 31u << 26 | (uint32_t)rt << 21 | (uint32_t)ra << 16 | (uint32_t)rb << 11 | (uint32_t)xo << 1;
}

/* ---- Inmediatos ---------------------------------------------------------- */

PPC_INLINE uint32_t ppc_addi(unsigned rt, unsigned ra, int32_t si) { return ppc_d(14, rt, ra, (uint32_t)si); }
PPC_INLINE uint32_t ppc_addis(unsigned rt, unsigned ra, int32_t si) { return ppc_d(15, rt, ra, (uint32_t)si); }
PPC_INLINE uint32_t ppc_li(unsigned rt, int32_t si) { return ppc_addi(rt, 0, si); }
PPC_INLINE uint32_t ppc_lis(unsigned rt, int32_t si) { return ppc_addis(rt, 0, si); }
PPC_INLINE uint32_t ppc_ori(unsigned ra, unsigned rs, uint32_t ui) { return ppc_d(24, rs, ra, ui); }
PPC_INLINE uint32_t ppc_oris(unsigned ra, unsigned rs, uint32_t ui) { return ppc_d(25, rs, ra, ui); }
PPC_INLINE uint32_t ppc_xori(unsigned ra, unsigned rs, uint32_t ui) { return ppc_d(26, rs, ra, ui); }
PPC_INLINE uint32_t ppc_andi_(unsigned ra, unsigned rs, uint32_t ui) { return ppc_d(28, rs, ra, ui); }
PPC_INLINE uint32_t ppc_andis_(unsigned ra, unsigned rs, uint32_t ui) { return ppc_d(29, rs, ra, ui); }
PPC_INLINE uint32_t ppc_mulli(unsigned rt, unsigned ra, int32_t si) { return ppc_d(7, rt, ra, (uint32_t)si); }

/* cmpi/cmpli: campo BF, L = 1 para 64 bits. */
PPC_INLINE uint32_t ppc_cmpdi(unsigned bf, unsigned ra, int32_t si) { return ppc_d(11, bf << 2 | 1, ra, (uint32_t)si); }
PPC_INLINE uint32_t ppc_cmpwi(unsigned bf, unsigned ra, int32_t si) { return ppc_d(11, bf << 2, ra, (uint32_t)si); }
PPC_INLINE uint32_t ppc_cmpldi(unsigned bf, unsigned ra, uint32_t ui) { return ppc_d(10, bf << 2 | 1, ra, ui); }
PPC_INLINE uint32_t ppc_cmplwi(unsigned bf, unsigned ra, uint32_t ui) { return ppc_d(10, bf << 2, ra, ui); }

/* ---- Cargas y almacenamientos ------------------------------------------ */

PPC_INLINE uint32_t ppc_lwz(unsigned rt, int32_t d, unsigned ra) { return ppc_d(32, rt, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_lbz(unsigned rt, int32_t d, unsigned ra) { return ppc_d(34, rt, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_lhz(unsigned rt, int32_t d, unsigned ra) { return ppc_d(40, rt, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_stw(unsigned rs, int32_t d, unsigned ra) { return ppc_d(36, rs, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_stb(unsigned rs, int32_t d, unsigned ra) { return ppc_d(38, rs, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_sth(unsigned rs, int32_t d, unsigned ra) { return ppc_d(44, rs, ra, (uint32_t)d); }
/* DS: el desplazamiento tiene que ser múltiplo de 4. */
PPC_INLINE uint32_t ppc_ld(unsigned rt, int32_t ds, unsigned ra) { return ppc_d(58, rt, ra, (uint32_t)ds & 0xFFFC); }
PPC_INLINE uint32_t ppc_std(unsigned rs, int32_t ds, unsigned ra) { return ppc_d(62, rs, ra, (uint32_t)ds & 0xFFFC); }
PPC_INLINE uint32_t ppc_stdu(unsigned rs, int32_t ds, unsigned ra) { return ppc_d(62, rs, ra, ((uint32_t)ds & 0xFFFC) | 1); }

PPC_INLINE uint32_t ppc_lwzx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 23); }
PPC_INLINE uint32_t ppc_lbzx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 87); }
PPC_INLINE uint32_t ppc_lhzx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 279); }
PPC_INLINE uint32_t ppc_ldx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 21); }
PPC_INLINE uint32_t ppc_stwx(unsigned rs, unsigned ra, unsigned rb) { return ppc_x(rs, ra, rb, 151); }
PPC_INLINE uint32_t ppc_stbx(unsigned rs, unsigned ra, unsigned rb) { return ppc_x(rs, ra, rb, 215); }
PPC_INLINE uint32_t ppc_sthx(unsigned rs, unsigned ra, unsigned rb) { return ppc_x(rs, ra, rb, 407); }
PPC_INLINE uint32_t ppc_stdx(unsigned rs, unsigned ra, unsigned rb) { return ppc_x(rs, ra, rb, 149); }

/* ---- Aritmética y lógica (forma X/XO, sin OE ni Rc) ------------------------- */

PPC_INLINE uint32_t ppc_add(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 266); }
/* subf rt, ra, rb: rt = rb - ra */
PPC_INLINE uint32_t ppc_subf(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 40); }
PPC_INLINE uint32_t ppc_neg(unsigned rt, unsigned ra) { return ppc_x(rt, ra, 0, 104); }
PPC_INLINE uint32_t ppc_mulld(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 233); }
PPC_INLINE uint32_t ppc_mulhd(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 73); }
PPC_INLINE uint32_t ppc_mulhdu(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 9); }
PPC_INLINE uint32_t ppc_divd(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 489); }
PPC_INLINE uint32_t ppc_divdu(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(rt, ra, rb, 457); }

/* Lógicas: ra = rs op rb */
PPC_INLINE uint32_t ppc_and(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 28); }
PPC_INLINE uint32_t ppc_andc(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 60); }
PPC_INLINE uint32_t ppc_or(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 444); }
PPC_INLINE uint32_t ppc_xor(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 316); }
PPC_INLINE uint32_t ppc_nor(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 124); }
PPC_INLINE uint32_t ppc_eqv(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 284); }
PPC_INLINE uint32_t ppc_mr(unsigned ra, unsigned rs) { return ppc_or(ra, rs, rs); }
PPC_INLINE uint32_t ppc_extsw(unsigned ra, unsigned rs) { return ppc_x(rs, ra, 0, 986); }
PPC_INLINE uint32_t ppc_extsh(unsigned ra, unsigned rs) { return ppc_x(rs, ra, 0, 922); }
PPC_INLINE uint32_t ppc_extsb(unsigned ra, unsigned rs) { return ppc_x(rs, ra, 0, 954); }

/* Desplazamientos con registro: microcodificados en el PPE, solo para
 * SLLV/SRLV/SRAV y sus versiones de 64 bits. */
PPC_INLINE uint32_t ppc_slw(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 24); }
PPC_INLINE uint32_t ppc_srw(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 536); }
PPC_INLINE uint32_t ppc_sld(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 27); }
PPC_INLINE uint32_t ppc_srd(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 539); }
PPC_INLINE uint32_t ppc_srad(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(rs, ra, rb, 794); }

/* ---- Comparaciones (forma X) ------------------------------------------------ */

PPC_INLINE uint32_t ppc_cmpd(unsigned bf, unsigned ra, unsigned rb) { return ppc_x(bf << 2 | 1, ra, rb, 0); }
PPC_INLINE uint32_t ppc_cmpld(unsigned bf, unsigned ra, unsigned rb) { return ppc_x(bf << 2 | 1, ra, rb, 32); }

/* ---- Rotaciones con inmediato ----------------------------------------------- */

PPC_INLINE uint32_t ppc_rlwinm(unsigned ra, unsigned rs, unsigned sh, unsigned mb, unsigned me)
{
	return 21u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (sh & 31) << 11 | (mb & 31) << 6 | (me & 31) << 1;
}

/* Forma MD: sh y mb/me de 6 bits, partidos como pide la codificación. */
PPC_INLINE uint32_t ppc_md(unsigned rs, unsigned ra, unsigned sh, unsigned m, unsigned xo)
{
	return 30u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (sh & 31) << 11 |
	       ((m & 31) << 1 | (m >> 5 & 1)) << 5 | xo << 2 | (sh >> 5 & 1) << 1;
}

PPC_INLINE uint32_t ppc_rldicl(unsigned ra, unsigned rs, unsigned sh, unsigned mb) { return ppc_md(rs, ra, sh, mb, 0); }
PPC_INLINE uint32_t ppc_rldicr(unsigned ra, unsigned rs, unsigned sh, unsigned me) { return ppc_md(rs, ra, sh, me, 1); }
PPC_INLINE uint32_t ppc_sldi(unsigned ra, unsigned rs, unsigned n) { return ppc_rldicr(ra, rs, n, 63 - n); }
PPC_INLINE uint32_t ppc_srdi(unsigned ra, unsigned rs, unsigned n) { return ppc_rldicl(ra, rs, (64 - n) & 63, n); }
PPC_INLINE uint32_t ppc_clrldi(unsigned ra, unsigned rs, unsigned n) { return ppc_rldicl(ra, rs, 0, n); }

PPC_INLINE uint32_t ppc_sradi(unsigned ra, unsigned rs, unsigned sh)
{
	return 31u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (sh & 31) << 11 | 413u << 2 | (sh >> 5 & 1) << 1;
}

/* ---- Saltos -------------------------------------------------------------- */

/* Desplazamiento en bytes desde la instrucción (±32 MB). */
PPC_INLINE uint32_t ppc_b(int32_t disp) { return 18u << 26 | ((uint32_t)disp & 0x03FFFFFC); }
PPC_INLINE uint32_t ppc_bl(int32_t disp) { return ppc_b(disp) | 1; }
/* Condicional, ±32 KB. */
PPC_INLINE uint32_t ppc_bc(unsigned bo, unsigned bi, int32_t disp)
{
	return 16u << 26 | (uint32_t)bo << 21 | (uint32_t)bi << 16 | ((uint32_t)disp & 0xFFFC);
}

/* ---- Registros especiales ------------------------------------------------- */

PPC_INLINE uint32_t ppc_mflr(unsigned rt) { return 0x7C0802A6u | (uint32_t)rt << 21; }
PPC_INLINE uint32_t ppc_mtlr(unsigned rs) { return 0x7C0803A6u | (uint32_t)rs << 21; }
PPC_INLINE uint32_t ppc_mtctr(unsigned rs) { return 0x7C0903A6u | (uint32_t)rs << 21; }
PPC_INLINE uint32_t ppc_mfcr(unsigned rt) { return 0x7C000026u | (uint32_t)rt << 21; }
PPC_INLINE uint32_t ppc_mtcrf(unsigned fxm, unsigned rs)
{
	return 31u << 26 | (uint32_t)rs << 21 | (fxm & 0xFF) << 12 | 144u << 1;
}

/* Desplazamiento de un salto que cabe en la forma I o B. */
PPC_INLINE int ppc_fits_b(int64_t disp) { return disp >= -(1 << 25) && disp < (1 << 25) && !(disp & 3); }
PPC_INLINE int ppc_fits_bc(int64_t disp) { return disp >= -(1 << 15) && disp < (1 << 15) && !(disp & 3); }

#endif
