/*
 * Salsa64 — pruebas de cargas y almacenamientos: anchos, extensión de
 * signo, accesos no alineados (LWL/LWR/SWL/SWR/LDL/LDR/SDL/SDR), errores de
 * dirección y LL/SC.
 */
#include "test.h"

static void load_widths_and_sign(void)
{
	R[T0] = T_DATA;
	R[T1] = 0x8899AABBCCDDEEFFull;
	T_LOAD(SD(T1, 0, T0), LD(T2, 0, T0), LW(T3, 0, T0), LWU(T4, 4, T0), LH(T5, 6, T0),
	       LHU(T6, 6, T0), LB(T7, 1, T0), LBU(T8, 1, T0));
	t_step(8);
	CHECK_EQ(t_rd64(T_DATA_PA), 0x8899AABBCCDDEEFFull);
	CHECK_EQ(R[T2], 0x8899AABBCCDDEEFFull);
	CHECK_EQ(R[T3], 0xFFFFFFFF8899AABBull);
	CHECK_EQ(R[T4], 0xCCDDEEFF);
	CHECK_EQ(R[T5], 0xFFFFFFFFFFFFEEFFull);
	CHECK_EQ(R[T6], 0xEEFF);
	CHECK_EQ(R[T7], 0xFFFFFFFFFFFFFF99ull);
	CHECK_EQ(R[T8], 0x99);
}

static void narrow_stores(void)
{
	t_wr32(T_DATA_PA, 0x11223344);
	R[T0] = T_DATA;
	R[T1] = 0xFFFFFFFFFFFFAABBull;
	T_LOAD(SB(T1, 1, T0), SH(T1, 2, T0));
	t_step(2);
	CHECK_EQ(t_rd32(T_DATA_PA), 0x11BBAABB);
}

static void lwl_lwr_pair(void)
{
	t_wr64(T_DATA_PA, 0x0011223344556677ull);
	R[T0] = T_DATA;
	T_LOAD(LWL(T1, 1, T0), LWR(T1, 4, T0), LWL(T2, 4, T0), LWR(T2, 7, T0));
	t_step(4);
	CHECK_EQ(R[T1], 0x11223344);
	CHECK_EQ(R[T2], 0x44556677);
}

static void lwl_lwr_merge(void)
{
	t_wr32(T_DATA_PA, 0x00112233);
	R[T0] = T_DATA;
	R[T1] = 0xAABBCCDD;
	R[T2] = 0x00000000AABBCCDDull;
	R[T3] = 0x12345678;
	t_wr32(T_DATA_PA + 4, 0x8899AABB);
	T_LOAD(LWL(T1, 2, T0), LWR(T2, 1, T0), LWR(T3, 7, T0));
	t_step(3);
	CHECK_EQ(R[T1], 0x2233CCDD);
	CHECK_EQ(R[T2], 0x00000000AABB0011ull);  /* LWR parcial: los 32 bits altos quedan */
	CHECK_EQ(R[T3], 0xFFFFFFFF8899AABBull);
}

static void swl_swr_pair(void)
{
	t_wr64(T_DATA_PA, 0xFFFFFFFFFFFFFFFFull);
	R[T0] = T_DATA;
	R[T1] = 0x11223344;
	T_LOAD(SWL(T1, 1, T0), SWR(T1, 4, T0));
	t_step(2);
	CHECK_EQ(t_rd64(T_DATA_PA), 0xFF11223344FFFFFFull);
}

static void swl_swr_single(void)
{
	t_wr64(T_DATA_PA, 0);
	R[T0] = T_DATA;
	R[T1] = 0xAABBCCDD;
	T_LOAD(SWL(T1, 3, T0), SWR(T1, 4, T0));
	t_step(2);
	CHECK_EQ(t_rd64(T_DATA_PA), 0x000000AADD000000ull);
}

static void ldl_ldr_sdl_sdr(void)
{
	t_wr64(T_DATA_PA, 0x0011223344556677ull);
	t_wr64(T_DATA_PA + 8, 0x8899AABBCCDDEEFFull);
	R[T0] = T_DATA;
	T_LOAD(LDL(T1, 3, T0), LDR(T1, 10, T0), SDL(T1, 0x13, T0), SDR(T1, 0x1A, T0));
	t_step(4);
	CHECK_EQ(R[T1], 0x33445566778899AAull);
	CHECK_EQ(t_rd64(T_DATA_PA + 0x10), 0x0000003344556677ull);
	CHECK_EQ(t_rd64(T_DATA_PA + 0x18), 0x8899AA0000000000ull);
}

static void misaligned_load(void)
{
	R[T0] = T_DATA + 2;
	R[T1] = 0x55;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CPU->bad_vaddr, T_DATA + 2);
	CHECK_EQ(R[T1], 0x55);
}

static void misaligned_store(void)
{
	R[T0] = T_DATA;
	T_LOAD(SH(T1, 3, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_ADES, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CPU->bad_vaddr, T_DATA + 3);
}

static void misaligned_doubleword(void)
{
	R[T0] = T_DATA + 4;
	T_LOAD(LD(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE, 0, T_VEC_GEN);
}

static void address_not_sign_extended(void)
{
	/* En modo de 32 bits, una dirección que no es extensión de signo falla. */
	R[T0] = 0x0000000080002000ull;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ(CPU->bad_vaddr, 0x0000000080002000ull);
}

static void ll_sc_success(void)
{
	t_wr32(T_DATA_PA, 10);
	R[T0] = T_DATA;
	T_LOAD(LL(T1, 0, T0), ADDIU(T1, T1, 5), SC(T1, 0, T0));
	t_step(3);
	CHECK_EQ(R[T1], 1);
	CHECK_EQ(t_rd32(T_DATA_PA), 15);
	CHECK_EQ(CPU->lladdr, T_DATA_PA >> 4);
}

static void sc_without_ll_fails(void)
{
	t_wr32(T_DATA_PA, 10);
	R[T0] = T_DATA;
	R[T1] = 99;
	T_LOAD(SC(T1, 0, T0));
	t_step(1);
	CHECK_EQ(R[T1], 0);
	CHECK_EQ(t_rd32(T_DATA_PA), 10);
}

static void eret_clears_llbit(void)
{
	t_wr32(T_DATA_PA, 10);
	R[T0] = T_DATA;
	R[T2] = 7;
	t_set_status(T_STATUS | SALSA64_ST_EXL);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_EPC, T_CODE + 12);
	T_LOAD(LL(T1, 0, T0), ERET, NOP, SC(T2, 0, T0));
	t_step(3);
	CHECK_EQ(R[T2], 0);
	CHECK_EQ(t_rd32(T_DATA_PA), 10);
}

static void load_to_r0_discarded(void)
{
	t_wr32(T_DATA_PA, 0x12345678);
	R[T0] = T_DATA;
	T_LOAD(LW(ZERO, 0, T0), LD(ZERO, 0, T0));
	t_step(2);
	CHECK_EQ(R[ZERO], 0);
}

static void uncached_alias_store(void)
{
	R[T0] = 0xFFFFFFFFA0002000ull;
	R[T1] = 0x0123456789ABCDEFull;
	T_LOAD(SD(T1, 8, T0));
	t_step(1);
	CHECK_EQ(t_rd64(T_DATA_PA + 8), 0x0123456789ABCDEFull);
}

void t_suite_mem(void)
{
	RUN(load_widths_and_sign);
	RUN(narrow_stores);
	RUN(lwl_lwr_pair);
	RUN(lwl_lwr_merge);
	RUN(swl_swr_pair);
	RUN(swl_swr_single);
	RUN(ldl_ldr_sdl_sdr);
	RUN(misaligned_load);
	RUN(misaligned_store);
	RUN(misaligned_doubleword);
	RUN(address_not_sign_extended);
	RUN(ll_sc_success);
	RUN(sc_without_ll_fails);
	RUN(eret_clears_llbit);
	RUN(load_to_r0_discarded);
	RUN(uncached_alias_store);
}
