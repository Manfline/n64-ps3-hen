/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — piezas de la FPU (COP1) que comparten el camino completo
 * (cop1.c) y el camino rápido, que se integra en el bucle del intérprete
 * (interp.c) para no pagar una llamada por instrucción. Las reglas del
 * hardware y el método de redondeo están en cop1.c.
 */
#ifndef SALSA64_CPU_FPU_H
#define SALSA64_CPU_FPU_H

#include "cpu/vr4300_internal.h"

enum { FPE_I = 0x01, FPE_U = 0x02, FPE_O = 0x04, FPE_Z = 0x08, FPE_V = 0x10, FPE_E = 0x20 };

enum { FMT_S = 0x10, FMT_D = 0x11, FMT_W = 0x14, FMT_L = 0x15 };

enum { RM_NEAREST, RM_ZERO, RM_PLUS, RM_MINUS };

/* Se integra siempre: en el bucle del intérprete, una llamada obliga a
 * guardar registros de punto flotante en la pila y recargarlos. */
#define FPU_INLINE static inline __attribute__((always_inline))

FPU_INLINE int sign_of(double x) { return (x > 0) - (x < 0); }

/* ---- Signo del error de redondeo ---------------------------------------------- */

/* Signo del error de r = a + b redondeado (TwoSum de Knuth; exacto sin
 * desbordamiento). */
FPU_INLINE int add_error(double a, double b, double r)
{
	double bb = r - a;
	double e = (a - (r - bb)) + (b - bb);
	return sign_of(e);
}

/* Cerca del subdesbordamiento el fma puede perder el residuo; se escala por
 * 2^128 el operando chico (exacto, sin desbordar) antes de calcularlo. */
#define TINY_RESULT 0x1p-900
#define SCALE       0x1p128

FPU_INLINE int mul_error(double a, double b, double r)
{
	if (__builtin_fabs(r) >= TINY_RESULT)
		return sign_of(__builtin_fma(a, b, -r));
	if (r == 0)  /* producto de dos no ceros redondeado a cero */
		return (a == 0 || b == 0) ? 0 : (__builtin_signbit(a) != __builtin_signbit(b) ? -1 : 1);
	if (__builtin_fabs(a) < __builtin_fabs(b))
		a *= SCALE;
	else
		b *= SCALE;
	return sign_of(__builtin_fma(a, b, -r * SCALE));
}

FPU_INLINE int div_error(double a, double b, double r)
{
	if (__builtin_fabs(r) >= TINY_RESULT)
		return sign_of(__builtin_fma(-r, b, a)) * sign_of(b);
	if (r == 0)
		return a == 0 ? 0 : (__builtin_signbit(a) != __builtin_signbit(b) ? -1 : 1);
	return sign_of(__builtin_fma(-r * SCALE, b, a * SCALE)) * sign_of(b);
}

/* ---- Camino rápido ------------------------------------------------------------ */

/*
 * Lo más frecuente de COP1 sin salir del bucle del intérprete: MFC1, DMFC1,
 * CFC1, MTC1, DMTC1 y, en S y D, ADD/SUB/MUL/DIV, ABS, NEG, MOV y C.cond.
 * Da lo mismo que el camino completo (la prueba fast_path_matches_full los
 * compara); lo que no cubre devuelve false y va por salsa64_vr4300_cop1.
 *
 * Pensado para el PPE, que no tiene movimientos directos entre registros
 * enteros y de punto flotante (van por memoria: load-hit-store, decenas de
 * ciclos) y donde una comparación de punto flotante seguida de un salto
 * espera toda la latencia de la FPU:
 *   - Los bits de cada operando se cargan de cpu->fpr a un registro entero y,
 *     aparte, el valor a uno de punto flotante (lfs/lfd); el resultado se
 *     guarda desde el registro de punto flotante.
 *   - La clasificación (cero, normal, NaN, desnormal) y C.cond se hacen con
 *     los bits, en enteros.
 *   - La aritmética solo toma entradas cero o con exponente en una franja
 *     alrededor de 1 (fpu_fast_band): así ningún resultado desborda ni queda
 *     diminuto y no hay que revisarlo. Queda una sola comparación de punto
 *     flotante, la del flag I.
 */

#if SALSA64_HOST_BIG_ENDIAN
enum { FPR_LO32 = 4, FPR_HI32 = 0 };  /* mitades de un registro de 64 bits en memoria */
#else
enum { FPR_LO32 = 0, FPR_HI32 = 4 };
#endif

/* Accesos directos a cpu->fpr con el tipo de punto flotante (lfs/lfd,
 * stfs/stfd). may_alias los hace válidos sobre el arreglo de uint64_t; con
 * memcpy, GCC deja el float en la pila (stfs + lfs: load-hit-store). */
typedef float __attribute__((may_alias)) fpu_alias_f32;
typedef double __attribute__((may_alias)) fpu_alias_f64;
typedef uint32_t __attribute__((may_alias)) fpu_alias_u32;

FPU_INLINE double fpu_fast_load(const struct salsa64_cpu *cpu, bool is_s, unsigned r)
{
	if (is_s)
		return *(const fpu_alias_f32 *)((const char *)&cpu->fpr[r] + FPR_LO32);
	return *(const fpu_alias_f64 *)&cpu->fpr[r];
}

/* Los mismos bits en un registro entero. volatile impide que GCC aproveche
 * una carga para la otra pasando el valor entre registros (por la pila). */
FPU_INLINE uint64_t fpu_fast_bits(const struct salsa64_cpu *cpu, bool is_s, unsigned r)
{
	if (is_s)
		return *(const volatile fpu_alias_u32 *)((const char *)&cpu->fpr[r] + FPR_LO32);
	return *(const volatile uint64_t *)&cpu->fpr[r];
}

/* Exponente sesgado; |valor| sin signo. */
FPU_INLINE unsigned fpu_fast_exp(uint64_t bits, bool is_s)
{
	return is_s ? (unsigned)(bits >> 23) & 0xFF : (unsigned)(bits >> 52) & 0x7FF;
}

FPU_INLINE uint64_t fpu_fast_mag(uint64_t bits, bool is_s)
{
	return is_s ? bits & 0x7FFFFFFF : bits & 0x7FFFFFFFFFFFFFFFull;
}

/*
 * ¿Cero, o exponente en la franja segura? S: 2^-60 ≤ |x| < 2^61; D: 2^-440 ≤
 * |x| < 2^441. Con dos entradas así, suma y resta no desbordan y, si no dan
 * cero, dan al menos 2^-83 (S) o 2^-492 (D) (múltiplos del último bit del
 * menor); producto y cociente quedan entre 2^-121 y 2^122 (S) o 2^-881 y 2^882
 * (D): por encima del mínimo normal y de TINY_RESULT.
 */
FPU_INLINE bool fpu_fast_band(uint64_t bits, bool is_s)
{
	unsigned e = fpu_fast_exp(bits, is_s);
	return (is_s ? e - 67u <= 120u : e - 583u <= 880u) || fpu_fast_mag(bits, is_s) == 0;
}

/* Escribe las causas (ninguna, o I) sin almacenamiento redundante: la
 * próxima instrucción de COP1 lee FCR31 y chocaría con él. */
FPU_INLINE void fpu_fast_cause(struct salsa64_cpu *cpu, uint32_t fcr31, bool inexact)
{
	uint32_t f = (fcr31 & ~(0x3Fu << 12)) | (inexact ? FPE_I << 12 | FPE_I << 2 : 0);
	if (f != fcr31)
		cpu->fcr31 = f;
}

/* ADD, SUB, MUL y DIV con redondeo al más cercano e I deshabilitada. */
FPU_INLINE bool fpu_fast_arith(struct salsa64_cpu *cpu, uint32_t i, bool is_s, unsigned fs, unsigned funct)
{
	uint32_t fcr31 = cpu->fcr31;
	unsigned ft = I_FT(i);
	if (fcr31 & 0x83)  /* RM ≠ 0 o I habilitada */
		return false;
	uint64_t ba = fpu_fast_bits(cpu, is_s, fs), bb = fpu_fast_bits(cpu, is_s, ft);
	if (!fpu_fast_band(ba, is_s) || !fpu_fast_band(bb, is_s) ||
	    (funct == 0x03 && fpu_fast_mag(bb, is_s) == 0))  /* entre cero: Z o V */
		return false;

	double a = fpu_fast_load(cpu, is_s, fs), b = fpu_fast_load(cpu, is_s, ft), r;
	switch (funct) {
	case 0x01:
		b = -b;  /* SUB = ADD con -b */
		/* fall through */
	case 0x00:
		r = a + b;
		break;
	case 0x02:
		r = a * b;
		break;
	default:
		r = a / b;
		break;
	}

	/* Inexacto: el resultado redondeado no es el exacto. En la franja segura
	 * los residuos (TwoSum, fma) son exactos. */
	bool inexact;
	float f = 0;
	if (is_s) {
		f = (float)r;
		if ((double)f != r) {
			inexact = true;
		} else if (funct <= 0x01) {
			/* Dos float con exponentes a 28 o menos de distancia (o un
			 * cero) suman exacto en double. */
			unsigned ea = fpu_fast_exp(ba, true), eb = fpu_fast_exp(bb, true);
			unsigned gap = ea > eb ? ea - eb : eb - ea;
			inexact = ea && eb && gap > 28 && add_error(a, b, r) != 0;
		} else {
			/* El producto de dos float es exacto en double. */
			inexact = funct == 0x03 && __builtin_fma(-r, b, a) != 0;
		}
	} else if (funct <= 0x01) {
		double bb2 = r - a;
		inexact = (a - (r - bb2)) + (b - bb2) != 0;  /* TwoSum (add_error) */
	} else {
		inexact = (funct == 0x02 ? __builtin_fma(a, b, -r) : __builtin_fma(-r, b, a)) != 0;
	}

	fpu_fast_cause(cpu, fcr31, inexact);
	unsigned fd = I_FD(i);
	if (is_s) {
		*(fpu_alias_f32 *)((char *)&cpu->fpr[fd] + FPR_LO32) = f;
		*(fpu_alias_u32 *)((char *)&cpu->fpr[fd] + FPR_HI32) = 0;
	} else {
		*(fpu_alias_f64 *)&cpu->fpr[fd] = r;
	}
	return true;
}

/* ABS y NEG: cambian el bit de signo. NaN y desnormales van al camino
 * completo (V o E); infinito, cero y normales son exactos y sin causa. */
FPU_INLINE bool fpu_fast_abs_neg(struct salsa64_cpu *cpu, uint32_t i, bool is_s, unsigned fs, bool neg)
{
	uint64_t b = fpu_fast_bits(cpu, is_s, fs), sign = is_s ? 0x80000000u : 0x8000000000000000ull;
	unsigned e = fpu_fast_exp(b, is_s), emax = is_s ? 0xFF : 0x7FF;
	uint64_t mant = b & (is_s ? 0x7FFFFFull : 0xFFFFFFFFFFFFFull);
	if (mant && (e == 0 || e == emax))
		return false;
	uint32_t fcr31 = cpu->fcr31;
	fpu_fast_cause(cpu, fcr31, false);
	cpu->fpr[I_FD(i)] = neg ? b ^ sign : b & ~sign;  /* en S, los 32 bits altos en 0 */
	return true;
}

/* C.cond sin NaN: no hay causa y la comparación se hace en enteros. Signo y
 * magnitud pasan a un entero ordenado (±0 dan 0); los desnormales comparan
 * bien así. */
FPU_INLINE bool fpu_fast_compare(struct salsa64_cpu *cpu, uint32_t i, bool is_s, unsigned fs, unsigned cond)
{
	uint64_t ba = fpu_fast_bits(cpu, is_s, fs), bb = fpu_fast_bits(cpu, is_s, I_FT(i));
	uint64_t ma = fpu_fast_mag(ba, is_s), mb = fpu_fast_mag(bb, is_s);
	uint64_t inf = is_s ? 0x7F800000u : 0x7FF0000000000000ull;
	if (ma > inf || mb > inf)
		return false;
	int64_t sa, sb;  /* 0 o -1 según el signo */
	if (is_s) {
		sa = -(int64_t)(ba >> 31);
		sb = -(int64_t)(bb >> 31);
	} else {
		sa = -(int64_t)(ba >> 63);
		sb = -(int64_t)(bb >> 63);
	}
	int64_t ka = ((int64_t)ma ^ sa) - sa, kb = ((int64_t)mb ^ sb) - sb;
	uint32_t fcr31 = cpu->fcr31, f = fcr31 & ~(0x3Fu << 12);
	if (((cond & 4) && ka < kb) || ((cond & 2) && ka == kb))
		f |= SALSA64_FCR31_C;
	else
		f &= ~SALSA64_FCR31_C;
	if (f != fcr31)
		cpu->fcr31 = f;
	return true;
}

FPU_INLINE bool vr4300_cop1_fast(struct salsa64_cpu *cpu, uint32_t i)
{
	uint32_t status = cpu->status;
	if (!(status & SALSA64_ST_CU1))
		return false;
	unsigned fmt = I_FMT(i), fs = I_FS(i), rt = I_RT(i);

	if ((fmt & ~1u) == FMT_S) {  /* S o D */
		bool is_s = fmt == FMT_S;
		unsigned funct = I_FUNCT(i), fsr = (status & SALSA64_ST_FR) ? fs : fs & ~1u;
		if (funct <= 0x03)
			return fpu_fast_arith(cpu, i, is_s, fsr, funct);
		if (funct >= 0x30)
			return fpu_fast_compare(cpu, i, is_s, fsr, funct & 15);
		if (funct == 0x06) {  /* MOV: 64 bits, sin causa */
			cpu->fpr[I_FD(i)] = cpu->fpr[fsr];
			return true;
		}
		if (funct == 0x05 || funct == 0x07)
			return fpu_fast_abs_neg(cpu, i, is_s, fsr, funct == 0x07);
		return false;
	}
	switch (fmt) {
	case 0x00:                                                        /* MFC1 */
		vr4300_set_gpr(cpu, rt, salsa64_sx32(vr4300_fpr_get32(cpu, fs)));
		return true;
	case 0x04:                                                        /* MTC1 */
		vr4300_fpr_set32(cpu, fs, (uint32_t)cpu->gpr[rt]);
		return true;
	case 0x01:                                                        /* DMFC1 */
		if (!cpu->allow64)
			return false;
		vr4300_set_gpr(cpu, rt, vr4300_fpr_get64(cpu, fs));
		return true;
	case 0x05:                                                        /* DMTC1 */
		if (!cpu->allow64)
			return false;
		vr4300_fpr_set64(cpu, fs, cpu->gpr[rt]);
		return true;
	case 0x02:                                                        /* CFC1 */
		vr4300_set_gpr(cpu, rt, salsa64_sx32(fs == 0 ? SALSA64_FCR0_VALUE : fs == 31 ? cpu->fcr31 : 0));
		return true;
	default:
		return false;
	}
}

#endif
