/*
 * Salsa64 — pruebas de COP1 (FPU): movimientos, modo FR, aritmética,
 * causas y banderas de FCR31, conversiones, comparaciones y BC1.
 */
#include <string.h>

#include "cpu/fpu.h"
#include "cpu/rec.h"
#include "test.h"

enum { CI = 0x01, CU = 0x02, CO = 0x04, CZ = 0x08, CV = 0x10, CE = 0x20 };

#define CAUSE() ((CPU->fcr31 >> 12) & 0x3F)
#define FLAGS() ((CPU->fcr31 >> 2) & 0x1F)
#define S(r)    ((uint32_t)CPU->fpr[r])
#define D(r)    (CPU->fpr[r])

static void set_s(unsigned r, float f) { CPU->fpr[r] = t_fbits(f); }
static void set_d(unsigned r, double d) { CPU->fpr[r] = t_dbits(d); }

/* Reintenta la instrucción en T_CODE con la máquina sin EXL. */
static void rerun(void)
{
	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(1);
}

static void cop1_unusable(void)
{
	t_set_status(T_STATUS & ~SALSA64_ST_CU1);
	R[T0] = T_DATA;
	T_LOAD(ADD_F(F_S, 0, 1, 2), LWC1(1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_CPU, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 1);
	t_set_status(T_STATUS & ~SALSA64_ST_CU1);
	salsa64_cpu_set_pc(CPU, T_CODE + 4);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_CPU, T_CODE + 4, 0, T_VEC_GEN);
}

static void mtc1_mfc1(void)
{
	R[T0] = 0x80000000;
	R[T2] = 0x0123456789ABCDEFull;
	T_LOAD(MTC1(T0, 5), MFC1(T1, 5), DMTC1(T2, 6), DMFC1(T3, 6), MFC1(T4, 6));
	t_step(5);
	CHECK_EQ(S(5), 0x80000000);
	CHECK_EQ(R[T1], 0xFFFFFFFF80000000ull);
	CHECK_EQ(R[T3], 0x0123456789ABCDEFull);
	CHECK_EQ(R[T4], 0xFFFFFFFF89ABCDEFull);
}

static void fr0_register_pairs(void)
{
	t_set_status(T_STATUS & ~SALSA64_ST_FR);
	R[T0] = 0x11111111;
	R[T1] = 0x22222222;
	T_LOAD(MTC1(T0, 0), MTC1(T1, 1), DMFC1(T2, 0), MFC1(T3, 1), DMFC1(T4, 1));
	t_step(5);
	CHECK_EQ(R[T2], 0x2222222211111111ull);
	CHECK_EQ(R[T3], 0x22222222);
	CHECK_EQ(R[T4], 0x2222222211111111ull);  /* el bit bajo se ignora */
	CHECK_EQ(CPU->fpr[1], 0);
}

static void fr1_independent_registers(void)
{
	R[T0] = 0x11111111;
	R[T1] = 0x22222222;
	T_LOAD(MTC1(T0, 0), MTC1(T1, 1), DMFC1(T2, 0), DMFC1(T3, 1));
	t_step(4);
	CHECK_EQ(R[T2], 0x11111111);
	CHECK_EQ(R[T3], 0x22222222);
}

static void fr0_double_arith(void)
{
	t_set_status(T_STATUS & ~SALSA64_ST_FR);
	t_wr64(T_DATA_PA, t_dbits(1.5));
	t_wr64(T_DATA_PA + 8, t_dbits(2.25));
	R[T0] = T_DATA;
	T_LOAD(LDC1(2, 0, T0), LDC1(4, 8, T0), ADD_F(F_D, 6, 2, 4), SDC1(6, 16, T0),
	       MFC1(T1, 7), MFC1(T2, 6));
	t_step(6);
	CHECK_EQ(t_rd64(T_DATA_PA + 16), t_dbits(3.75));
	CHECK_EQ(R[T1], 0x400E0000);  /* mitad alta de 3.75 por el registro impar */
	CHECK_EQ(R[T2], 0);
}

static void arith_single(void)
{
	set_s(1, 1.5f);
	set_s(2, 2.25f);
	T_LOAD(ADD_F(F_S, 3, 1, 2), SUB_F(F_S, 4, 1, 2), MUL_F(F_S, 5, 1, 2),
	       DIV_F(F_S, 6, 1, 2), SQRT_F(F_S, 7, 2));
	t_step(5);
	CHECK_EQ(S(3), t_fbits(3.75f));
	CHECK_EQ(S(4), t_fbits(-0.75f));
	CHECK_EQ(S(5), t_fbits(3.375f));
	CHECK_EQ(S(6), t_fbits(1.5f / 2.25f));
	CHECK_EQ(S(7), t_fbits(1.5f));
}

static void arith_double(void)
{
	set_d(1, 2.0);
	set_d(2, 3.0);
	T_LOAD(ADD_F(F_D, 3, 1, 2), SUB_F(F_D, 4, 1, 2), MUL_F(F_D, 5, 1, 2),
	       DIV_F(F_D, 6, 1, 2), SQRT_F(F_D, 7, 1));
	t_step(5);
	CHECK_EQ(D(3), t_dbits(5.0));
	CHECK_EQ(D(4), t_dbits(-1.0));
	CHECK_EQ(D(5), t_dbits(6.0));
	CHECK_EQ(D(6), t_dbits(2.0 / 3.0));
	CHECK_EQ(D(7), 0x3FF6A09E667F3BCDull);  /* sqrt(2) */
	CHECK_EQ(CAUSE() & ~CI, 0);
}

static void abs_neg_mov(void)
{
	set_s(1, -0.0f);
	set_d(2, 1.0);
	T_LOAD(ABS_F(F_S, 3, 1), NEG_F(F_D, 4, 2), MOV_F(F_D, 5, 4), NEG_F(F_S, 6, 1));
	t_step(4);
	CHECK_EQ(S(3), 0);
	CHECK_EQ(D(4), t_dbits(-1.0));
	CHECK_EQ(D(5), t_dbits(-1.0));
	CHECK_EQ(S(6), 0);
}

static void div_by_zero_flag(void)
{
	set_s(1, 1.0f);
	set_s(2, 0.0f);
	set_s(3, -2.0f);
	T_LOAD(DIV_F(F_S, 4, 1, 2), DIV_F(F_S, 5, 3, 2));
	t_step(2);
	CHECK_EQ(S(4), 0x7F800000);
	CHECK_EQ(S(5), 0xFF800000);
	CHECK_EQ(CAUSE(), CZ);
	CHECK_EQ(FLAGS(), CZ);
}

static void div_by_zero_trap(void)
{
	CPU->fcr31 = CZ << 7;  /* habilitar Z */
	set_s(1, 1.0f);
	set_s(2, 0.0f);
	CPU->fpr[4] = 0x12345678;
	T_LOAD(DIV_F(F_S, 4, 1, 2));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(S(4), 0x12345678);
	CHECK_EQ(CAUSE(), CZ);
	CHECK_EQ(FLAGS(), 0);
}

static void invalid_gives_default_nan(void)
{
	set_s(1, 0.0f);
	set_d(2, -1.0);
	T_LOAD(DIV_F(F_S, 4, 1, 1), SQRT_F(F_D, 5, 2));
	t_step(1);
	CHECK_EQ(S(4), 0x7FBFFFFF);
	CHECK_EQ(CAUSE(), CV);
	t_step(1);
	CHECK_EQ(D(5), 0x7FF7FFFFFFFFFFFFull);
	CHECK_EQ(CAUSE(), CV);
	CHECK_EQ(FLAGS(), CV);
}

/* NaN de entrada con el bit alto de la mantisa en 1 (qNaN de IEEE 754-2008):
 * Invalid y el NaN por omisión (nemu64-test). */
static void qnan_input_is_invalid(void)
{
	CPU->fpr[1] = 0x7FC00000;
	set_s(2, 1.0f);
	T_LOAD(ADD_F(F_S, 3, 1, 2));
	t_step(1);
	CHECK_EQ(S(3), 0x7FBFFFFF);
	CHECK_EQ(CAUSE(), CV);
}

/* Con ese bit en 0 (sNaN de IEEE; también el NaN por omisión de MIPS):
 * Unimplemented. */
static void snan_input_is_unimplemented(void)
{
	CPU->fpr[1] = 0x7FBFFFFF;
	set_s(2, 1.0f);
	T_LOAD(ADD_F(F_S, 3, 1, 2));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);
}

static void denormal_input_is_unimplemented(void)
{
	CPU->fpr[1] = 1;  /* desnormal */
	set_s(2, 1.0f);
	CPU->fpr[3] = 0x55;
	T_LOAD(ADD_F(F_S, 3, 1, 2));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);
	CHECK_EQ(S(3), 0x55);
}

static void overflow_sets_o_and_i(void)
{
	set_s(1, 1e30f);
	set_d(3, 1e300);
	T_LOAD(MUL_F(F_S, 2, 1, 1), MUL_F(F_D, 4, 3, 3));
	t_step(1);
	CHECK_EQ(S(2), 0x7F800000);
	CHECK_EQ(CAUSE(), CO | CI);
	t_step(1);
	CHECK_EQ(D(4), 0x7FF0000000000000ull);
	CHECK_EQ(FLAGS(), CO | CI);
}

static void infinity_arith_is_exact(void)
{
	CPU->fpr[1] = 0x7F800000;
	set_s(2, 1.0f);
	T_LOAD(ADD_F(F_S, 3, 1, 2));
	t_step(1);
	CHECK_EQ(S(3), 0x7F800000);
	CHECK_EQ(CAUSE(), 0);
}

static void tiny_result_traps_without_fs(void)
{
	set_s(1, 1e-30f);
	T_LOAD(MUL_F(F_S, 2, 1, 1));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);
}

static void tiny_result_flushes_with_fs(void)
{
	CPU->fcr31 = SALSA64_FCR31_FS;
	set_s(1, -1e-30f);
	set_s(3, 1e-20f);
	T_LOAD(MUL_F(F_S, 2, 1, 3));
	t_step(1);
	CHECK_EQ(S(2), 0x80000000);
	CHECK_EQ(CAUSE(), CU | CI);
	CHECK_EQ(FLAGS(), CU | CI);
}

static void round_to_integer(void)
{
	set_s(1, 2.5f);
	set_s(2, -2.5f);
	set_s(3, -1.7f);
	set_d(4, 3.5);
	set_d(5, -3.5);
	T_LOAD(ROUND_W(F_S, 10, 1), ROUND_W(F_S, 11, 2), TRUNC_W(F_S, 12, 3), CEIL_W(F_S, 13, 3),
	       FLOOR_W(F_S, 14, 3), ROUND_W(F_D, 15, 4), ROUND_L(F_D, 16, 5), TRUNC_L(F_D, 17, 5));
	t_step(8);
	CHECK_EQ(S(10), 2);
	CHECK_EQ(S(11), (uint32_t)-2);
	CHECK_EQ(S(12), (uint32_t)-1);
	CHECK_EQ(S(13), (uint32_t)-1);
	CHECK_EQ(S(14), (uint32_t)-2);
	CHECK_EQ(S(15), 4);
	CHECK_EQ(D(16), (uint64_t)-4);
	CHECK_EQ(D(17), (uint64_t)-3);
	CHECK_EQ(FLAGS(), CI);
}

static void cvt_w_uses_rounding_mode(void)
{
	static const int32_t pos[4] = { 2, 2, 3, 2 }, neg[4] = { -2, -2, -2, -3 };
	T_LOAD(CVT_W(F_S, 2, 1));
	for (unsigned m = 0; m < 4; m++) {
		CPU->fcr31 = m;
		set_s(1, 2.5f);
		rerun();
		CHECK_EQ(S(2), (uint32_t)pos[m]);
		set_s(1, -2.5f);
		rerun();
		CHECK_EQ(S(2), (uint32_t)neg[m]);
	}
}

static void cvt_to_int_unimplemented(void)
{
	CPU->fpr[1] = 0x7FBFFFFF;
	T_LOAD(CVT_W(F_S, 3, 1));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);

	set_s(1, 3e9f);  /* fuera del rango de 32 bits */
	rerun();
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_FPE);
	CHECK_EQ(CAUSE(), CE);
}

static void cvt_l_wide_range(void)
{
	set_s(1, 3e9f);
	T_LOAD(CVT_L(F_S, 2, 1));
	t_step(1);
	CHECK_EQ(D(2), 3000000000ull);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
}

static void cvt_between_formats(void)
{
	CPU->fpr[1] = (uint32_t)-7;
	CPU->fpr[2] = 1ull << 40;
	set_d(3, 0.1);
	set_s(4, 0.5f);
	T_LOAD(CVT_S(F_W, 5, 1), CVT_D(F_W, 6, 1), CVT_D(F_L, 7, 2), CVT_S(F_D, 8, 3),
	       CVT_D(F_S, 9, 4), CVT_S(F_L, 10, 2));
	t_step(6);
	CHECK_EQ(S(5), t_fbits(-7.0f));
	CHECK_EQ(D(6), t_dbits(-7.0));
	CHECK_EQ(D(7), t_dbits(1099511627776.0));
	CHECK_EQ(S(8), t_fbits((float)0.1));
	CHECK_EQ(D(9), t_dbits(0.5));
	CHECK_EQ(S(10), t_fbits(1099511627776.0f));
}

static void cvt_s_d_overflow(void)
{
	set_d(1, 1e300);
	T_LOAD(CVT_S(F_D, 2, 1));
	t_step(1);
	CHECK_EQ(S(2), 0x7F800000);
	CHECK_EQ(CAUSE(), CO | CI);
}

static void cvt_from_long_out_of_range(void)
{
	CPU->fpr[1] = 1ull << 55;
	T_LOAD(CVT_D(F_L, 2, 1));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);
}

static void unimplemented_encodings(void)
{
	T_LOAD(CVT_S(F_S, 2, 1));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);

	t_wr32(T_CODE_PA, ADD_F(F_W, 3, 1, 2));
	rerun();
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_FPE);

	t_wr32(T_CODE_PA, M_R(0x11, 0x03, 0, 0, 0, 0));  /* DCFC1: tampoco es RI */
	rerun();
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_FPE);
	CHECK_EQ(CAUSE(), CE);
}

static void compare_and_branch(void)
{
	set_s(1, 1.0f);
	set_s(2, 2.0f);
	T_LOAD(C_F(F_S, C_LT, 1, 2), BC1T(2), ADDIU(T0, ZERO, 1), ADDIU(T1, ZERO, 1),
	       ADDIU(T2, ZERO, 1));
	t_step(4);
	CHECK(CPU->fcr31 & SALSA64_FCR31_C);
	CHECK_EQ(R[T0], 1);
	CHECK_EQ(R[T1], 0);
	CHECK_EQ(R[T2], 1);
}

static void compare_false_and_likely(void)
{
	set_s(1, 1.0f);
	set_s(2, 2.0f);
	T_LOAD(C_F(F_S, C_EQ, 1, 2), BC1TL(5), ADDIU(T0, ZERO, 1), BC1FL(2), ADDIU(T1, ZERO, 1),
	       ADDIU(T2, ZERO, 1), ADDIU(T3, ZERO, 1));
	t_step(5);
	CHECK_EQ(CPU->fcr31 & SALSA64_FCR31_C, 0);
	CHECK_EQ(R[T0], 0);  /* anulado */
	CHECK_EQ(R[T1], 1);
	CHECK_EQ(R[T2], 0);
	CHECK_EQ(R[T3], 1);
}

static void compare_double(void)
{
	set_d(1, 2.0);
	set_d(2, 2.0);
	T_LOAD(C_F(F_D, C_LE, 1, 2), CFC1(T0, 31), C_F(F_D, C_LT, 1, 2), CFC1(T1, 31));
	t_step(4);
	CHECK(R[T0] & SALSA64_FCR31_C);
	CHECK_EQ(R[T1] & SALSA64_FCR31_C, 0);
}

static void compare_unordered(void)
{
	CPU->fpr[1] = 0x7FBFFFFF;
	set_s(2, 1.0f);
	T_LOAD(C_F(F_S, C_UN, 1, 2), CFC1(T0, 31), C_F(F_S, C_EQ, 1, 2), CFC1(T1, 31),
	       C_F(F_S, C_ULE, 1, 2), CFC1(T2, 31), C_F(F_S, C_SEQ, 1, 2), CFC1(T3, 31));
	t_step(8);
	CHECK(R[T0] & SALSA64_FCR31_C);
	CHECK_EQ((R[T0] >> 12) & 0x3F, 0);
	CHECK_EQ(R[T1] & SALSA64_FCR31_C, 0);
	CHECK(R[T2] & SALSA64_FCR31_C);
	CHECK_EQ(R[T3] & SALSA64_FCR31_C, 0);
	CHECK_EQ((R[T3] >> 12) & 0x3F, CV);  /* comparación con señal y NaN */
	CHECK_EQ(FLAGS(), CV);
}

static void signaling_compare_traps_when_enabled(void)
{
	CPU->fcr31 = CV << 7;
	CPU->fpr[1] = 0x7FBFFFFF;
	T_LOAD(C_F(F_S, C_SEQ, 1, 1));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
}

static void ctc1_cfc1(void)
{
	R[T0] = 0x01000003;
	R[T3] = 0xFE800FFF;
	T_LOAD(CTC1(T0, 31), CFC1(T1, 31), CFC1(T2, 0), CTC1(T3, 31), CFC1(T4, 31));
	t_step(5);
	CHECK_EQ(R[T1], 0x01000003);
	CHECK_EQ(R[T2], SALSA64_FCR0_VALUE);
	CHECK_EQ(R[T4], 0x00800FFF);  /* máscara de escritura */
}

static void ctc1_raises_pending_cause(void)
{
	R[T0] = 1u << 12 | 1u << 7;  /* causa I con I habilitada */
	R[T1] = 1u << 17;            /* causa E */
	T_LOAD(CTC1(T0, 31), CTC1(T1, 31));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CPU->fcr31, 1u << 12 | 1u << 7);
	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE + 4);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_FPE, T_CODE + 4, 0, T_VEC_GEN);
}

static void load_store_fpr(void)
{
	R[T0] = T_DATA;
	set_s(1, 3.0f);
	set_d(2, -8.5);
	T_LOAD(SWC1(1, 0, T0), SDC1(2, 8, T0), LWC1(3, 0, T0), LDC1(4, 8, T0));
	t_step(4);
	CHECK_EQ(t_rd32(T_DATA_PA), t_fbits(3.0f));
	CHECK_EQ(t_rd64(T_DATA_PA + 8), t_dbits(-8.5));
	CHECK_EQ(S(3), t_fbits(3.0f));
	CHECK_EQ(D(4), t_dbits(-8.5));
}

/* Redondeo dirigido e Inexact: 1 + 2^-30 no cabe en float. */
static void directed_rounding_and_inexact(void)
{
	static const struct { unsigned mode; uint32_t bits; } cases[] = {
		{ 0, 0x3F800000 },  /* más cercano: 1.0 */
		{ 1, 0x3F800000 },  /* a cero */
		{ 2, 0x3F800001 },  /* a +inf: el float siguiente */
		{ 3, 0x3F800000 },  /* a -inf */
	};
	for (unsigned k = 0; k < 4; k++) {
		set_s(1, 1.0f);
		set_s(2, 0x1p-30f);
		CPU->fcr31 = cases[k].mode;
		T_LOAD(ADD_F(F_S, 3, 1, 2));
		rerun();
		CHECK_EQ(S(3), cases[k].bits);
		CHECK_EQ(CAUSE(), CI);
		CHECK_EQ(FLAGS(), CI);
	}
	/* Exacto: sin I. Resultado S: los 32 bits altos de fd quedan en 0. */
	set_s(1, 1.5f);
	set_s(2, 0.25f);
	CPU->fpr[3] = 0x1234567800000000ull;
	CPU->fcr31 = 0;
	rerun();
	CHECK_EQ(D(3), t_fbits(1.75f));
	CHECK_EQ(CAUSE(), 0);

	/* D: 1/3 al más cercano redondea hacia abajo; hacia +inf da 1 ulp más y
	 * hacia -inf lo mismo. */
	set_d(1, 1.0);
	set_d(2, 3.0);
	t_wr32(T_CODE_PA, DIV_F(F_D, 3, 1, 2));
	CPU->fcr31 = 2;
	rerun();
	CHECK_EQ(D(3), t_dbits(1.0 / 3.0) + 1);
	CHECK_EQ(CAUSE(), CI);
	CPU->fcr31 = 3;
	rerun();
	CHECK_EQ(D(3), t_dbits(1.0 / 3.0));
}

/* Desbordamiento: ±inf o ±máximo según el modo, con O e I. */
static void overflow_by_mode(void)
{
	set_s(1, 3.0e38f);
	set_s(2, 3.0e38f);
	T_LOAD(ADD_F(F_S, 3, 1, 2));
	CPU->fcr31 = 0;
	rerun();
	CHECK_EQ(S(3), 0x7F800000);
	CHECK_EQ(CAUSE(), CO | CI);
	CPU->fcr31 = 1;
	rerun();
	CHECK_EQ(S(3), 0x7F7FFFFF);
}

/* Resultado diminuto con FS = 1 y U/I deshabilitadas: se aplana; hacia +inf,
 * al mínimo normal. Con U habilitada, E (nemu64-test). */
static void flush_tiny_results(void)
{
	set_d(1, 0x1p-1022);
	set_d(2, 0.5);
	T_LOAD(MUL_F(F_D, 3, 1, 2));
	CPU->fcr31 = SALSA64_FCR31_FS | 2;
	rerun();
	CHECK_EQ(D(3), 0x0010000000000000ull);
	CHECK_EQ(CAUSE(), CU | CI);
	CPU->fcr31 = SALSA64_FCR31_FS;
	rerun();
	CHECK_EQ(D(3), 0);
	CPU->fcr31 = SALSA64_FCR31_FS | (CU << 7);
	rerun();
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_FPE);
	CHECK_EQ(CAUSE(), CE);
}

/* ---- Camino rápido (vr4300_cop1_fast) contra el completo ---------- */

static uint64_t fz_state = 0x9E3779B97F4A7C15ull;

static uint64_t fz_rand(void)  /* xorshift64*: misma secuencia en todas las plataformas */
{
	fz_state ^= fz_state >> 12;
	fz_state ^= fz_state << 25;
	fz_state ^= fz_state >> 27;
	return fz_state * 0x2545F4914F6CDD1Dull;
}

/* Operando: casi siempre normal con exponente en una franja (centro, bordes
 * de la franja segura del camino rápido, cerca de desbordamiento y de
 * subdesbordamiento); a veces un valor especial o bits al azar (NaN,
 * infinito, desnormal). */
static uint64_t fz_operand(bool is_s)
{
	static const uint32_t special_s[] = {
		0, 0x80000000, 0x00800000, 0x7F7FFFFF, 0x7F800000, 0xFF800000,
		0x7FC00000, 0x7FBFFFFF, 0x00000001, 0x3F800000, 0xBF800000,
	};
	static const uint64_t special_d[] = {
		0, 0x8000000000000000ull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull,
		0x7FF0000000000000ull, 0x7FF8000000000000ull, 0x7FF7FFFFFFFFFFFFull,
		0x0000000000000001ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
	};
	/* Franjas de exponentes sesgados: {mínimo, ancho}. */
	static const unsigned bands_s[][2] = {
		{ 107, 41 }, { 107, 41 }, { 107, 41 }, { 107, 41 }, { 107, 41 },
		{ 1, 40 }, { 214, 41 }, { 50, 40 }, { 165, 40 },     /* 67 y 187: bordes de la franja segura */
		{ 1, 40 }, { 214, 41 }, { 50, 40 }, { 165, 40 }, { 107, 41 },
	};
	static const unsigned bands_d[][2] = {
		{ 1003, 41 }, { 1003, 41 }, { 1003, 41 }, { 1003, 41 }, { 1003, 41 },
		{ 1, 60 }, { 100, 150 }, { 1987, 60 }, { 560, 50 }, { 1440, 50 }, /* 583 y 1463: bordes */
		{ 1, 60 }, { 100, 150 }, { 560, 50 }, { 1440, 50 },
	};
	uint64_t x = fz_rand(), sign = x >> 63, m = fz_rand();
	unsigned kind = x & 15;

	if (kind == 0)
		return is_s ? (uint32_t)m : m;
	if (kind == 1)
		return is_s ? special_s[m % (sizeof special_s / sizeof special_s[0])]
			    : special_d[m % (sizeof special_d / sizeof special_d[0])];
	if (is_s) {
		const unsigned *b = bands_s[kind - 2];
		unsigned e = b[0] + (unsigned)(m % b[1]);
		return sign << 31 | (uint64_t)e << 23 | (x >> 8 & 0x7FFFFF);
	}
	const unsigned *b = bands_d[kind - 2];
	unsigned e = b[0] + (unsigned)(m % b[1]);
	return sign << 63 | (uint64_t)e << 52 | (x >> 4 & 0xFFFFFFFFFFFFFull);
}

static struct salsa64_cpu fz_full, fz_fast;

static void fast_path_matches_full(void)
{
	static const unsigned functs[] = {
		0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07,
		0x30, 0x32, 0x36, 0x3A, 0x3C, 0x3E,
	};
	static const unsigned moves[] = { 0x00, 0x01, 0x02, 0x04, 0x05 };  /* MFC1 DMFC1 CFC1 MTC1 DMTC1 */
	unsigned hits = 0, failed = 0;

	for (unsigned n = 0; n < 30000; n++) {
		bool is_s = fz_rand() & 1, fr = (fz_rand() & 3) != 0, move = (fz_rand() & 7) == 0;
		unsigned funct = functs[fz_rand() % (sizeof functs / sizeof functs[0])];
		unsigned fs = fz_rand() & 31, ft = fz_rand() & 31, fd = fz_rand() & 31;
		uint64_t r = fz_rand();
		/* Redondeo al más cercano la mitad de las veces; habilitaciones y FS
		 * de vez en cuando; banderas, causas y C al azar. */
		uint32_t fcr31 = ((r & 1) ? (uint32_t)(r >> 1) & 3 : 0) |
				 ((uint32_t)(r >> 8) & 0x1F) << 2 |
				 ((r >> 16 & 7) == 0 ? ((uint32_t)(r >> 20) & 0x1F) << 7 : 0) |
				 ((r >> 26 & 3) == 0 ? ((uint32_t)(r >> 28) & 0x1F) << 12 : 0) |
				 ((r >> 32 & 3) == 0 ? SALSA64_FCR31_FS : 0) |
				 ((r >> 40 & 1) ? SALSA64_FCR31_C : 0);

		t_set_status(fr ? T_STATUS : T_STATUS & ~SALSA64_ST_FR);
		for (unsigned k = 0; k < 32; k++) {
			CPU->fpr[k] = fz_rand();
			R[k] = k ? fz_rand() : 0;
		}
		unsigned rs = fr ? fs : fs & ~1u;
		uint64_t a = fz_operand(is_s), b = fz_operand(is_s);
		CPU->fpr[rs] = is_s ? (CPU->fpr[rs] & 0xFFFFFFFF00000000ull) | a : a;
		CPU->fpr[ft] = is_s ? (CPU->fpr[ft] & 0xFFFFFFFF00000000ull) | b : b;
		CPU->fcr31 = fcr31;
		uint32_t instr = move ? M_R(0x11, moves[fz_rand() % 5], ft, (fz_rand() & 1) ? fs : 31 * (fs & 1), 0, 0)
				      : FOP(is_s ? F_S : F_D, funct, fd, fs, ft);

		memcpy(&fz_full, CPU, sizeof fz_full);
		memcpy(&fz_fast, CPU, sizeof fz_fast);
		salsa64_vr4300_cop1(&fz_full, instr);
		if (vr4300_cop1_fast(&fz_fast, instr))
			hits++;
		else
			salsa64_vr4300_cop1(&fz_fast, instr);

		t_checks++;
		if (memcmp(fz_full.fpr, fz_fast.fpr, sizeof fz_full.fpr) != 0 ||
		    memcmp(fz_full.gpr, fz_fast.gpr, sizeof fz_full.gpr) != 0 ||
		    fz_full.fcr31 != fz_fast.fcr31 || fz_full.cause != fz_fast.cause ||
		    fz_full.status != fz_fast.status || fz_full.pc != fz_fast.pc) {
			if (failed++ < 5)
				t_fail(__FILE__, __LINE__,
				       "instr 0x%08x, fs 0x%016llx, ft 0x%016llx, FCR31 0x%08x: "
				       "fd 0x%016llx/0x%016llx, FCR31 0x%08x/0x%08x (completo/rápido)",
				       instr, (unsigned long long)a, (unsigned long long)b, fcr31,
				       (unsigned long long)fz_full.fpr[fd], (unsigned long long)fz_fast.fpr[fd],
				       fz_full.fcr31, fz_fast.fcr31);
		}
	}
	/* Que el camino rápido de verdad se haya usado en buena parte. */
	CHECK(hits > 8000);
}

/*
 * Lo mismo, pero ejecutando la instrucción con el CPU (t_step): con el
 * intérprete prueba su camino rápido y, con el recompilador (modo
 * estricto), el código generado. La referencia es el camino completo
 * llamado directo.
 */
static void executed_matches_full(void)
{
	static const unsigned functs[] = {
		0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07,
		0x30, 0x32, 0x36, 0x3A, 0x3C, 0x3E,
	};
	unsigned failed = 0;

	for (unsigned n = 0; n < 6000; n++) {
		bool is_s = fz_rand() & 1, fr = (fz_rand() & 3) != 0;
		unsigned funct = functs[fz_rand() % (sizeof functs / sizeof functs[0])];
		unsigned fs = fz_rand() & 31, ft = fz_rand() & 31, fd = fz_rand() & 31;
		uint64_t r = fz_rand();
		uint32_t fcr31 = ((r & 1) ? (uint32_t)(r >> 1) & 3 : 0) |
				 ((uint32_t)(r >> 8) & 0x1F) << 2 |
				 ((r >> 16 & 7) == 0 ? ((uint32_t)(r >> 20) & 0x1F) << 7 : 0) |
				 ((r >> 26 & 3) == 0 ? ((uint32_t)(r >> 28) & 0x1F) << 12 : 0) |
				 ((r >> 32 & 3) == 0 ? SALSA64_FCR31_FS : 0) |
				 ((r >> 40 & 1) ? SALSA64_FCR31_C : 0);

		t_set_status(fr ? T_STATUS : T_STATUS & ~SALSA64_ST_FR);
		for (unsigned k = 0; k < 32; k++)
			CPU->fpr[k] = fz_rand();
		unsigned rs = fr ? fs : fs & ~1u;
		uint64_t a = fz_operand(is_s), b = fz_operand(is_s);
		CPU->fpr[rs] = is_s ? (CPU->fpr[rs] & 0xFFFFFFFF00000000ull) | a : a;
		CPU->fpr[ft] = is_s ? (CPU->fpr[ft] & 0xFFFFFFFF00000000ull) | b : b;
		CPU->fcr31 = fcr31;
		uint32_t instr = FOP(is_s ? F_S : F_D, funct, fd, fs, ft);
		t_wr32(T_CODE_PA, instr);
		salsa64_cpu_set_pc(CPU, T_CODE);
		CPU->cur_pc = T_CODE;
		CPU->cause = 0;

		memcpy(&fz_full, CPU, sizeof fz_full);
		fz_full.redirect = 0;  /* set_pc lo deja en 1; aquí marca una excepción */
		salsa64_vr4300_cop1(&fz_full, instr);
		bool exc = fz_full.redirect;
		t_step(1);

		t_checks++;
		if (memcmp(fz_full.fpr, CPU->fpr, sizeof fz_full.fpr) != 0 ||
		    fz_full.fcr31 != CPU->fcr31 || fz_full.cause != CPU->cause ||
		    fz_full.status != CPU->status || CPU->pc != (exc ? fz_full.pc : T_CODE + 4)) {
			if (failed++ < 5)
				t_fail(__FILE__, __LINE__,
				       "instr 0x%08x, fs 0x%016llx, ft 0x%016llx, FCR31 0x%08x: "
				       "fd 0x%016llx/0x%016llx, FCR31 0x%08x/0x%08x (completo/ejecutado)",
				       instr, (unsigned long long)a, (unsigned long long)b, fcr31,
				       (unsigned long long)fz_full.fpr[fd], (unsigned long long)CPU->fpr[fd],
				       fz_full.fcr31, CPU->fcr31);
		}
	}
}

/* Operando de una conversión o de SQRT: los de fz_operand, enteros con
 * fracción (0, 1/4, 1/2, 3/4 o al azar: los empates de ROUND) de hasta 62
 * bits, valores a unas unidades de 2^30, 2^31, 2^52 y 2^53 (donde un modo
 * de redondeo se sale del rango) y los bordes de la franja de CVT.S.D. */
static uint64_t fz_cvt_operand(bool is_s)
{
	static const double edges[] = {
		2147483647.0, 2147483647.5, 2147483648.0, 2147483648.5, -2147483648.0, -2147483648.5,
		-2147483649.0, 1073741823.5, 1073741824.0, -1073741824.5, -1073741825.0,
		4503599627370495.5, -4503599627370495.5, 4503599627370496.0, 9007199254740991.0,
		9007199254740992.0, -9007199254740992.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 1e-30, -1e-30,
		0x1p-126, 0x1.000001p-126, 0x1.fffffffp-127, 0x1p-127, 0x1.fffffep127, 0x1.fffffefp126,
		0x1.ffffffp127, 0x1p127, -0x1.ffffffp126, 0.0, -0.0,
	};
	static const double fracs[] = { 0, 0.25, 0.5, 0.75, 0.5 };
	uint64_t x = fz_rand();
	double v;
	switch (x & 7) {
	case 0: case 1:
		return fz_operand(is_s);
	case 2:
		v = edges[(x >> 3) % (sizeof edges / sizeof edges[0])];
		break;
	case 3: {
		static const unsigned ks[] = { 30, 31, 52, 53 };
		v = (double)(1ull << ks[(x >> 3) % 4]) - (double)((x >> 5) % 4) + fracs[(x >> 7) % 5];
		if (x >> 63)
			v = -v;
		break;
	}
	default: {
		unsigned bits = (unsigned)(x >> 3) % 63, f = (unsigned)(x >> 9) % 6;
		uint64_t m = bits ? fz_rand() >> (64 - bits) : 0;
		v = (double)m + (f < 5 ? fracs[f] : (double)(fz_rand() >> 11) * 0x1p-53);
		if (x >> 63)
			v = -v;
		break;
	}
	}
	if (is_s && !(__builtin_fabs(v) <= 0x1.fffffep127))
		v = 0x1p100;  /* (float) de algo mayor no está definido en C */
	return is_s ? t_fbits((float)v) : t_dbits(v);
}

/* Entero de 32 bits para CVT.S.W y CVT.D.W: al azar (casi siempre con más
 * de 24 bits), chico, bordes y empates de 25 bits (el bit que sobra es justo
 * la mitad). */
static uint32_t fz_int_operand(void)
{
	static const uint32_t edges[] = {
		0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x01000000, 0x01000001, 0x01000003,
		0x00FFFFFF, 0x02000006, 0x7FFFFFC0, 0x7FFFFF80, 0x7FFFFFBF, 0x81000001, 0xFF000001,
	};
	uint64_t x = fz_rand();
	uint32_t w;
	switch (x & 3) {
	case 0:
		return (uint32_t)fz_rand();
	case 1:
		w = (uint32_t)(fz_rand() >> (40 + (x >> 2) % 24));
		break;
	case 2:
		return edges[(x >> 2) % (sizeof edges / sizeof edges[0])];
	default:
		w = (((uint32_t)fz_rand() & 0x00FFFFFF) | 0x01000001u) << ((x >> 2) % 7);
		break;
	}
	return (x >> 63) ? 0u - w : w;
}

/*
 * Conversiones, SQRT, CFC1 y CTC1 ejecutados por el CPU (t_step: el
 * intérprete o, con el recompilador, el código generado) contra el camino
 * completo llamado directo. Como executed_matches_full, más los registros
 * generales.
 */
static void executed_conversions_match_full(void)
{
	unsigned failed = 0, native = 0;

	for (unsigned n = 0; n < 8000; n++) {
		bool is_s = fz_rand() & 1, fr = (fz_rand() & 3) != 0;
		unsigned kind = (unsigned)(fz_rand() % 6);
		unsigned fs = fz_rand() & 31, fd = fz_rand() & 31, rt = fz_rand() & 31;
		uint64_t r = fz_rand();
		uint32_t fcr31 = ((r & 1) ? (uint32_t)(r >> 1) & 3 : 0) |
				 ((uint32_t)(r >> 8) & 0x1F) << 2 |
				 ((r >> 16 & 7) == 0 ? ((uint32_t)(r >> 20) & 0x1F) << 7 : 0) |
				 ((r >> 26 & 3) == 0 ? ((uint32_t)(r >> 28) & 0x1F) << 12 : 0) |
				 ((r >> 32 & 3) == 0 ? SALSA64_FCR31_FS : 0) |
				 ((r >> 40 & 1) ? SALSA64_FCR31_C : 0);

		t_set_status(fr ? T_STATUS : T_STATUS & ~SALSA64_ST_FR);
		for (unsigned k = 0; k < 32; k++) {
			CPU->fpr[k] = fz_rand();
			R[k] = k ? fz_rand() : 0;
		}
		unsigned rs = fr ? fs : fs & ~1u;
		uint32_t instr;
		switch (kind) {
		case 0:  /* SQRT */
			instr = SQRT_F(is_s ? F_S : F_D, fd, fs);
			break;
		case 1:  /* CVT.D.S, CVT.S.D */
			instr = is_s ? CVT_D(F_S, fd, fs) : CVT_S(F_D, fd, fs);
			break;
		case 2:  /* CVT.S.W, CVT.D.W */
			instr = (fz_rand() & 1) ? CVT_S(F_W, fd, fs) : CVT_D(F_W, fd, fs);
			CPU->fpr[rs] = (CPU->fpr[rs] & 0xFFFFFFFF00000000ull) | fz_int_operand();
			break;
		case 3:  /* ROUND, TRUNC, CEIL, FLOOR a L y W */
			instr = FOP(is_s ? F_S : F_D, 0x08 + (unsigned)(fz_rand() % 8), fd, fs, 0);
			break;
		case 4:  /* CVT.W, CVT.L */
			instr = FOP(is_s ? F_S : F_D, (fz_rand() & 1) ? 0x24 : 0x25, fd, fs, 0);
			break;
		default: /* CFC1, CTC1 */
			fs = (fz_rand() & 1) ? 31 : (fz_rand() & 3) ? 0 : fs;
			if (fz_rand() & 1) {
				instr = CFC1(rt, fs);
			} else {
				instr = CTC1(rt, fs);
				if (rt && (fz_rand() & 1))
					R[rt] = (fz_rand() & ~0xFFFFFFFFull) | (fz_rand() & 1 ? fcr31 >> 1 : fcr31);
			}
			break;
		}
		if (kind != 2 && kind != 5) {
			uint64_t a = fz_cvt_operand(is_s);
			CPU->fpr[rs] = is_s ? (CPU->fpr[rs] & 0xFFFFFFFF00000000ull) | a : a;
		}
		CPU->fcr31 = fcr31;
		t_wr32(T_CODE_PA, instr);
		salsa64_cpu_set_pc(CPU, T_CODE);
		CPU->cur_pc = T_CODE;
		CPU->cause = 0;

		memcpy(&fz_full, CPU, sizeof fz_full);
		fz_full.redirect = 0;
		salsa64_vr4300_cop1(&fz_full, instr);
		bool exc = fz_full.redirect;
		uint64_t calls = CPU->rec ? CPU->rec->stats.exec_calls : 0;
		t_step(1);
		native += CPU->rec && CPU->rec->stats.exec_calls == calls;

		t_checks++;
		if (memcmp(fz_full.fpr, CPU->fpr, sizeof fz_full.fpr) != 0 ||
		    memcmp(fz_full.gpr, CPU->gpr, sizeof fz_full.gpr) != 0 ||
		    fz_full.fcr31 != CPU->fcr31 || fz_full.cause != CPU->cause ||
		    fz_full.status != CPU->status || CPU->pc != (exc ? fz_full.pc : T_CODE + 4)) {
			if (failed++ < 8)
				t_fail(__FILE__, __LINE__,
				       "instr 0x%08x, fs 0x%016llx, FCR31 0x%08x: fd 0x%016llx/0x%016llx, "
				       "FCR31 0x%08x/0x%08x, rt 0x%016llx/0x%016llx (completo/ejecutado)",
				       instr, (unsigned long long)fz_full.fpr[rs], fcr31,
				       (unsigned long long)fz_full.fpr[I_FD(instr)], (unsigned long long)CPU->fpr[I_FD(instr)],
				       fz_full.fcr31, CPU->fcr31, (unsigned long long)fz_full.gpr[rt],
				       (unsigned long long)CPU->gpr[rt]);
		}
	}
	/* Con el recompilador, que buena parte haya corrido en nativo. */
	if (CPU->rec)
		CHECK(native > 3000);
}

void t_suite_cop1(void)
{
	RUN(cop1_unusable);
	RUN(mtc1_mfc1);
	RUN(fr0_register_pairs);
	RUN(fr1_independent_registers);
	RUN(fr0_double_arith);
	RUN(arith_single);
	RUN(arith_double);
	RUN(abs_neg_mov);
	RUN(div_by_zero_flag);
	RUN(div_by_zero_trap);
	RUN(invalid_gives_default_nan);
	RUN(qnan_input_is_invalid);
	RUN(snan_input_is_unimplemented);
	RUN(denormal_input_is_unimplemented);
	RUN(overflow_sets_o_and_i);
	RUN(infinity_arith_is_exact);
	RUN(tiny_result_traps_without_fs);
	RUN(tiny_result_flushes_with_fs);
	RUN(round_to_integer);
	RUN(cvt_w_uses_rounding_mode);
	RUN(cvt_to_int_unimplemented);
	RUN(cvt_l_wide_range);
	RUN(cvt_between_formats);
	RUN(cvt_s_d_overflow);
	RUN(cvt_from_long_out_of_range);
	RUN(unimplemented_encodings);
	RUN(compare_and_branch);
	RUN(compare_false_and_likely);
	RUN(compare_double);
	RUN(compare_unordered);
	RUN(signaling_compare_traps_when_enabled);
	RUN(ctc1_cfc1);
	RUN(ctc1_raises_pending_cause);
	RUN(load_store_fpr);
	RUN(directed_rounding_and_inexact);
	RUN(overflow_by_mode);
	RUN(flush_tiny_results);
	RUN(fast_path_matches_full);
	RUN(executed_matches_full);
	RUN(executed_conversions_match_full);
}
