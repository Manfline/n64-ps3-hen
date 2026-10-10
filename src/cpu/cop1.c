/*
 * Retro64 — COP1 (FPU) del VR4300.
 *
 * Las operaciones de formato S se calculan en double y se redondean a float
 * una sola vez: para +, -, *, / y raíz cuadrada eso da el mismo resultado
 * que la operación en precisión simple (53 ≥ 2·24 + 2), y deja un solo
 * camino para S y D. Se compila con -ffp-contract=off para que el
 * compilador no fusione multiplicación y suma (fmadd en el PPE).
 *
 * Reglas del VR4300 que se modelan (pendientes de validar contra
 * n64-systemtest; ver docs/cpu-vr4300.md):
 *   - Entradas desnormales o sNaN → Unimplemented Operation (causa E, que
 *     siempre provoca la excepción). qNaN → Invalid (V). Ojo: en MIPS el
 *     bit alto de la mantisa en 1 marca sNaN, al revés que en IEEE 754-2008.
 *   - Resultado desnormal o que se va a cero: E si FCR31.FS = 0; con FS = 1
 *     se aplana a ±0 con U e I.
 *   - NaN generado: el qNaN por omisión de MIPS (0x7FBFFFFF / 0x7FF7FF…).
 *   - Conversión a entero de NaN, infinito o fuera de rango → E.
 *   - CVT.S.L/CVT.D.L con |valor| ≥ 2^55 → E.
 * Limitaciones de este hito: la aritmética siempre redondea al más
 * cercano (las conversiones a entero sí respetan el modo) y la bandera I
 * solo se calcula en conversiones.
 */
#include <string.h>

#include "cpu/vr4300_internal.h"

enum { FPE_I = 0x01, FPE_U = 0x02, FPE_O = 0x04, FPE_Z = 0x08, FPE_V = 0x10, FPE_E = 0x20 };

enum { FMT_S = 0x10, FMT_D = 0x11, FMT_W = 0x14, FMT_L = 0x15 };

#define FCR31_WMASK 0x0183FFFFu
#define NAN_S       0x7FBFFFFFu
#define NAN_D       0x7FF7FFFFFFFFFFFFull

static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, sizeof d); return d; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }

/* Escribe la causa en FCR31; si alguna está habilitada (E siempre lo está)
 * lanza la excepción y devuelve false. Si no, acumula las banderas. */
static bool fpu_finish(struct retro64_cpu *cpu, unsigned cause)
{
	uint32_t f = (cpu->fcr31 & ~(0x3Fu << 12)) | cause << 12;
	unsigned enabled = ((f >> 7) & 0x1F) | FPE_E;
	if (cause & enabled) {
		cpu->fcr31 = f;
		retro64_vr4300_exception(cpu, RETRO64_EXC_FPE, 0);
		return false;
	}
	cpu->fcr31 = f | (cause & 0x1Fu) << 2;
	return true;
}

static uint64_t fpu_read(const struct retro64_cpu *cpu, unsigned fmt, unsigned r)
{
	return (fmt == FMT_S || fmt == FMT_W) ? vr4300_fpr_get32(cpu, r) : vr4300_fpr_get64(cpu, r);
}

/* 0 = número, 1 = qNaN, 2 = sNaN (convención MIPS). */
static int nan_kind(uint64_t bits, unsigned fmt)
{
	if (fmt == FMT_S) {
		uint32_t b = (uint32_t)bits;
		if ((b & 0x7F800000) != 0x7F800000 || !(b & 0x7FFFFF))
			return 0;
		return (b & 0x400000) ? 2 : 1;
	}
	if ((bits & 0x7FF0000000000000ull) != 0x7FF0000000000000ull || !(bits & 0xFFFFFFFFFFFFFull))
		return 0;
	return (bits & 0x8000000000000ull) ? 2 : 1;
}

/* Convierte una entrada S o D a double y devuelve la causa que provoca. */
static unsigned fpu_input(uint64_t bits, unsigned fmt, double *out)
{
	bool denormal;
	if (fmt == FMT_S) {
		uint32_t b = (uint32_t)bits;
		*out = (double)u2f(b);
		denormal = !(b & 0x7F800000) && (b & 0x7FFFFF);
	} else {
		*out = u2d(bits);
		denormal = !(bits & 0x7FF0000000000000ull) && (bits & 0xFFFFFFFFFFFFFull);
	}
	if (denormal)
		return FPE_E;
	switch (nan_kind(bits, fmt)) {
	case 1:  return FPE_V;
	case 2:  return FPE_E;
	default: return 0;
	}
}

/* Resultado diminuto (desnormal o redondeado a cero). */
static unsigned fpu_tiny(const struct retro64_cpu *cpu)
{
	return (cpu->fcr31 & RETRO64_FCR31_FS) ? (FPE_U | FPE_I) : FPE_E;
}

/* Redondea `r` al formato, aplica las reglas de resultado y escribe fd si
 * no hubo excepción. exact_inf: un infinito en el resultado es exacto
 * (vino de una entrada infinita o de dividir entre cero), no desbordamiento. */
static void fpu_result(struct retro64_cpu *cpu, unsigned fmt, unsigned fd, double r,
		       unsigned cause, bool exact_inf)
{
	if (fmt == FMT_S) {
		float f = (float)r;
		uint32_t b = f2u(f);
		if (r != r) {
			b = NAN_S;
			cause |= FPE_V;
		} else if ((b & 0x7FFFFFFF) == 0x7F800000) {
			if (!exact_inf)
				cause |= FPE_O | FPE_I;
		} else if ((!(b & 0x7F800000) && (b & 0x7FFFFF)) || (f == 0 && r != 0)) {
			cause |= fpu_tiny(cpu);
			b &= 0x80000000u;
		}
		if (fpu_finish(cpu, cause))
			vr4300_fpr_set32(cpu, fd, b);
	} else {
		uint64_t b = d2u(r);
		if (r != r) {
			b = NAN_D;
			cause |= FPE_V;
		} else if ((b & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull) {
			if (!exact_inf)
				cause |= FPE_O | FPE_I;
		} else if (!(b & 0x7FF0000000000000ull) && (b & 0xFFFFFFFFFFFFFull)) {
			cause |= fpu_tiny(cpu);
			b &= 0x8000000000000000ull;
		}
		if (fpu_finish(cpu, cause))
			vr4300_fpr_set64(cpu, fd, b);
	}
}

/* ADD, SUB, MUL, DIV, SQRT, ABS, NEG. */
static void fpu_arith(struct retro64_cpu *cpu, uint32_t i, unsigned fmt, unsigned funct)
{
	double a, b = 0, r;
	unsigned cause = fpu_input(fpu_read(cpu, fmt, I_FS(i)), fmt, &a);
	if (funct <= 3)
		cause |= fpu_input(fpu_read(cpu, fmt, I_FT(i)), fmt, &b);
	if (cause & FPE_E) {
		fpu_finish(cpu, cause);
		return;
	}

	bool exact_inf = __builtin_isinf(a) || __builtin_isinf(b);
	switch (funct) {
	case 0x00: r = a + b; break;
	case 0x01: r = a - b; break;
	case 0x02: r = a * b; break;
	case 0x03:
		if (b == 0 && a == a && a != 0 && !__builtin_isinf(a)) {
			cause |= FPE_Z;
			exact_inf = true;
		}
		r = a / b;
		break;
	case 0x04: r = __builtin_sqrt(a); break;
	case 0x05: r = __builtin_fabs(a); break;
	default:   r = -a; break;  /* 0x07 NEG */
	}
	fpu_result(cpu, fmt, I_FD(i), r, cause, exact_inf);
}

/* Redondeo a entero según el modo (0 = más cercano par, 1 = a cero,
 * 2 = a +inf, 3 = a -inf). No usa libm. */
static double fpu_round(double x, unsigned mode)
{
	if (!(x < 4503599627370496.0 && x > -4503599627370496.0))  /* |x| ≥ 2^52: ya es entero */
		return x;
	double t = (double)(int64_t)x, d = x - t;  /* d es exacto */
	switch (mode & 3) {
	case 0:
		if (d > 0.5 || (d == 0.5 && ((int64_t)t & 1)))
			t += 1.0;
		else if (d < -0.5 || (d == -0.5 && ((int64_t)t & 1)))
			t -= 1.0;
		return t;
	case 1:  return t;
	case 2:  return t < x ? t + 1.0 : t;
	default: return t > x ? t - 1.0 : t;
	}
}

static void fpu_to_int(struct retro64_cpu *cpu, uint32_t i, unsigned fmt, unsigned mode, bool to64)
{
	double a;
	unsigned cause = fpu_input(fpu_read(cpu, fmt, I_FS(i)), fmt, &a);
	if (a != a || __builtin_isinf(a))
		cause = (cause & ~(unsigned)FPE_V) | FPE_E;
	if (cause & FPE_E) {
		fpu_finish(cpu, cause);
		return;
	}

	double r = fpu_round(a, mode);
	bool out_of_range = to64 ? (r >= 9223372036854775808.0 || r < -9223372036854775808.0)
				 : (r > 2147483647.0 || r < -2147483648.0);
	if (out_of_range) {
		fpu_finish(cpu, cause | FPE_E);
		return;
	}
	if (r != a)
		cause |= FPE_I;
	if (!fpu_finish(cpu, cause))
		return;
	if (to64)
		vr4300_fpr_set64(cpu, I_FD(i), (uint64_t)(int64_t)r);
	else
		vr4300_fpr_set32(cpu, I_FD(i), (uint32_t)(int32_t)r);
}

static void fpu_compare(struct retro64_cpu *cpu, uint32_t i, unsigned fmt, unsigned cond)
{
	uint64_t ra = fpu_read(cpu, fmt, I_FS(i)), rb = fpu_read(cpu, fmt, I_FT(i));
	int ka = nan_kind(ra, fmt), kb = nan_kind(rb, fmt);
	double a = fmt == FMT_S ? (double)u2f((uint32_t)ra) : u2d(ra);
	double b = fmt == FMT_S ? (double)u2f((uint32_t)rb) : u2d(rb);
	bool unordered = ka || kb;

	unsigned cause = 0;
	if (unordered && ((cond & 8) || ka == 2 || kb == 2))
		cause |= FPE_V;
	if (!fpu_finish(cpu, cause))
		return;

	bool c = ((cond & 4) && !unordered && a < b) ||
		 ((cond & 2) && !unordered && a == b) ||
		 ((cond & 1) && unordered);
	if (c)
		cpu->fcr31 |= RETRO64_FCR31_C;
	else
		cpu->fcr31 &= ~RETRO64_FCR31_C;
}

/* Instrucciones de formato S y D. */
static void fpu_float(struct retro64_cpu *cpu, uint32_t i, unsigned fmt)
{
	unsigned funct = I_FUNCT(i);
	double a;
	unsigned cause;

	switch (funct) {
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x07:
		fpu_arith(cpu, i, fmt, funct);
		return;
	case 0x06:  /* MOV: sin verificación ni causa */
		if (fmt == FMT_S)
			vr4300_fpr_set32(cpu, I_FD(i), vr4300_fpr_get32(cpu, I_FS(i)));
		else
			vr4300_fpr_set64(cpu, I_FD(i), vr4300_fpr_get64(cpu, I_FS(i)));
		return;
	case 0x08: case 0x09: case 0x0A: case 0x0B:  /* ROUND/TRUNC/CEIL/FLOOR.L */
		fpu_to_int(cpu, i, fmt, funct & 3, true);
		return;
	case 0x0C: case 0x0D: case 0x0E: case 0x0F:  /* ROUND/TRUNC/CEIL/FLOOR.W */
		fpu_to_int(cpu, i, fmt, funct & 3, false);
		return;
	case 0x20:  /* CVT.S.D */
	case 0x21:  /* CVT.D.S */
		if ((funct == 0x20) == (fmt == FMT_S))
			break;
		cause = fpu_input(fpu_read(cpu, fmt, I_FS(i)), fmt, &a);
		if (cause & FPE_E) {
			fpu_finish(cpu, cause);
			return;
		}
		fpu_result(cpu, funct == 0x20 ? FMT_S : FMT_D, I_FD(i), a, cause, __builtin_isinf(a));
		return;
	case 0x24: fpu_to_int(cpu, i, fmt, cpu->fcr31 & 3, false); return;  /* CVT.W */
	case 0x25: fpu_to_int(cpu, i, fmt, cpu->fcr31 & 3, true); return;   /* CVT.L */
	default:
		if (funct >= 0x30) {
			fpu_compare(cpu, i, fmt, funct & 15);
			return;
		}
		break;
	}
	fpu_finish(cpu, FPE_E);
}

/* Instrucciones de formato W y L: solo CVT.S y CVT.D. */
static void fpu_int(struct retro64_cpu *cpu, uint32_t i, unsigned fmt)
{
	unsigned funct = I_FUNCT(i);
	int64_t v = fmt == FMT_W ? (int64_t)(int32_t)vr4300_fpr_get32(cpu, I_FS(i))
				 : (int64_t)vr4300_fpr_get64(cpu, I_FS(i));

	if ((funct != 0x20 && funct != 0x21) ||
	    (fmt == FMT_L && (v >= (1ll << 55) || v < -(1ll << 55)))) {
		fpu_finish(cpu, FPE_E);
		return;
	}
	if (funct == 0x20) {
		float f = (float)v;  /* un solo redondeo, también desde 64 bits */
		if (fpu_finish(cpu, (int64_t)f != v ? FPE_I : 0))
			vr4300_fpr_set32(cpu, I_FD(i), f2u(f));
	} else {
		double d = (double)v;
		if (fpu_finish(cpu, (int64_t)d != v ? FPE_I : 0))
			vr4300_fpr_set64(cpu, I_FD(i), d2u(d));
	}
}

void retro64_vr4300_cop1(struct retro64_cpu *cpu, uint32_t i)
{
	if (!(cpu->status & RETRO64_ST_CU1)) {
		retro64_vr4300_exception(cpu, RETRO64_EXC_CPU, 1);
		return;
	}

	unsigned rt = I_RT(i), fs = I_FS(i);
	switch (I_FMT(i)) {
	case 0x00:                                                        /* MFC1 */
		cpu->gpr[rt] = retro64_sx32(vr4300_fpr_get32(cpu, fs));
		return;
	case 0x01:                                                        /* DMFC1 */
		if (!cpu->allow64)
			break;
		cpu->gpr[rt] = vr4300_fpr_get64(cpu, fs);
		return;
	case 0x02:                                                        /* CFC1 */
		cpu->gpr[rt] = retro64_sx32(fs == 0 ? RETRO64_FCR0_VALUE : fs == 31 ? cpu->fcr31 : 0);
		return;
	case 0x04:                                                        /* MTC1 */
		vr4300_fpr_set32(cpu, fs, (uint32_t)cpu->gpr[rt]);
		return;
	case 0x05:                                                        /* DMTC1 */
		if (!cpu->allow64)
			break;
		vr4300_fpr_set64(cpu, fs, cpu->gpr[rt]);
		return;
	case 0x06:                                                        /* CTC1 */
		if (fs == 31) {
			cpu->fcr31 = (uint32_t)cpu->gpr[rt] & FCR31_WMASK;
			unsigned cause = (cpu->fcr31 >> 12) & 0x3F;
			if (cause & (((cpu->fcr31 >> 7) & 0x1F) | FPE_E))
				retro64_vr4300_exception(cpu, RETRO64_EXC_FPE, 0);
		}
		return;
	case 0x08: {                                                      /* BC1F/T/FL/TL */
		bool c = (cpu->fcr31 & RETRO64_FCR31_C) != 0;
		switch (rt & 3) {
		case 0: vr4300_branch(cpu, !c, i); return;
		case 1: vr4300_branch(cpu, c, i); return;
		case 2: vr4300_branch_likely(cpu, !c, i); return;
		default: vr4300_branch_likely(cpu, c, i); return;
		}
	}
	case FMT_S:
	case FMT_D:
		fpu_float(cpu, i, I_FMT(i));
		return;
	case FMT_W:
	case FMT_L:
		fpu_int(cpu, i, I_FMT(i));
		return;
	default:
		break;
	}
	vr4300_reserved(cpu);
}
