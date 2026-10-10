/*
 * Retro64 — pruebas de COP1 (FPU): movimientos, modo FR, aritmética,
 * causas y banderas de FCR31, conversiones, comparaciones y BC1.
 */
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
	retro64_cpu_set_pc(CPU, T_CODE);
	t_step(1);
}

static void cop1_unusable(void)
{
	t_set_status(T_STATUS & ~RETRO64_ST_CU1);
	R[T0] = T_DATA;
	T_LOAD(ADD_F(F_S, 0, 1, 2), LWC1(1, 0, T0));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_CPU, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> RETRO64_CAUSE_CE_SHIFT) & 3, 1);
	t_set_status(T_STATUS & ~RETRO64_ST_CU1);
	retro64_cpu_set_pc(CPU, T_CODE + 4);
	t_step(1);
	CHECK_EXC(RETRO64_EXC_CPU, T_CODE + 4, 0, T_VEC_GEN);
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
	t_set_status(T_STATUS & ~RETRO64_ST_FR);
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
	t_set_status(T_STATUS & ~RETRO64_ST_FR);
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
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
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
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);
}

static void denormal_input_is_unimplemented(void)
{
	CPU->fpr[1] = 1;  /* desnormal */
	set_s(2, 1.0f);
	CPU->fpr[3] = 0x55;
	T_LOAD(ADD_F(F_S, 3, 1, 2));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
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
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);
}

static void tiny_result_flushes_with_fs(void)
{
	CPU->fcr31 = RETRO64_FCR31_FS;
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
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);

	set_s(1, 3e9f);  /* fuera del rango de 32 bits */
	rerun();
	CHECK_EQ(RETRO64_CAUSE_EXC(CPU->cause), RETRO64_EXC_FPE);
	CHECK_EQ(CAUSE(), CE);
}

static void cvt_l_wide_range(void)
{
	set_s(1, 3e9f);
	T_LOAD(CVT_L(F_S, 2, 1));
	t_step(1);
	CHECK_EQ(D(2), 3000000000ull);
	CHECK_EQ(CPU->status & RETRO64_ST_EXL, 0);
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
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);
}

static void unimplemented_encodings(void)
{
	T_LOAD(CVT_S(F_S, 2, 1));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CAUSE(), CE);

	t_wr32(T_CODE_PA, ADD_F(F_W, 3, 1, 2));
	rerun();
	CHECK_EQ(RETRO64_CAUSE_EXC(CPU->cause), RETRO64_EXC_FPE);

	t_wr32(T_CODE_PA, M_R(0x11, 0x03, 0, 0, 0, 0));  /* DCFC1: tampoco es RI */
	rerun();
	CHECK_EQ(RETRO64_CAUSE_EXC(CPU->cause), RETRO64_EXC_FPE);
	CHECK_EQ(CAUSE(), CE);
}

static void compare_and_branch(void)
{
	set_s(1, 1.0f);
	set_s(2, 2.0f);
	T_LOAD(C_F(F_S, C_LT, 1, 2), BC1T(2), ADDIU(T0, ZERO, 1), ADDIU(T1, ZERO, 1),
	       ADDIU(T2, ZERO, 1));
	t_step(4);
	CHECK(CPU->fcr31 & RETRO64_FCR31_C);
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
	CHECK_EQ(CPU->fcr31 & RETRO64_FCR31_C, 0);
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
	CHECK(R[T0] & RETRO64_FCR31_C);
	CHECK_EQ(R[T1] & RETRO64_FCR31_C, 0);
}

static void compare_unordered(void)
{
	CPU->fpr[1] = 0x7FBFFFFF;
	set_s(2, 1.0f);
	T_LOAD(C_F(F_S, C_UN, 1, 2), CFC1(T0, 31), C_F(F_S, C_EQ, 1, 2), CFC1(T1, 31),
	       C_F(F_S, C_ULE, 1, 2), CFC1(T2, 31), C_F(F_S, C_SEQ, 1, 2), CFC1(T3, 31));
	t_step(8);
	CHECK(R[T0] & RETRO64_FCR31_C);
	CHECK_EQ((R[T0] >> 12) & 0x3F, 0);
	CHECK_EQ(R[T1] & RETRO64_FCR31_C, 0);
	CHECK(R[T2] & RETRO64_FCR31_C);
	CHECK_EQ(R[T3] & RETRO64_FCR31_C, 0);
	CHECK_EQ((R[T3] >> 12) & 0x3F, CV);  /* comparación con señal y NaN */
	CHECK_EQ(FLAGS(), CV);
}

static void signaling_compare_traps_when_enabled(void)
{
	CPU->fcr31 = CV << 7;
	CPU->fpr[1] = 0x7FBFFFFF;
	T_LOAD(C_F(F_S, C_SEQ, 1, 1));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
}

static void ctc1_cfc1(void)
{
	R[T0] = 0x01000003;
	R[T3] = 0xFE800FFF;
	T_LOAD(CTC1(T0, 31), CFC1(T1, 31), CFC1(T2, 0), CTC1(T3, 31), CFC1(T4, 31));
	t_step(5);
	CHECK_EQ(R[T1], 0x01000003);
	CHECK_EQ(R[T2], RETRO64_FCR0_VALUE);
	CHECK_EQ(R[T4], 0x00800FFF);  /* máscara de escritura */
}

static void ctc1_raises_pending_cause(void)
{
	R[T0] = 1u << 12 | 1u << 7;  /* causa I con I habilitada */
	R[T1] = 1u << 17;            /* causa E */
	T_LOAD(CTC1(T0, 31), CTC1(T1, 31));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CPU->fcr31, 1u << 12 | 1u << 7);
	t_set_status(T_STATUS);
	retro64_cpu_set_pc(CPU, T_CODE + 4);
	t_step(1);
	CHECK_EXC(RETRO64_EXC_FPE, T_CODE + 4, 0, T_VEC_GEN);
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
	CPU->fcr31 = RETRO64_FCR31_FS | 2;
	rerun();
	CHECK_EQ(D(3), 0x0010000000000000ull);
	CHECK_EQ(CAUSE(), CU | CI);
	CPU->fcr31 = RETRO64_FCR31_FS;
	rerun();
	CHECK_EQ(D(3), 0);
	CPU->fcr31 = RETRO64_FCR31_FS | (CU << 7);
	rerun();
	CHECK_EQ(RETRO64_CAUSE_EXC(CPU->cause), RETRO64_EXC_FPE);
	CHECK_EQ(CAUSE(), CE);
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
}
