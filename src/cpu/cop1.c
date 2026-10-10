/*
 * Salsa64 — COP1 (FPU) del VR4300.
 *
 * Reglas del hardware que se modelan (comprobadas con nemu64-test; ver
 * docs/cpu-vr4300.md):
 *   - Registros en las instrucciones de cálculo: fs ignora el bit bajo con
 *     FR = 0, ft no (se lee el registro físico); el resultado se escribe en el
 *     registro físico fd completo (64 bits; los resultados S y W dejan los 32
 *     altos en 0). MOV.S copia los 64 bits, como MOV.D. MFC1, MTC1, LWC1 y
 *     SWC1 usan en cambio la mitad alta del par para un índice impar con
 *     FR = 0 (vr4300_internal.h).
 *   - NaN de entrada: con el bit alto de la mantisa en 1 (qNaN de IEEE
 *     754-2008) da Invalid (V) y el NaN por omisión de MIPS (0x7FBFFFFF /
 *     0x7FF7FFFF…); con ese bit en 0 (sNaN de IEEE, incluido el propio NaN
 *     por omisión) da Unimplemented (E), que siempre provoca la excepción.
 *     Una entrada desnormal también da E.
 *   - Resultado diminuto (|exacto| < mínimo normal): E, salvo con FCR31.FS = 1
 *     y las excepciones U e I deshabilitadas; entonces se aplana con U e I a
 *     ±0 o, si el modo redondea hacia afuera, a ±mínimo normal.
 *   - Desbordamiento: O e I, con ±inf o ±máximo según el modo de redondeo.
 *   - Toda operación salvo MOV pone en 0 las causas antes de escribir la suya.
 *   - Conversión a entero de NaN, infinito o fuera de rango → E; a 64 bits
 *     el rango es |x| < 2^53. CVT.S/D.L con |valor| ≥ 2^55 → E.
 *   - Si una entrada da E, la causa es solo E aunque la otra dé V.
 *   - Una instrucción de COP1 que no existe da E (excepción de punto
 *     flotante), no Reserved Instruction.
 *
 * Redondeo: el host calcula cada operación en double con redondeo al más
 * cercano y el signo del error exacto sale de TwoSum (suma) o de un fma (el
 * residuo de producto, división y raíz es exacto). Con eso se aplican los
 * modos dirigidos y la bandera I. El formato S se calcula en double y se
 * redondea a float: con redondeo al más cercano el doble redondeo es inocuo
 * para +, -, *, / y raíz (53 ≥ 2·24 + 2), y el signo del error contra el
 * float se deduce del double. Se compila con -ffp-contract=off: los únicos
 * fma son los explícitos. Nunca se cambia el modo de redondeo del host.
 *
 * Lo frecuente (MFC1, MTC1, CFC1, MOV, ABS, NEG, C.cond y ADD/SUB/MUL/DIV con
 * entradas normales y redondeo al más cercano) lo resuelve antes un camino
 * rápido integrado en el bucle del intérprete (vr4300_cop1_fast en fpu.h);
 * lo demás llega aquí.
 */
#include <string.h>

#include "cpu/fpu.h"

#define FCR31_WMASK   0x0183FFFFu
#define DEFNAN_S      0x7FBFFFFFu
#define DEFNAN_D      0x7FF7FFFFFFFFFFFFull
#define MAX_S         0x7F7FFFFFu
#define MAX_D         0x7FEFFFFFFFFFFFFFull
#define MINNORM_S     0x00800000u
#define MINNORM_D     0x0010000000000000ull
#define INF_S         0x7F800000u
#define INF_D         0x7FF0000000000000ull
#define SIGN_S        0x80000000u
#define SIGN_D        0x8000000000000000ull

static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, sizeof d); return d; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }

static inline bool is32(unsigned fmt) { return fmt == FMT_S || fmt == FMT_W; }

/* ---- Registros ------------------------------------------------------------ */

/* Operando fs de una instrucción de cálculo: con FR = 0 se ignora el bit bajo. */
static inline uint64_t fpu_read(const struct salsa64_cpu *cpu, unsigned fmt, unsigned r)
{
	uint64_t v = cpu->fpr[(cpu->status & SALSA64_ST_FR) ? r : r & ~1u];
	return is32(fmt) ? (uint32_t)v : v;
}

/* Operando ft: el registro físico tal cual, también con FR = 0. */
static inline uint64_t fpu_read_ft(const struct salsa64_cpu *cpu, unsigned fmt, unsigned r)
{
	uint64_t v = cpu->fpr[r];
	return is32(fmt) ? (uint32_t)v : v;
}

/* Resultado: el registro físico fd, los 64 bits. */
static inline void fpu_write(struct salsa64_cpu *cpu, unsigned fd, uint64_t v)
{
	cpu->fpr[fd] = v;
}

/* ---- Causas y excepciones --------------------------------------------------- */

/* Escribe la causa en FCR31; si alguna está habilitada (E siempre lo está)
 * lanza la excepción y devuelve false. Si no, acumula las banderas. */
static bool fpu_finish(struct salsa64_cpu *cpu, unsigned cause)
{
	uint32_t f = (cpu->fcr31 & ~(0x3Fu << 12)) | cause << 12;
	unsigned enabled = ((f >> 7) & 0x1F) | FPE_E;
	if (cause & enabled) {
		cpu->fcr31 = f;
		salsa64_vr4300_exception(cpu, SALSA64_EXC_FPE, 0);
		return false;
	}
	cpu->fcr31 = f | (cause & 0x1Fu) << 2;
	return true;
}

/* 0 = número; si no, NaN con el bit alto de la mantisa en 1 (V) o en 0 (E). */
enum { NOT_NAN, NAN_QUIET_BIT, NAN_NO_QUIET_BIT };

static int nan_kind(uint64_t bits, unsigned fmt)
{
	if (fmt == FMT_S) {
		uint32_t b = (uint32_t)bits;
		if ((b & INF_S) != INF_S || !(b & 0x7FFFFF))
			return NOT_NAN;
		return (b & 0x400000) ? NAN_QUIET_BIT : NAN_NO_QUIET_BIT;
	}
	if ((bits & INF_D) != INF_D || !(bits & 0xFFFFFFFFFFFFFull))
		return NOT_NAN;
	return (bits & 0x8000000000000ull) ? NAN_QUIET_BIT : NAN_NO_QUIET_BIT;
}

/* Convierte una entrada S o D a double y devuelve la causa que provoca. */
static unsigned fpu_input(uint64_t bits, unsigned fmt, double *out)
{
	bool denormal;
	if (fmt == FMT_S) {
		uint32_t b = (uint32_t)bits;
		*out = (double)u2f(b);
		denormal = !(b & INF_S) && (b & 0x7FFFFF);
	} else {
		*out = u2d(bits);
		denormal = !(bits & INF_D) && (bits & 0xFFFFFFFFFFFFFull);
	}
	if (denormal)
		return FPE_E;
	switch (nan_kind(bits, fmt)) {
	case NAN_QUIET_BIT:    return FPE_V;
	case NAN_NO_QUIET_BIT: return FPE_E;
	default:               return 0;
	}
}

/* ---- Redondeo del resultado ------------------------------------------------- */

static void fpu_store(struct salsa64_cpu *cpu, unsigned fd, uint64_t bits, unsigned cause)
{
	if (fpu_finish(cpu, cause))
		fpu_write(cpu, fd, bits);
}

static void fpu_overflow(struct salsa64_cpu *cpu, unsigned fmt, unsigned fd, bool neg, unsigned cause)
{
	unsigned mode = cpu->fcr31 & 3;
	bool to_inf = mode == RM_NEAREST || (mode == RM_PLUS && !neg) || (mode == RM_MINUS && neg);
	uint64_t bits = fmt == FMT_S ? (to_inf ? INF_S : MAX_S) | (neg ? SIGN_S : 0)
				     : (to_inf ? INF_D : MAX_D) | (neg ? SIGN_D : 0);
	fpu_store(cpu, fd, bits, cause | FPE_O | FPE_I);
}

static void fpu_tiny(struct salsa64_cpu *cpu, unsigned fmt, unsigned fd, bool neg, unsigned cause)
{
	unsigned en = (cpu->fcr31 >> 7) & 0x1F;
	if (!(cpu->fcr31 & SALSA64_FCR31_FS) || (en & (FPE_U | FPE_I))) {
		fpu_finish(cpu, cause | FPE_E);
		return;
	}
	unsigned mode = cpu->fcr31 & 3;
	bool to_min = (mode == RM_PLUS && !neg) || (mode == RM_MINUS && neg);
	uint64_t bits = fmt == FMT_S ? (to_min ? MINNORM_S : 0) | (neg ? SIGN_S : 0)
				     : (to_min ? MINNORM_D : 0) | (neg ? SIGN_D : 0);
	fpu_store(cpu, fd, bits, cause | FPE_U | FPE_I);
}

/* ¿Hay que alejar de cero (+1) o acercar (-1) el resultado redondeado al más
 * cercano `r` para el modo actual? dir es el signo de (exacto - r). */
static int directed_step(unsigned mode, bool neg, int dir)
{
	if (dir == 0)
		return 0;
	switch (mode) {
	case RM_ZERO:  return (dir > 0) == neg ? -1 : 0;
	case RM_PLUS:  return dir > 0 ? (neg ? -1 : 1) : 0;
	case RM_MINUS: return dir < 0 ? (neg ? 1 : -1) : 0;
	default:       return 0;
	}
}

/*
 * Escribe el resultado de una operación S o D. `r` es el resultado exacto
 * redondeado al más cercano en double y `dir` el signo de (exacto - r).
 * exact_inf: un infinito en r es exacto (vino de una entrada infinita o de
 * dividir entre cero), no desbordamiento.
 */
static void fpu_result(struct salsa64_cpu *cpu, unsigned fmt, unsigned fd, double r, int dir,
		       unsigned cause, bool exact_inf)
{
	unsigned mode = cpu->fcr31 & 3;
	bool neg = __builtin_signbit(r) != 0;

	if (r != r) {
		fpu_store(cpu, fd, fmt == FMT_S ? DEFNAN_S : DEFNAN_D, cause | FPE_V);
		return;
	}
	if (__builtin_isinf(r)) {
		if (!exact_inf) {
			fpu_overflow(cpu, fmt, fd, neg, cause);
			return;
		}
		fpu_store(cpu, fd, fmt == FMT_S ? (INF_S | (neg ? SIGN_S : 0)) : d2u(r), cause);
		return;
	}
	if (r == 0 && dir == 0) {
		fpu_store(cpu, fd, fmt == FMT_S ? (neg ? SIGN_S : 0) : d2u(r), cause);
		return;
	}

	/* |exacto| < mínimo normal: el exacto está del lado de cero respecto de r
	 * cuando r ya es el mínimo normal. */
	double minnorm = fmt == FMT_S ? 0x1p-126 : 0x1p-1022;
	double ar = __builtin_fabs(r);
	if (ar < minnorm || (ar == minnorm && dir != 0 && (dir > 0) == neg)) {
		fpu_tiny(cpu, fmt, fd, r == 0 ? dir < 0 : neg, cause);
		return;
	}

	if (fmt == FMT_D) {
		uint64_t b = d2u(r);
		int step = directed_step(mode, neg, dir);
		b = step > 0 ? b + 1 : step < 0 ? b - 1 : b;  /* magnitud ± 1 ulp */
		if ((b & ~SIGN_D) == INF_D) {
			fpu_overflow(cpu, fmt, fd, neg, cause);
			return;
		}
		fpu_store(cpu, fd, b, cause | (dir ? FPE_I : 0));
		return;
	}

	float f = (float)r;
	if (__builtin_isinf(f)) {
		fpu_overflow(cpu, fmt, fd, neg, cause);
		return;
	}
	uint32_t b = f2u(f);
	if (dir != 0 && (double)f != r) {
		/* Doble redondeo: si r cayó justo a medio camino entre dos float (solo
		 * puede pasar en CVT.S.L), decide el lado el signo del error. */
		uint32_t nb = __builtin_fabs(r) > __builtin_fabs((double)f) ? b + 1 : b - 1;
		double g = (double)u2f(nb);
		if (r - (double)f == g - r && sign_of(g - r) == dir)
			b = nb;
	}
	int dirf = (double)u2f(b) != r ? sign_of(r - (double)u2f(b)) : dir;
	int step = directed_step(mode, neg, dirf);
	b = step > 0 ? b + 1 : step < 0 ? b - 1 : b;
	if ((b & ~SIGN_S) == INF_S) {
		fpu_overflow(cpu, fmt, fd, neg, cause);
		return;
	}
	fpu_store(cpu, fd, b, cause | (dirf ? FPE_I : 0));
}

/* ---- Aritmética -------------------------------------------------------------- */

/* ADD, SUB, MUL, DIV, SQRT, ABS, NEG. */
static void fpu_arith(struct salsa64_cpu *cpu, uint32_t i, unsigned fmt, unsigned funct)
{
	double a, b = 0, r;
	int dir = 0;
	unsigned cause = fpu_input(fpu_read(cpu, fmt, I_FS(i)), fmt, &a);
	if (funct <= 3)
		cause |= fpu_input(fpu_read_ft(cpu, fmt, I_FT(i)), fmt, &b);
	if (cause & FPE_E) {  /* E manda: un NaN de cada tipo o NaN y desnormal dan solo E */
		fpu_finish(cpu, FPE_E);
		return;
	}

	bool exact_inf = __builtin_isinf(a) || __builtin_isinf(b);
	switch (funct) {
	case 0x00:
	case 0x01:
		if (funct == 0x01)
			b = -b;
		r = a + b;
		if (__builtin_isfinite(r)) {
			dir = add_error(a, b, r);
			/* Cancelación exacta: +0, salvo redondeando hacia -inf. */
			if (r == 0 && dir == 0 && __builtin_signbit(a) != __builtin_signbit(b))
				r = (cpu->fcr31 & 3) == RM_MINUS ? -0.0 : 0.0;
		}
		break;
	case 0x02:
		r = a * b;
		/* En S el producto de dos float es exacto en double. */
		if (fmt == FMT_D && __builtin_isfinite(r))
			dir = mul_error(a, b, r);
		break;
	case 0x03:
		if (b == 0 && a == a && a != 0 && !__builtin_isinf(a)) {
			cause |= FPE_Z;
			exact_inf = true;
		}
		r = a / b;
		if (__builtin_isfinite(r) && b != 0 && !__builtin_isinf(b))
			dir = div_error(a, b, r);
		break;
	case 0x04:
		r = __builtin_sqrt(a);
		if (__builtin_isfinite(r) && r > 0)
			dir = sign_of(__builtin_fma(-r, r, a));
		break;
	case 0x05: r = __builtin_fabs(a); break;
	default:   r = -a; break;  /* 0x07 NEG */
	}
	fpu_result(cpu, fmt, I_FD(i), r, dir, cause, exact_inf);
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

static void fpu_to_int(struct salsa64_cpu *cpu, uint32_t i, unsigned fmt, unsigned mode, bool to64)
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
	/* A 64 bits solo convierte |x| < 2^53; lo demás da E (nemu64-test). */
	bool out_of_range = to64 ? (r >= 9007199254740992.0 || r <= -9007199254740992.0)
				 : (r > 2147483647.0 || r < -2147483648.0);
	if (out_of_range) {
		fpu_finish(cpu, cause | FPE_E);
		return;
	}
	if (r != a)
		cause |= FPE_I;
	fpu_store(cpu, I_FD(i), to64 ? (uint64_t)(int64_t)r : (uint32_t)(int32_t)r, cause);
}

static void fpu_compare(struct salsa64_cpu *cpu, uint32_t i, unsigned fmt, unsigned cond)
{
	uint64_t ra = fpu_read(cpu, fmt, I_FS(i)), rb = fpu_read_ft(cpu, fmt, I_FT(i));
	int ka = nan_kind(ra, fmt), kb = nan_kind(rb, fmt);
	double a = fmt == FMT_S ? (double)u2f((uint32_t)ra) : u2d(ra);
	double b = fmt == FMT_S ? (double)u2f((uint32_t)rb) : u2d(rb);
	bool unordered = ka || kb;

	/* En las comparaciones un NaN con el bit alto en 1 siempre da V; uno con
	 * el bit en 0, solo en las comparaciones con señal (cond & 8). */
	unsigned cause = 0;
	if (unordered && ((cond & 8) || ka == NAN_QUIET_BIT || kb == NAN_QUIET_BIT))
		cause |= FPE_V;
	if (!fpu_finish(cpu, cause))
		return;

	bool c = ((cond & 4) && !unordered && a < b) ||
		 ((cond & 2) && !unordered && a == b) ||
		 ((cond & 1) && unordered);
	if (c)
		cpu->fcr31 |= SALSA64_FCR31_C;
	else
		cpu->fcr31 &= ~SALSA64_FCR31_C;
}

/* Instrucciones de formato S y D. */
static void fpu_float(struct salsa64_cpu *cpu, uint32_t i, unsigned fmt)
{
	unsigned funct = I_FUNCT(i);
	double a;
	unsigned cause;

	switch (funct) {
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x07:
		fpu_arith(cpu, i, fmt, funct);
		return;
	case 0x06:  /* MOV.S/MOV.D: 64 bits, sin verificación ni causa */
		fpu_write(cpu, I_FD(i), fpu_read(cpu, FMT_D, I_FS(i)));
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
		fpu_result(cpu, funct == 0x20 ? FMT_S : FMT_D, I_FD(i), a, 0, cause, __builtin_isinf(a));
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
static void fpu_int(struct salsa64_cpu *cpu, uint32_t i, unsigned fmt)
{
	unsigned funct = I_FUNCT(i);
	int64_t v = fmt == FMT_W ? (int64_t)(int32_t)fpu_read(cpu, FMT_W, I_FS(i))
				 : (int64_t)fpu_read(cpu, FMT_L, I_FS(i));

	if ((funct != 0x20 && funct != 0x21) ||
	    (fmt == FMT_L && (v >= (1ll << 55) || v < -(1ll << 55)))) {
		fpu_finish(cpu, FPE_E);
		return;
	}
	/* (double)v redondea al más cercano; |v| < 2^55 cabe en int64 de vuelta. */
	double r = (double)v;
	int64_t back = (int64_t)r;
	int dir = v > back ? 1 : v < back ? -1 : 0;
	fpu_result(cpu, funct == 0x20 ? FMT_S : FMT_D, I_FD(i), r, dir, 0, false);
}

/* ---- COP2 ------------------------------------------------------------------ */

/*
 * El VR4300 no tiene COP2, pero con Status.CU2 = 1 MFC2, DMFC2, CFC2, MTC2,
 * DMTC2 y CTC2 usan un único latch de 64 bits (nemu64-test, cop_unusable).
 * Lo demás da Reserved Instruction con Cause.CE = 2.
 */
void salsa64_vr4300_cop2(struct salsa64_cpu *cpu, uint32_t i)
{
	if (!(cpu->status & SALSA64_ST_CU2)) {
		salsa64_vr4300_exception(cpu, SALSA64_EXC_CPU, 2);
		return;
	}
	unsigned rt = I_RT(i);
	switch (I_RS(i)) {
	case 0x00: case 0x02: vr4300_set_gpr(cpu, rt, salsa64_sx32((uint32_t)cpu->cop2_latch)); return; /* MFC2, CFC2 */
	case 0x01:            vr4300_set_gpr(cpu, rt, cpu->cop2_latch); return;                /* DMFC2 */
	case 0x04: case 0x05: case 0x06: cpu->cop2_latch = cpu->gpr[rt]; return;               /* MTC2, DMTC2, CTC2 */
	default:
		salsa64_vr4300_exception(cpu, SALSA64_EXC_RI, 2);
		return;
	}
}

/* ---- Decodificación ----------------------------------------------------------- */

void salsa64_vr4300_cop1(struct salsa64_cpu *cpu, uint32_t i)
{
	if (!(cpu->status & SALSA64_ST_CU1)) {
		salsa64_vr4300_exception(cpu, SALSA64_EXC_CPU, 1);
		return;
	}

	unsigned rt = I_RT(i), fs = I_FS(i);
	switch (I_FMT(i)) {
	case 0x00:                                                        /* MFC1 */
		vr4300_set_gpr(cpu, rt, salsa64_sx32(vr4300_fpr_get32(cpu, fs)));
		return;
	case 0x01:                                                        /* DMFC1 */
		if (!cpu->allow64)
			break;
		vr4300_set_gpr(cpu, rt, vr4300_fpr_get64(cpu, fs));
		return;
	case 0x02:                                                        /* CFC1 */
		vr4300_set_gpr(cpu, rt, salsa64_sx32(fs == 0 ? SALSA64_FCR0_VALUE : fs == 31 ? cpu->fcr31 : 0));
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
				salsa64_vr4300_exception(cpu, SALSA64_EXC_FPE, 0);
		}
		return;
	case 0x08:  /* BC1F/T/FL/TL los resuelve interp.c; las demás variantes no existen */
		fpu_finish(cpu, FPE_E);
		return;
	case FMT_S:
	case FMT_D:
		fpu_float(cpu, i, I_FMT(i));
		return;
	case FMT_W:
	case FMT_L:
		fpu_int(cpu, i, I_FMT(i));
		return;
	default:  /* DCFC1, DCTC1 y formatos inexistentes */
		fpu_finish(cpu, FPE_E);
		return;
	}
	vr4300_reserved(cpu);
}
