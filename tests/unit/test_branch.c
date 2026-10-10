/*
 * Retro64 — pruebas de saltos: delay slots, branch-likely, enlaces y
 * excepciones dentro de un delay slot.
 */
#include "test.h"

static void beq_taken_runs_delay_slot(void)
{
	R[T0] = R[T1] = 1;
	T_LOAD(BEQ(T0, T1, 2),       /* destino: T_CODE + 4 + 8 */
	       ADDIU(T2, ZERO, 1),   /* delay slot */
	       ADDIU(T3, ZERO, 1),   /* se salta */
	       ADDIU(T4, ZERO, 1));
	t_step(3);
	CHECK_EQ(R[T2], 1);
	CHECK_EQ(R[T3], 0);
	CHECK_EQ(R[T4], 1);
	CHECK_EQ(CPU->pc, T_CODE + 16);
}

static void branch_not_taken_runs_delay_slot(void)
{
	R[T0] = R[T1] = 1;
	T_LOAD(BNE(T0, T1, 5), ADDIU(T2, ZERO, 1), ADDIU(T3, ZERO, 1));
	t_step(3);
	CHECK_EQ(R[T2], 1);
	CHECK_EQ(R[T3], 1);
	CHECK_EQ(CPU->pc, T_CODE + 12);
}

static void branch_likely_not_taken_nullifies_slot(void)
{
	R[T0] = 1;
	R[T1] = 2;
	T_LOAD(BEQL(T0, T1, 5), ADDIU(T2, ZERO, 1), ADDIU(T3, ZERO, 1));
	t_step(2);
	CHECK_EQ(R[T2], 0);
	CHECK_EQ(R[T3], 1);
	CHECK_EQ(CPU->pc, T_CODE + 12);
}

static void branch_likely_taken_runs_slot(void)
{
	R[T0] = 1;
	R[T1] = 2;
	T_LOAD(BNEL(T0, T1, 2), ADDIU(T2, ZERO, 1), ADDIU(T3, ZERO, 1), ADDIU(T4, ZERO, 1));
	t_step(3);
	CHECK_EQ(R[T2], 1);
	CHECK_EQ(R[T3], 0);
	CHECK_EQ(R[T4], 1);
}

static void regimm_likely(void)
{
	R[T0] = (uint64_t)-5;
	T_LOAD(BGEZL(T0, 5), ADDIU(T1, ZERO, 1), BLTZL(T0, 2), ADDIU(T2, ZERO, 1),
	       ADDIU(T3, ZERO, 1), ADDIU(T4, ZERO, 1));
	t_step(4);
	CHECK_EQ(R[T1], 0);  /* anulado */
	CHECK_EQ(R[T2], 1);
	CHECK_EQ(R[T3], 0);
	CHECK_EQ(R[T4], 1);
}

static void blez_bgtz_on_64_bits(void)
{
	R[T0] = 0x0000000100000000ull;  /* positivo en 64 bits, 0 en los 32 bajos */
	T_LOAD(BLEZ(T0, 5), NOP, BGTZ(T0, 2), NOP, ADDIU(T1, ZERO, 1), ADDIU(T2, ZERO, 1));
	t_step(5);
	CHECK_EQ(R[T1], 0);
	CHECK_EQ(R[T2], 1);
}

static void jal_links_and_jumps(void)
{
	T_LOAD(JAL(0x80001010), ADDIU(T0, ZERO, 7), NOP, NOP, ADDIU(T1, ZERO, 9));
	t_step(3);
	CHECK_EQ(R[RA], T_CODE + 8);
	CHECK_EQ(R[T0], 7);
	CHECK_EQ(R[T1], 9);
	CHECK_EQ(CPU->pc, T_CODE + 0x14);
}

/* Un JAL en el delay slot de J enlaza al destino de J + 4: la instrucción que
 * sigue a su propio delay slot (nemu64-test, jumps). */
static void jal_in_delay_slot_links_after_target(void)
{
	T_LOAD(J(0x80001020), JAL(0x80001040));
	t_step(3);
	CHECK_EQ(R[RA], T_CODE + 0x24);
	CHECK_EQ(CPU->pc, T_CODE + 0x40);
}

static void j_keeps_upper_region(void)
{
	T_LOAD(J(0x00002000), NOP);
	t_step(2);
	CHECK_EQ(CPU->pc, 0xFFFFFFFF80002000ull);
}

static void jalr_custom_link(void)
{
	R[T0] = T_CODE + 0x10;
	T_LOAD(JALR(T1, T0), NOP, NOP, NOP, ADDIU(T2, ZERO, 3));
	t_step(3);
	CHECK_EQ(R[T1], T_CODE + 8);
	CHECK_EQ(R[T2], 3);
}

static void jalr_same_register_uses_old_value(void)
{
	R[T0] = T_CODE + 0x10;
	T_LOAD(JALR(T0, T0), NOP, NOP, NOP, ADDIU(T2, ZERO, 3));
	t_step(3);
	CHECK_EQ(R[T0], T_CODE + 8);
	CHECK_EQ(R[T2], 3);
}

static void jr_returns(void)
{
	R[RA] = T_CODE + 0x0C;
	T_LOAD(JR(RA), ADDIU(T0, ZERO, 1), ADDIU(T1, ZERO, 1), ADDIU(T2, ZERO, 1));
	t_step(3);
	CHECK_EQ(R[T0], 1);
	CHECK_EQ(R[T1], 0);
	CHECK_EQ(R[T2], 1);
}

static void bltzal_links_even_if_not_taken(void)
{
	R[T0] = 5;
	T_LOAD(BLTZAL(T0, 4), NOP, ADDIU(T1, ZERO, 1));
	t_step(3);
	CHECK_EQ(R[RA], T_CODE + 8);
	CHECK_EQ(R[T1], 1);
}

static void bltzal_reads_ra_before_link(void)
{
	/* El enlace (dirección KSEG0, negativa) no debe afectar la condición. */
	R[RA] = 1;
	T_LOAD(BLTZAL(RA, 4), NOP, ADDIU(T1, ZERO, 1));
	t_step(3);
	CHECK_EQ(R[RA], T_CODE + 8);
	CHECK_EQ(R[T1], 1);
	CHECK_EQ(CPU->pc, T_CODE + 12);
}

static void backward_loop(void)
{
	R[T0] = 3;
	T_LOAD(ADDIU(T1, T1, 1), ADDIU(T0, T0, -1), BNE(T0, ZERO, -3), NOP, ADDIU(T2, ZERO, 1));
	t_step(13);
	CHECK_EQ(R[T1], 3);
	CHECK_EQ(R[T0], 0);
	CHECK_EQ(R[T2], 1);
}

static void exception_in_taken_delay_slot(void)
{
	R[T0] = 0x7FFFFFFF;
	R[T1] = 1;
	T_LOAD(BEQ(ZERO, ZERO, 4), ADD(T2, T0, T1));
	t_step(2);
	CHECK_EXC(RETRO64_EXC_OV, T_CODE, 1, T_VEC_GEN);
	CHECK_EQ(R[T2], 0);
}

static void exception_in_not_taken_delay_slot(void)
{
	R[T0] = 0x7FFFFFFF;
	R[T1] = 1;
	T_LOAD(BNE(ZERO, ZERO, 4), ADD(T2, T0, T1));
	t_step(2);
	CHECK_EXC(RETRO64_EXC_OV, T_CODE, 1, T_VEC_GEN);
}

static void exception_after_slot_clears_bd(void)
{
	R[T0] = 0x7FFFFFFF;
	R[T1] = 1;
	T_LOAD(BNE(ZERO, ZERO, 4), NOP, ADD(T2, T0, T1));
	t_step(3);
	CHECK_EXC(RETRO64_EXC_OV, T_CODE + 8, 0, T_VEC_GEN);
}

static void eret_reexecutes_branch(void)
{
	/* SYSCALL en el delay slot: EPC apunta al salto; ERET vuelve a él. */
	t_wr32(0x180, ERET);
	R[T0] = R[T1] = 1;
	T_LOAD(BEQ(T0, T1, 2), SYSCALL, NOP, ADDIU(T2, ZERO, 1));
	t_step(2);
	CHECK_EXC(RETRO64_EXC_SYS, T_CODE, 1, T_VEC_GEN);
	t_step(1);  /* ERET */
	CHECK_EQ(CPU->pc, T_CODE);
	CHECK_EQ(CPU->status & RETRO64_ST_EXL, 0);
}

void t_suite_branch(void)
{
	RUN(beq_taken_runs_delay_slot);
	RUN(branch_not_taken_runs_delay_slot);
	RUN(branch_likely_not_taken_nullifies_slot);
	RUN(branch_likely_taken_runs_slot);
	RUN(regimm_likely);
	RUN(blez_bgtz_on_64_bits);
	RUN(jal_links_and_jumps);
	RUN(j_keeps_upper_region);
	RUN(jalr_custom_link);
	RUN(jalr_same_register_uses_old_value);
	RUN(jr_returns);
	RUN(jal_in_delay_slot_links_after_target);
	RUN(bltzal_links_even_if_not_taken);
	RUN(bltzal_reads_ra_before_link);
	RUN(backward_loop);
	RUN(exception_in_taken_delay_slot);
	RUN(exception_in_not_taken_delay_slot);
	RUN(exception_after_slot_clears_bd);
	RUN(eret_reexecutes_branch);
}
