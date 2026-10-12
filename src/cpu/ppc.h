/* SPDX-License-Identifier: GPL-2.0-or-later */
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
 *
 * Ojo: en addi, addis y en la base de las cargas y almacenamientos con
 * desplazamiento, ra = 0 no es r0 sino el valor 0 (así se escribe li). r0
 * sirve de destino, pero nunca de fuente en esas instrucciones.
 */
#ifndef SALSA64_CPU_PPC_H
#define SALSA64_CPU_PPC_H

#include <stdint.h>

#define PPC_INLINE static inline __attribute__((unused))

/* Campos de condición: bit dentro de un campo de CR (SO es "unordered" en
 * las comparaciones de punto flotante). */
enum { PPC_LT = 0, PPC_GT = 1, PPC_EQ = 2, PPC_SO = 3 };
#define PPC_BI(cr, bit) ((cr) * 4 + (bit))

/*
 * Lo que en el PPE es microcódigo (manual del Cell, tabla A-3: ≥ 11 ciclos
 * y vacía el pipeline) y aquí no se emite: toda forma con punto (andi.,
 * andis., rlwinm., add., …), los desplazamientos y rotaciones por registro
 * (slw, srw, sld, srd, sraw, srad, rldcl, rlwnm), lwa/lha, mtcrf de más de
 * un campo (mtocrf de uno es de un ciclo), mcrfs, mtfsf*, lmw/stmw. mfcr
 * cuesta ~34 ciclos y mffs 11–28. Los desplazamientos por registro quedan
 * donde no hay otra forma (SLLV y compañía, LWL/LWR sueltos).
 */

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
PPC_INLINE uint32_t ppc_xoris(unsigned ra, unsigned rs, uint32_t ui) { return ppc_d(27, rs, ra, ui); }
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
PPC_INLINE uint32_t ppc_cmpw(unsigned bf, unsigned ra, unsigned rb) { return ppc_x(bf << 2, ra, rb, 0); }
PPC_INLINE uint32_t ppc_cmplw(unsigned bf, unsigned ra, unsigned rb) { return ppc_x(bf << 2, ra, rb, 32); }

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

/* ---- Punto flotante (doble precisión, f0–f13 volátiles) ------------------- */

PPC_INLINE uint32_t ppc_lfs(unsigned frt, int32_t d, unsigned ra) { return ppc_d(48, frt, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_lfd(unsigned frt, int32_t d, unsigned ra) { return ppc_d(50, frt, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_stfs(unsigned frs, int32_t d, unsigned ra) { return ppc_d(52, frs, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_stfd(unsigned frs, int32_t d, unsigned ra) { return ppc_d(54, frs, ra, (uint32_t)d); }
PPC_INLINE uint32_t ppc_lfsx(unsigned frt, unsigned ra, unsigned rb) { return ppc_x(frt, ra, rb, 535); }
PPC_INLINE uint32_t ppc_lfdx(unsigned frt, unsigned ra, unsigned rb) { return ppc_x(frt, ra, rb, 599); }
PPC_INLINE uint32_t ppc_stfsx(unsigned frs, unsigned ra, unsigned rb) { return ppc_x(frs, ra, rb, 663); }
PPC_INLINE uint32_t ppc_stfdx(unsigned frs, unsigned ra, unsigned rb) { return ppc_x(frs, ra, rb, 727); }
/* Copia y valor absoluto: operan sobre los bits (no levantan causas). */
PPC_INLINE uint32_t ppc_fmr(unsigned frt, unsigned frb) { return 63u << 26 | (uint32_t)frt << 21 | (uint32_t)frb << 11 | 72u << 1; }
PPC_INLINE uint32_t ppc_fabs(unsigned frt, unsigned frb) { return 63u << 26 | (uint32_t)frt << 21 | (uint32_t)frb << 11 | 264u << 1; }

/* ---- Lógica sobre bits del CR (un ciclo en la unidad de saltos) ------------ */

PPC_INLINE uint32_t ppc_crop(unsigned bt, unsigned ba, unsigned bb, unsigned xo)
{
	return 19u << 26 | (uint32_t)bt << 21 | (uint32_t)ba << 16 | (uint32_t)bb << 11 | xo << 1;
}
PPC_INLINE uint32_t ppc_crand(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(bt, ba, bb, 257); }
PPC_INLINE uint32_t ppc_cror(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(bt, ba, bb, 449); }
PPC_INLINE uint32_t ppc_crxor(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(bt, ba, bb, 193); }
PPC_INLINE uint32_t ppc_crnor(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(bt, ba, bb, 33); }
PPC_INLINE uint32_t ppc_creqv(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(bt, ba, bb, 289); }
/* crandc: ba & ~bb; crorc: ba | ~bb */
PPC_INLINE uint32_t ppc_crandc(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(bt, ba, bb, 129); }
PPC_INLINE uint32_t ppc_crorc(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(bt, ba, bb, 417); }
PPC_INLINE uint32_t ppc_crmove(unsigned bt, unsigned ba) { return ppc_cror(bt, ba, ba); }
PPC_INLINE uint32_t ppc_crnot(unsigned bt, unsigned ba) { return ppc_crnor(bt, ba, ba); }
PPC_INLINE uint32_t ppc_crclr(unsigned bt) { return ppc_crxor(bt, bt, bt); }
PPC_INLINE uint32_t ppc_crset(unsigned bt) { return ppc_creqv(bt, bt, bt); }

/* Forma A (opcode 63): frt, fra, frb, frc. */
PPC_INLINE uint32_t ppc_fa(unsigned frt, unsigned fra, unsigned frb, unsigned frc, unsigned xo)
{
	return 63u << 26 | (uint32_t)frt << 21 | (uint32_t)fra << 16 | (uint32_t)frb << 11 | (uint32_t)frc << 6 | xo << 1;
}

PPC_INLINE uint32_t ppc_fadd(unsigned frt, unsigned fra, unsigned frb) { return ppc_fa(frt, fra, frb, 0, 21); }
PPC_INLINE uint32_t ppc_fsub(unsigned frt, unsigned fra, unsigned frb) { return ppc_fa(frt, fra, frb, 0, 20); }
PPC_INLINE uint32_t ppc_fmul(unsigned frt, unsigned fra, unsigned frc) { return ppc_fa(frt, fra, 0, frc, 25); }
PPC_INLINE uint32_t ppc_fdiv(unsigned frt, unsigned fra, unsigned frb) { return ppc_fa(frt, fra, frb, 0, 18); }
/* fmsub: fra * frc - frb; fnmsub: frb - fra * frc (un solo redondeo). */
PPC_INLINE uint32_t ppc_fmsub(unsigned frt, unsigned fra, unsigned frc, unsigned frb) { return ppc_fa(frt, fra, frb, frc, 28); }
PPC_INLINE uint32_t ppc_fnmsub(unsigned frt, unsigned fra, unsigned frc, unsigned frb) { return ppc_fa(frt, fra, frb, frc, 30); }
/* Forma X (opcode 63). */
PPC_INLINE uint32_t ppc_frsp(unsigned frt, unsigned frb) { return 63u << 26 | (uint32_t)frt << 21 | (uint32_t)frb << 11 | 12u << 1; }
PPC_INLINE uint32_t ppc_fneg(unsigned frt, unsigned frb) { return 63u << 26 | (uint32_t)frt << 21 | (uint32_t)frb << 11 | 40u << 1; }
PPC_INLINE uint32_t ppc_fcmpu(unsigned bf, unsigned fra, unsigned frb)
{
	return 63u << 26 | (uint32_t)bf << 23 | (uint32_t)fra << 16 | (uint32_t)frb << 11;
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

/* bclr: vuelve por LR si la condición (como en bc) se cumple. */
PPC_INLINE uint32_t ppc_bclr(unsigned bo, unsigned bi) { return 0x4C000020u | (uint32_t)bo << 21 | (uint32_t)bi << 16; }
PPC_INLINE uint32_t ppc_mflr(unsigned rt) { return 0x7C0802A6u | (uint32_t)rt << 21; }
PPC_INLINE uint32_t ppc_mtlr(unsigned rs) { return 0x7C0803A6u | (uint32_t)rs << 21; }
PPC_INLINE uint32_t ppc_mtctr(unsigned rs) { return 0x7C0903A6u | (uint32_t)rs << 21; }
PPC_INLINE uint32_t ppc_mfcr(unsigned rt) { return 0x7C000026u | (uint32_t)rt << 21; }
PPC_INLINE uint32_t ppc_mtcrf(unsigned fxm, unsigned rs)
{
	return 31u << 26 | (uint32_t)rs << 21 | (fxm & 0xFF) << 12 | 144u << 1;
}
/* mtocrf: un solo campo (fxm con un bit); mtcrf de varios es microcódigo. */
PPC_INLINE uint32_t ppc_mtocrf(unsigned fxm, unsigned rs) { return ppc_mtcrf(fxm, rs) | 1u << 20; }

/* Desplazamiento de un salto que cabe en la forma I o B. */
PPC_INLINE int ppc_fits_b(int64_t disp) { return disp >= -(1 << 25) && disp < (1 << 25) && !(disp & 3); }
PPC_INLINE int ppc_fits_bc(int64_t disp) { return disp >= -(1 << 15) && disp < (1 << 15) && !(disp & 3); }

/* ---- Para conversiones de la FPU y accesos no alineados (rec_emit_ext.h) --- */

/* subfic rt, ra, si: rt = si - ra */
PPC_INLINE uint32_t ppc_subfic(unsigned rt, unsigned ra, int32_t si) { return ppc_d(8, rt, ra, (uint32_t)si); }
/* Ceros a la izquierda en los 32 bits bajos (32 si son todos 0). */
PPC_INLINE uint32_t ppc_cntlzw(unsigned ra, unsigned rs) { return ppc_x(rs, ra, 0, 26); }
/* rlwimi: rota rs y lo inserta en ra bajo la máscara mb–me (el resto de ra queda). */
PPC_INLINE uint32_t ppc_rlwimi(unsigned ra, unsigned rs, unsigned sh, unsigned mb, unsigned me)
{
	return 20u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (sh & 31) << 11 | (mb & 31) << 6 | (me & 31) << 1;
}
/* Raíz cuadrada (opcional en la arquitectura; el PPE la tiene: ppu-gcc
 * -mcpu=cell la emite). */
PPC_INLINE uint32_t ppc_fsqrt(unsigned frt, unsigned frb) { return ppc_fa(frt, 0, frb, 0, 22); }
/* A entero de 64 bits en el registro de punto flotante: con el modo del
 * FPSCR (siempre al más cercano par) o hacia cero; y de vuelta a double. */
PPC_INLINE uint32_t ppc_fctid(unsigned frt, unsigned frb) { return 63u << 26 | (uint32_t)frt << 21 | (uint32_t)frb << 11 | 814u << 1; }
PPC_INLINE uint32_t ppc_fctidz(unsigned frt, unsigned frb) { return 63u << 26 | (uint32_t)frt << 21 | (uint32_t)frb << 11 | 815u << 1; }
PPC_INLINE uint32_t ppc_fcfid(unsigned frt, unsigned frb) { return 63u << 26 | (uint32_t)frt << 21 | (uint32_t)frb << 11 | 846u << 1; }

#endif
