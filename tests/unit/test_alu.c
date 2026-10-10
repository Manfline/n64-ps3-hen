/*
 * Salsa64 — pruebas de la ALU entera: extensión de signo, desbordamiento,
 * comparaciones, lógicas y desplazamientos.
 */
#include "test.h"

static void addiu_sign_extends(void)
{
	R[T1] = 0x7FFFFFFF;
	R[T3] = 0x0000000100000005ull;  /* bits altos que no son extensión de signo */
	T_LOAD(ADDIU(T0, ZERO, -1), ADDIU(T2, T1, 1), ADDIU(T4, T3, 1));
	t_step(3);
	CHECK_EQ(R[T0], 0xFFFFFFFFFFFFFFFFull);
	CHECK_EQ(R[T2], 0xFFFFFFFF80000000ull);  /* ADDIU no atrapa */
	CHECK_EQ(R[T4], 6);
}

static void lui_sign_extends(void)
{
	T_LOAD(LUI(T0, 0x8000), LUI(T1, 0x7FFF), ORI(T1, T1, 0xFFFF));
	t_step(3);
	CHECK_EQ(R[T0], 0xFFFFFFFF80000000ull);
	CHECK_EQ(R[T1], 0x7FFFFFFF);
}

static void addu_uses_low_32_bits(void)
{
	R[T0] = 0x0000000100000000ull;
	R[T1] = 5;
	R[T2] = 0x00000000FFFFFFFFull;
	T_LOAD(ADDU(T3, T0, T1), ADDU(T4, T2, ZERO), SUBU(T5, ZERO, T1));
	t_step(3);
	CHECK_EQ(R[T3], 5);
	CHECK_EQ(R[T4], 0xFFFFFFFFFFFFFFFFull);
	CHECK_EQ(R[T5], 0xFFFFFFFFFFFFFFFBull);
}

static void add_overflow_traps(void)
{
	R[T0] = 0x7FFFFFFF;
	R[T1] = 1;
	R[T2] = 0x1234;
	T_LOAD(ADD(T2, T0, T1));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_OV, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(R[T2], 0x1234);  /* rd intacto */
}

static void add_negative_no_overflow(void)
{
	R[T0] = 0xFFFFFFFFFFFFFFFFull;
	R[T1] = 0xFFFFFFFF80000000ull;
	T_LOAD(ADD(T2, T0, T0), ADD(T3, T1, ZERO));
	t_step(2);
	CHECK_EQ(R[T2], 0xFFFFFFFFFFFFFFFEull);
	CHECK_EQ(R[T3], 0xFFFFFFFF80000000ull);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
}

static void add_overflow_negative(void)
{
	R[T0] = 0xFFFFFFFF80000000ull;
	T_LOAD(ADD(T1, T0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_OV, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(R[T1], 0);
}

static void sub_overflow_traps(void)
{
	R[T0] = 0xFFFFFFFF80000000ull;
	R[T1] = 1;
	T_LOAD(SUB(T2, T1, T0), SUB(T3, T0, T1));
	t_step(1);  /* 1 - INT32_MIN desborda */
	CHECK_EXC(SALSA64_EXC_OV, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(R[T2], 0);
}

static void sub_no_overflow(void)
{
	R[T0] = 5;
	R[T1] = 7;
	T_LOAD(SUB(T2, T0, T1));
	t_step(1);
	CHECK_EQ(R[T2], 0xFFFFFFFFFFFFFFFEull);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
}

static void addi_overflow_traps(void)
{
	R[T0] = 0x7FFFFFF0;
	T_LOAD(ADDI(T1, T0, 0x0F), ADDI(T2, T0, 0x10));
	t_step(2);
	CHECK_EQ(R[T1], 0x7FFFFFFF);
	CHECK_EXC(SALSA64_EXC_OV, T_CODE + 4, 0, T_VEC_GEN);
	CHECK_EQ(R[T2], 0);
}

static void dadd_overflow_traps(void)
{
	R[T0] = 0x7FFFFFFFFFFFFFFFull;
	R[T1] = 1;
	T_LOAD(DADDU(T2, T0, T1), DADD(T3, T0, T1));
	t_step(2);
	CHECK_EQ(R[T2], 0x8000000000000000ull);
	CHECK_EXC(SALSA64_EXC_OV, T_CODE + 4, 0, T_VEC_GEN);
	CHECK_EQ(R[T3], 0);
}

static void dsub_and_daddi(void)
{
	R[T0] = 0x8000000000000000ull;
	R[T1] = 0x0000000100000000ull;
	T_LOAD(DSUBU(T2, T0, T1), DADDIU(T3, T1, -1), DADDI(T4, T1, 0x7FFF), DSUB(T5, T0, T1));
	t_step(4);
	CHECK_EQ(R[T2], 0x7FFFFFFF00000000ull);
	CHECK_EQ(R[T3], 0x00000000FFFFFFFFull);
	CHECK_EQ(R[T4], 0x0000000100007FFFull);
	CHECK_EXC(SALSA64_EXC_OV, T_CODE + 12, 0, T_VEC_GEN);
}

static void set_less_than(void)
{
	R[T0] = 0xFFFFFFFFFFFFFFFFull;  /* -1 */
	R[T1] = 1;
	T_LOAD(SLT(T2, T0, T1), SLTU(T3, T0, T1), SLTI(T4, T1, -1), SLTIU(T5, T1, -1),
	       SLTIU(T6, T0, -1));
	t_step(5);
	CHECK_EQ(R[T2], 1);
	CHECK_EQ(R[T3], 0);
	CHECK_EQ(R[T4], 0);
	CHECK_EQ(R[T5], 1);  /* el inmediato se extiende con signo y se compara sin signo */
	CHECK_EQ(R[T6], 0);
}

static void logic_immediates_zero_extend(void)
{
	R[T0] = 0xFFFFFFFFFFFFFFFFull;
	R[T1] = 0xF0F0F0F0F0F0F0F0ull;
	T_LOAD(ANDI(T2, T0, 0x8001), ORI(T3, ZERO, 0x8000), XORI(T4, T0, 0xFFFF),
	       NOR(T5, T1, ZERO), XOR(T6, T1, T0), AND(T7, T1, T0), OR(T8, T1, ZERO));
	t_step(7);
	CHECK_EQ(R[T2], 0x8001);
	CHECK_EQ(R[T3], 0x8000);
	CHECK_EQ(R[T4], 0xFFFFFFFFFFFF0000ull);
	CHECK_EQ(R[T5], 0x0F0F0F0F0F0F0F0Full);
	CHECK_EQ(R[T6], 0x0F0F0F0F0F0F0F0Full);
	CHECK_EQ(R[T7], 0xF0F0F0F0F0F0F0F0ull);
	CHECK_EQ(R[T8], 0xF0F0F0F0F0F0F0F0ull);
}

static void shifts_32(void)
{
	R[T0] = 0x0000000080000000ull;  /* no extendido */
	R[T1] = 0xFFFFFFFF80000000ull;
	R[T2] = 36;                     /* SLLV usa solo 5 bits: 4 */
	T_LOAD(SLL(T3, T0, 0), SRL(T4, T1, 4), SRA(T5, T1, 4), SLLV(T6, T0, T2),
	       SRLV(T7, T1, T2), SLL(T8, T0, 1));
	t_step(6);
	CHECK_EQ(R[T3], 0xFFFFFFFF80000000ull);  /* SLL 0 extiende el signo */
	CHECK_EQ(R[T4], 0x08000000);
	CHECK_EQ(R[T5], 0xFFFFFFFFF8000000ull);
	CHECK_EQ(R[T6], 0);
	CHECK_EQ(R[T7], 0x08000000);
	CHECK_EQ(R[T8], 0);
}

static void sra_shifts_in_upper_bits(void)
{
	/* Particularidad del VR4300: SRA/SRAV desplazan los 64 bits. */
	R[T0] = 0x0000000100000000ull;
	R[T1] = 0xFFFFFFFF0000000Full;
	R[T2] = 4;
	T_LOAD(SRA(T3, T0, 4), SRAV(T4, T1, T2), SRA(T5, T0, 0));
	t_step(3);
	CHECK_EQ(R[T3], 0x10000000);
	CHECK_EQ(R[T4], 0xFFFFFFFFF0000000ull);
	CHECK_EQ(R[T5], 0);
}

static void shifts_64(void)
{
	R[T0] = 0x8000000000000001ull;
	R[T1] = 68;  /* DSLLV usa 6 bits: 4 */
	T_LOAD(DSLL(T2, T0, 4), DSRL(T3, T0, 4), DSRA(T4, T0, 4), DSLL32(T5, T0, 0),
	       DSRL32(T6, T0, 31), DSRA32(T7, T0, 0), DSLLV(T8, T0, T1), DSRAV(T9, T0, T1));
	t_step(8);
	CHECK_EQ(R[T2], 0x0000000000000010ull);
	CHECK_EQ(R[T3], 0x0800000000000000ull);
	CHECK_EQ(R[T4], 0xF800000000000000ull);
	CHECK_EQ(R[T5], 0x0000000100000000ull);
	CHECK_EQ(R[T6], 1);
	CHECK_EQ(R[T7], 0xFFFFFFFF80000000ull);
	CHECK_EQ(R[T8], 0x0000000000000010ull);
	CHECK_EQ(R[T9], 0xF800000000000000ull);
}

static void r0_is_hardwired(void)
{
	T_LOAD(ADDIU(ZERO, ZERO, 5), ADDU(T0, ZERO, ZERO));
	t_step(2);
	CHECK_EQ(R[ZERO], 0);
	CHECK_EQ(R[T0], 0);
}

void t_suite_alu(void)
{
	RUN(addiu_sign_extends);
	RUN(lui_sign_extends);
	RUN(addu_uses_low_32_bits);
	RUN(add_overflow_traps);
	RUN(add_negative_no_overflow);
	RUN(add_overflow_negative);
	RUN(sub_overflow_traps);
	RUN(sub_no_overflow);
	RUN(addi_overflow_traps);
	RUN(dadd_overflow_traps);
	RUN(dsub_and_daddi);
	RUN(set_less_than);
	RUN(logic_immediates_zero_extend);
	RUN(shifts_32);
	RUN(sra_shifts_in_upper_bits);
	RUN(shifts_64);
	RUN(r0_is_hardwired);
}
