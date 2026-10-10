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
 * ADD, SUB, MUL, DIV y C.cond en S y D para el caso común: entradas cero o
 * normales, redondeo al más cercano, excepción I deshabilitada y resultado
 * normal o cero exacto. Da lo mismo que el camino completo (la prueba
 * fast_path_matches_full los compara); cualquier otro caso devuelve false y
 * va por salsa64_vr4300_cop1.
 *
 * Pensado para el PPE: los operandos van de cpu->fpr directo a registros de
 * punto flotante (lfs/lfd), el resultado vuelve igual y la clasificación se
 * hace con comparaciones de punto flotante. El camino completo trabaja con
 * los bits en registros enteros, y en el PPE cada paso entre unos y otros va
 * por memoria (load-hit-store, decenas de ciclos).
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

/* Cero, o normal y finito. NaN, infinito y desnormales no lo son. */
FPU_INLINE bool fpu_fast_input(double x, double minnorm, double max)
{
	double ax = __builtin_fabs(x);
	return (ax >= minnorm && ax <= max) || x == 0;
}

FPU_INLINE bool vr4300_cop1_fast(struct salsa64_cpu *cpu, uint32_t i)
{
	unsigned fmt = I_FMT(i), funct = I_FUNCT(i);
	if ((fmt != FMT_S && fmt != FMT_D) || (funct > 0x03 && funct < 0x30) ||
	    !(cpu->status & SALSA64_ST_CU1))
		return false;

	bool is_s = fmt == FMT_S;
	unsigned fs = (cpu->status & SALSA64_ST_FR) ? I_FS(i) : I_FS(i) & ~1u;
	double a = fpu_fast_load(cpu, is_s, fs), b = fpu_fast_load(cpu, is_s, I_FT(i));
	uint32_t fcr31 = cpu->fcr31, f31 = fcr31 & ~(0x3Fu << 12);

	if (funct >= 0x30) {  /* C.cond: sin NaN no hay causa */
		if (a != a || b != b)
			return false;
		unsigned cond = funct & 15;
		if (((cond & 4) && a < b) || ((cond & 2) && a == b))
			f31 |= SALSA64_FCR31_C;
		else
			f31 &= ~SALSA64_FCR31_C;
		if (f31 != fcr31)  /* sin almacenamiento redundante que la próxima lectura choque */
			cpu->fcr31 = f31;
		return true;
	}

	/* Redondeo al más cercano (RM = 0) e I deshabilitada (bit 7). */
	if (fcr31 & 0x83)
		return false;
	double minnorm = is_s ? 0x1p-126 : 0x1p-1022;
	double max = is_s ? 0x1.fffffep127 : 0x1.fffffffffffffp1023;
	if (!fpu_fast_input(a, minnorm, max) || !fpu_fast_input(b, minnorm, max))
		return false;

	double r;
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
		if (b == 0)
			return false;
		r = a / b;
		break;
	}

	/* Cero: exacto en la suma (de no ceros sale al menos el mínimo
	 * desnormal); en producto y cociente solo con un factor o dividendo 0.
	 * Si no, lejos del subdesbordamiento y de donde el fma pierde el residuo. */
	if (r == 0) {
		if ((funct == 0x02 && a != 0 && b != 0) || (funct == 0x03 && a != 0))
			return false;
	} else if (!(__builtin_fabs(r) >= (is_s ? 0x1p-125 : TINY_RESULT) &&
		     __builtin_fabs(r) <= 0x1.fffffffffffffp1023)) {
		return false;
	}

	bool inexact;
	float f = 0;
	if (is_s) {
		f = (float)r;
		if (__builtin_isinf(f))
			return false;
		/* Si el float no es r, es inexacto; si lo es, depende de si r lo fue
		 * (el producto de dos float es exacto en double). */
		inexact = (double)f != r ||
			  (funct <= 0x01 ? add_error(a, b, r) : funct == 0x03 ? div_error(a, b, r) : 0) != 0;
	} else {
		inexact = (funct <= 0x01 ? add_error(a, b, r)
			   : funct == 0x02 ? mul_error(a, b, r) : div_error(a, b, r)) != 0;
	}

	if (inexact)
		f31 |= FPE_I << 12 | FPE_I << 2;
	if (f31 != fcr31)
		cpu->fcr31 = f31;
	unsigned fd = I_FD(i);
	if (is_s) {
		*(fpu_alias_f32 *)((char *)&cpu->fpr[fd] + FPR_LO32) = f;
		*(fpu_alias_u32 *)((char *)&cpu->fpr[fd] + FPR_HI32) = 0;
	} else {
		*(fpu_alias_f64 *)&cpu->fpr[fd] = r;
	}
	return true;
}

#endif
