/*
 * Salsa64 — pruebas de multiplicación, división y HI/LO.
 */
#include "test.h"

static void mult_signed(void)
{
	R[T0] = (uint64_t)-3;
	R[T1] = 7;
	T_LOAD(MULT(T0, T1), MFLO(T2), MFHI(T3));
	t_step(3);
	CHECK_EQ(R[T2], (uint64_t)-21);
	CHECK_EQ(R[T3], 0xFFFFFFFFFFFFFFFFull);
}

static void multu_extends_halves(void)
{
	R[T0] = 0xFFFFFFFF;
	T_LOAD(MULTU(T0, T0));
	t_step(1);
	/* 0xFFFFFFFE00000001: LO = 1, HI = 0xFFFFFFFE extendido. */
	CHECK_EQ(CPU->lo, 1);
	CHECK_EQ(CPU->hi, 0xFFFFFFFFFFFFFFFEull);
}

/* Con entradas que no son extensión de signo de 32 bits, MULT multiplica rs
 * completo por rt como entero de 35 bits con signo (nemu64-test). */
static void mult_upper_bits(void)
{
	R[T0] = 0x0000000100000002ull;  /* rs: se usan los 64 bits */
	R[T1] = 0x0000000140000000ull;  /* rt: bits 34..0 */
	T_LOAD(MULT(T0, T1));
	t_step(1);
	/* 0x100000002 × 0x140000000 = 0x1_40000002_80000000 (mod 2^64): */
	CHECK_EQ(CPU->lo, 0xFFFFFFFF80000000ull);
	CHECK_EQ(CPU->hi, 0x40000002);

	R[T0] = 0x10;
	R[T1] = 0x0000000400000000ull;  /* bit 34 en 1: rt vale -2^34 */
	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(1);
	CHECK_EQ(CPU->lo, 0);
	CHECK_EQ(CPU->hi, 0xFFFFFFFFFFFFFFC0ull);
}

static void div_signed(void)
{
	R[T0] = 7;
	R[T1] = (uint64_t)-2;
	T_LOAD(DIV(T0, T1));
	t_step(1);
	CHECK_EQ(CPU->lo, (uint64_t)-3);
	CHECK_EQ(CPU->hi, 1);
}

static void div_by_zero(void)
{
	R[T0] = 9;
	R[T1] = (uint64_t)-9;
	T_LOAD(DIV(T0, ZERO), MFLO(T2), MFHI(T3), DIV(T1, ZERO), MFLO(T4), MFHI(T5));
	t_step(6);
	CHECK_EQ(R[T2], 0xFFFFFFFFFFFFFFFFull);
	CHECK_EQ(R[T3], 9);
	CHECK_EQ(R[T4], 1);
	CHECK_EQ(R[T5], (uint64_t)-9);
}

static void div_int_min_by_minus_one(void)
{
	R[T0] = 0xFFFFFFFF80000000ull;
	R[T1] = (uint64_t)-1;
	T_LOAD(DIV(T0, T1));
	t_step(1);
	CHECK_EQ(CPU->lo, 0xFFFFFFFF80000000ull);
	CHECK_EQ(CPU->hi, 0);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
}

static void divu(void)
{
	R[T0] = 0xFFFFFFFF;
	R[T1] = 2;
	R[T2] = 0x80000000;
	T_LOAD(DIVU(T0, T1), MFLO(T3), MFHI(T4), DIVU(T2, ZERO));
	t_step(4);
	CHECK_EQ(R[T3], 0x7FFFFFFF);
	CHECK_EQ(R[T4], 1);
	CHECK_EQ(CPU->lo, 0xFFFFFFFFFFFFFFFFull);
	CHECK_EQ(CPU->hi, 0xFFFFFFFF80000000ull);
}

static void dmult_dmultu(void)
{
	R[T0] = (uint64_t)-1;
	R[T1] = 0x7FFFFFFFFFFFFFFFull;
	R[T2] = 2;
	T_LOAD(DMULT(T0, T0), MFLO(T3), MFHI(T4), DMULTU(T0, T0), MFLO(T5), MFHI(T6),
	       DMULT(T1, T2), MFLO(T7), MFHI(T8));
	t_step(9);
	CHECK_EQ(R[T3], 1);
	CHECK_EQ(R[T4], 0);
	CHECK_EQ(R[T5], 1);
	CHECK_EQ(R[T6], 0xFFFFFFFFFFFFFFFEull);
	CHECK_EQ(R[T7], 0xFFFFFFFFFFFFFFFEull);
	CHECK_EQ(R[T8], 0);
}

static void ddiv_ddivu(void)
{
	R[T0] = 0x8000000000000000ull;
	R[T1] = (uint64_t)-1;
	R[T2] = 10;
	R[T3] = 3;
	T_LOAD(DDIV(T0, T1), MFLO(T4), MFHI(T5), DDIVU(T1, T2), MFLO(T6), MFHI(T7),
	       DDIV(T2, ZERO), MFLO(T8), DDIV(T0, T3));
	t_step(9);
	CHECK_EQ(R[T4], 0x8000000000000000ull);
	CHECK_EQ(R[T5], 0);
	CHECK_EQ(R[T6], 0x1999999999999999ull);
	CHECK_EQ(R[T7], 5);
	CHECK_EQ(R[T8], 0xFFFFFFFFFFFFFFFFull);
	CHECK_EQ(CPU->lo, (uint64_t)(INT64_MIN / 3));
	CHECK_EQ(CPU->hi, (uint64_t)(INT64_MIN % 3));
}

static void move_hi_lo(void)
{
	R[T0] = 0x1122334455667788ull;
	R[T1] = 0x99AABBCCDDEEFF00ull;
	T_LOAD(MTHI(T0), MTLO(T1), MFHI(T2), MFLO(T3));
	t_step(4);
	CHECK_EQ(R[T2], 0x1122334455667788ull);
	CHECK_EQ(R[T3], 0x99AABBCCDDEEFF00ull);
}

void t_suite_muldiv(void)
{
	RUN(mult_signed);
	RUN(multu_extends_halves);
	RUN(mult_upper_bits);
	RUN(div_signed);
	RUN(div_by_zero);
	RUN(div_int_min_by_minus_one);
	RUN(divu);
	RUN(dmult_dmultu);
	RUN(ddiv_ddivu);
	RUN(move_hi_lo);
}
