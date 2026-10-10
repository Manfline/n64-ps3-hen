/*
 * Retro64 — pruebas de COP0: registros, excepciones, ERET, Count/Compare,
 * interrupciones, TLB y restricciones de modo usuario.
 */
#include "test.h"

#define KSU_USER 0x10u

/* Escribe una entrada de la TLB ejecutando TLBWI desde una dirección aparte,
 * sin tocar el programa de la prueba. */
static void tlb_set(unsigned index, uint64_t hi, uint32_t lo0, uint32_t lo1, uint32_t page_mask)
{
	uint64_t pc = CPU->pc;
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_INDEX, index);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ENTRYHI, hi);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ENTRYLO0, lo0);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ENTRYLO1, lo1);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_PAGEMASK, page_mask);
	t_wr32(0x0F00, TLBWI);
	retro64_cpu_set_pc(CPU, 0xFFFFFFFF80000F00ull);
	t_step(1);
	retro64_cpu_set_pc(CPU, pc);
}

/* EntryLo: PFN | C=3 | D | V | G */
#define LO(pfn, d, v, g) ((uint32_t)(pfn) << 6 | 3u << 3 | (d) << 2 | (v) << 1 | (g))

static void prid_and_config(void)
{
	T_LOAD(MFC0(T0, RETRO64_CP0_PRID), MFC0(T1, RETRO64_CP0_CONFIG));
	t_step(2);
	CHECK_EQ(R[T0], RETRO64_PRID_VALUE);
	CHECK_EQ(R[T1], 0x7006E463);
}

static void write_masks(void)
{
	R[T0] = 0xFFFFFFFF;
	R[T1] = 0xFFFFFFE0;  /* sin KSU, ERL, EXL, IE */
	T_LOAD(MTC0(T0, RETRO64_CP0_CAUSE), MFC0(T2, RETRO64_CP0_CAUSE),
	       MTC0(T1, RETRO64_CP0_STATUS), MFC0(T3, RETRO64_CP0_STATUS));
	t_step(4);
	CHECK_EQ(R[T2], 0x300);
	CHECK_EQ(R[T3], 0xFFFFFFFFFF57FFE0ull);
}

static void mtc0_sign_extends_64bit_registers(void)
{
	R[T0] = 0x80001234;
	R[T1] = 0x0000001234567890ull;
	T_LOAD(MTC0(T0, RETRO64_CP0_EPC), DMFC0(T2, RETRO64_CP0_EPC),
	       DMTC0(T1, RETRO64_CP0_EPC), DMFC0(T3, RETRO64_CP0_EPC), MFC0(T4, RETRO64_CP0_EPC));
	t_step(5);
	CHECK_EQ(R[T2], 0xFFFFFFFF80001234ull);
	CHECK_EQ(R[T3], 0x0000001234567890ull);
	CHECK_EQ(R[T4], 0x34567890);
}

static void syscall_and_eret(void)
{
	t_wr32(0x180, ERET);
	T_LOAD(SYSCALL, ADDIU(T0, ZERO, 1));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_SYS, T_CODE, 0, T_VEC_GEN);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_EPC, T_CODE + 4);  /* el manejador salta el SYSCALL */
	t_step(2);
	CHECK_EQ(CPU->status & RETRO64_ST_EXL, 0);
	CHECK_EQ(R[T0], 1);
}

static void break_exception(void)
{
	T_LOAD(NOP, BREAK);
	t_step(2);
	CHECK_EXC(RETRO64_EXC_BP, T_CODE + 4, 0, T_VEC_GEN);
}

static void bev_vectors(void)
{
	t_set_status(T_STATUS | RETRO64_ST_BEV);
	T_LOAD(SYSCALL);
	t_step(1);
	CHECK_EXC(RETRO64_EXC_SYS, T_CODE, 0, 0xFFFFFFFFBFC00380ull);
}

static void nested_exception_keeps_epc(void)
{
	t_set_status(T_STATUS | RETRO64_ST_EXL);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_EPC, 0x1234);
	T_LOAD(SYSCALL);
	t_step(1);
	CHECK_EXC(RETRO64_EXC_SYS, 0x1234, 0, T_VEC_GEN);
}

static void reserved_instruction(void)
{
	T_LOAD(0x70000000u);  /* opcode 0x1C */
	t_step(1);
	CHECK_EXC(RETRO64_EXC_RI, T_CODE, 0, T_VEC_GEN);
}

static void cop2_unusable(void)
{
	T_LOAD(0x48000000u);  /* COP2 */
	t_step(1);
	CHECK_EXC(RETRO64_EXC_CPU, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> RETRO64_CAUSE_CE_SHIFT) & 3, 2);
}

static void traps(void)
{
	R[T0] = R[T1] = 5;
	T_LOAD(TNE(T0, T1), TGE(T0, T1));
	t_step(2);
	CHECK_EXC(RETRO64_EXC_TR, T_CODE + 4, 0, T_VEC_GEN);
}

static void trap_immediate(void)
{
	R[T0] = (uint64_t)-3;
	T_LOAD(TEQI(T0, -3));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_TR, T_CODE, 0, T_VEC_GEN);
}

static void misaligned_fetch(void)
{
	R[T0] = T_CODE + 0x22;
	T_LOAD(JR(T0), NOP);
	t_step(3);
	CHECK_EXC(RETRO64_EXC_ADEL, T_CODE + 0x22, 0, T_VEC_GEN);
	CHECK_EQ(CPU->bad_vaddr, T_CODE + 0x22);
}

static void count_half_rate(void)
{
	T_LOAD(MFC0(T0, RETRO64_CP0_COUNT));
	t_wr32(T_CODE_PA + 80, MFC0(T1, RETRO64_CP0_COUNT));  /* 20 instrucciones después */
	t_step(21);
	CHECK_EQ((uint32_t)(R[T1] - R[T0]), 10);
}

static void count_write(void)
{
	R[T0] = 0x12345;
	T_LOAD(MTC0(T0, RETRO64_CP0_COUNT), MFC0(T1, RETRO64_CP0_COUNT));
	t_step(2);
	CHECK(R[T1] - 0x12345 <= 1);
}

static void compare_interrupt(void)
{
	t_set_status(T_STATUS | RETRO64_ST_IE | 0x8000);  /* IM7 */
	T_LOAD(MFC0(T1, RETRO64_CP0_COUNT), ADDIU(T1, T1, 10), MTC0(T1, RETRO64_CP0_COMPARE));
	t_step(17);  /* 16 ciclos tras leer Count: todavía no */
	CHECK_EQ(CPU->status & RETRO64_ST_EXL, 0);
	CHECK_EQ(CPU->cause & 0x8000, 0);

	unsigned k = 0;
	while (k < 10 && !(CPU->status & RETRO64_ST_EXL)) {
		t_step(1);
		k++;
	}
	CHECK(k >= 3 && k <= 5);
	CHECK(CPU->cause & 0x8000);
	CHECK_EQ(RETRO64_CAUSE_EXC(CPU->cause), RETRO64_EXC_INT);
	CHECK_EQ(CPU->pc, T_VEC_GEN);

	retro64_cpu_cop0_write(CPU, RETRO64_CP0_COMPARE, 0);
	CHECK_EQ(CPU->cause & 0x8000, 0);
}

static void software_interrupt(void)
{
	t_set_status(T_STATUS | RETRO64_ST_IE | 0x100);  /* IM0 */
	R[T0] = 0x100;
	T_LOAD(MTC0(T0, RETRO64_CP0_CAUSE), NOP);
	t_step(2);
	CHECK_EXC(RETRO64_EXC_INT, T_CODE + 4, 0, T_VEC_GEN);
}

static void interrupt_blocked_by_exl_and_ie(void)
{
	R[T0] = 0x100;
	t_set_status(T_STATUS | RETRO64_ST_EXL | RETRO64_ST_IE | 0x100);
	T_LOAD(MTC0(T0, RETRO64_CP0_CAUSE), NOP, NOP);
	t_step(2);
	CHECK_EQ(CPU->pc, T_CODE + 8);
	t_set_status(T_STATUS | 0x100);  /* sin IE */
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 12);
}

static void mi_interrupt_on_ip2(void)
{
	struct retro64_bus *bus = &t_n64.bus;
	retro64_bus_write32(bus, 0x0430000C, 0x800, ~0u);  /* máscara DP */
	t_set_status(T_STATUS | RETRO64_ST_IE | 0x400);
	retro64_mi_raise(bus, RETRO64_IRQ_DP);
	CHECK(CPU->cause & 0x400);
	T_LOAD(NOP);
	t_step(1);
	CHECK_EXC(RETRO64_EXC_INT, T_CODE, 0, T_VEC_GEN);
	retro64_bus_write32(bus, 0x04300000, 0x800, ~0u);  /* MI_MODE: limpiar DP */
	CHECK_EQ(CPU->cause & 0x400, 0);
}

static void eret_from_error_level(void)
{
	t_set_status(T_STATUS | RETRO64_ST_ERL);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ERROREPC, T_CODE + 0x20);
	T_LOAD(ERET);
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 0x20);
	CHECK_EQ(CPU->status & RETRO64_ST_ERL, 0);
}

static void kuseg_unmapped_with_erl(void)
{
	t_wr32(T_DATA_PA, 0xABCD1234);
	t_set_status(T_STATUS | RETRO64_ST_ERL);
	R[T0] = T_DATA_PA;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EQ(R[T1], 0xFFFFFFFFABCD1234ull);
}

static void tlb_mapped_access(void)
{
	t_wr32(0x10010, 0xAAAA5555);
	t_wr32(0x11010, 0x12345678);
	R[T0] = 0;
	R[T1] = LO(0x10, 1, 1, 1);
	R[T2] = LO(0x11, 1, 1, 1);
	R[T3] = 5;
	R[T7] = 0x77;
	T_LOAD(MTC0(T0, RETRO64_CP0_ENTRYHI), MTC0(T1, RETRO64_CP0_ENTRYLO0),
	       MTC0(T2, RETRO64_CP0_ENTRYLO1), MTC0(ZERO, RETRO64_CP0_PAGEMASK),
	       MTC0(T3, RETRO64_CP0_INDEX), TLBWI,
	       LW(T4, 0x10, ZERO), LW(T5, 0x1010, ZERO), SW(T7, 0x14, ZERO),
	       TLBP, MFC0(T6, RETRO64_CP0_INDEX));
	t_step(11);
	CHECK_EQ(R[T4], 0xFFFFFFFFAAAA5555ull);
	CHECK_EQ(R[T5], 0x12345678);
	CHECK_EQ(t_rd32(0x10014), 0x77);
	CHECK_EQ(R[T6], 5);
}

static void tlb_read_back(void)
{
	tlb_set(7, 0x00402000 | 0x42, LO(0x20, 1, 1, 1), LO(0x21, 0, 1, 1), 0x6000);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ENTRYHI, 0);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ENTRYLO0, 0);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_PAGEMASK, 0);
	R[T0] = 7;
	T_LOAD(MTC0(T0, RETRO64_CP0_INDEX), TLBR, MFC0(T1, RETRO64_CP0_ENTRYHI),
	       MFC0(T2, RETRO64_CP0_ENTRYLO0), MFC0(T3, RETRO64_CP0_ENTRYLO1),
	       MFC0(T4, RETRO64_CP0_PAGEMASK));
	t_step(6);
	CHECK_EQ(R[T1], 0x00400042);  /* VPN2 enmascarado por la página de 16 KB */
	CHECK_EQ(R[T2], LO(0x20, 1, 1, 1));
	CHECK_EQ(R[T3], LO(0x21, 0, 1, 1));
	CHECK_EQ(R[T4], 0x6000);
}

static void tlb_probe_miss(void)
{
	R[T0] = 0x7FFFE000;
	T_LOAD(MTC0(T0, RETRO64_CP0_ENTRYHI), TLBP, MFC0(T1, RETRO64_CP0_INDEX));
	t_step(3);
	CHECK_EQ(R[T1], 0xFFFFFFFF80000000ull);
}

static void tlb_large_pages(void)
{
	/* Páginas de 16 KB: el par cubre 0x10000–0x17FFF. */
	tlb_set(1, 0x00010000, LO(0x40, 1, 1, 1), LO(0x50, 1, 1, 1), 0x6000);
	t_wr32(0x41234, 0x01010101);
	t_wr32(0x50010, 0x02020202);
	R[T0] = 0x00010000;
	T_LOAD(LW(T1, 0x1234, T0), LUI(T3, 1), ORI(T3, T3, 0x4010), LW(T2, 0, T3));
	t_step(4);
	CHECK_EQ(R[T1], 0x01010101);
	CHECK_EQ(R[T2], 0x02020202);
}

static void tlb_refill_on_miss(void)
{
	R[T0] = 0x00400120;
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ENTRYHI, 0x33);
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_TLBL, T_CODE, 0, T_VEC_TLB);
	CHECK_EQ(CPU->bad_vaddr, 0x00400120);
	CHECK_EQ(CPU->context & 0x7FFFF0, (0x00400120 >> 13) << 4);
	CHECK_EQ(CPU->entry_hi, 0x00400033);  /* VPN2 de la falla, ASID intacto */
}

static void tlb_refill_store(void)
{
	R[T0] = 0x00400120;
	T_LOAD(SW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_TLBS, T_CODE, 0, T_VEC_TLB);
}

static void tlb_miss_with_exl_uses_general_vector(void)
{
	t_set_status(T_STATUS | RETRO64_ST_EXL);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EQ(RETRO64_CAUSE_EXC(CPU->cause), RETRO64_EXC_TLBL);
	CHECK_EQ(CPU->pc, T_VEC_GEN);
}

static void tlb_invalid_entry(void)
{
	tlb_set(2, 0x00400000, LO(0x10, 1, 0, 1), LO(0x11, 1, 1, 1), 0);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_TLBL, T_CODE, 0, T_VEC_GEN);
}

static void tlb_modified(void)
{
	tlb_set(3, 0x00400000, LO(0x10, 0, 1, 1), LO(0x11, 1, 1, 1), 0);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0), SW(T1, 0, T0));
	t_step(2);
	CHECK_EXC(RETRO64_EXC_MOD, T_CODE + 4, 0, T_VEC_GEN);
}

static void tlb_asid_mismatch(void)
{
	tlb_set(4, 0x00400000 | 0x01, LO(0x10, 1, 1, 0), LO(0x11, 1, 1, 0), 0);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ENTRYHI, 0x02);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(RETRO64_EXC_TLBL, T_CODE, 0, T_VEC_TLB);
	t_set_status(T_STATUS);
	retro64_cpu_cop0_write(CPU, RETRO64_CP0_ENTRYHI, 0x01);
	retro64_cpu_set_pc(CPU, T_CODE);
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 4);
}

static void user_mode_cannot_fetch_kseg0(void)
{
	t_set_status((T_STATUS & ~RETRO64_ST_CU0) | KSU_USER);
	T_LOAD(NOP);
	t_step(1);
	CHECK_EXC(RETRO64_EXC_ADEL, T_CODE, 0, T_VEC_GEN);
}

static void user_mode_restrictions(void)
{
	/* Página 0 de kuseg → física 0x10000; ahí va el código de usuario. */
	tlb_set(0, 0, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	t_wr32(0x10000, MFC0(T0, RETRO64_CP0_STATUS));
	t_wr32(0x10004, DADDU(T1, T2, T3));
	R[T2] = 0x100000000ull;
	R[T3] = 1;

	t_set_status((T_STATUS & ~RETRO64_ST_CU0) | KSU_USER);
	retro64_cpu_set_pc(CPU, 0);
	t_step(1);
	CHECK_EXC(RETRO64_EXC_CPU, 0, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> RETRO64_CAUSE_CE_SHIFT) & 3, 0);

	t_set_status((T_STATUS & ~RETRO64_ST_CU0) | KSU_USER);
	retro64_cpu_set_pc(CPU, 4);
	t_step(1);
	CHECK_EXC(RETRO64_EXC_RI, 4, 0, T_VEC_GEN);  /* 64 bits sin UX */

	t_set_status((T_STATUS & ~RETRO64_ST_CU0) | KSU_USER | RETRO64_ST_UX);
	retro64_cpu_set_pc(CPU, 4);
	t_step(1);
	CHECK_EQ(R[T1], 0x100000001ull);
	CHECK_EQ(CPU->status & RETRO64_ST_EXL, 0);
}

static void random_in_range(void)
{
	R[T0] = 10;
	T_LOAD(MTC0(T0, RETRO64_CP0_WIRED), MFC0(T1, RETRO64_CP0_RANDOM));
	t_wr32(T_CODE_PA + 0x100, MFC0(T2, RETRO64_CP0_RANDOM));
	t_step(2);
	CHECK(R[T1] >= 10 && R[T1] <= 31);
	t_step(63);
	CHECK(R[T2] >= 10 && R[T2] <= 31);
	CHECK(R[T1] != R[T2] || R[T1] == 31);
}

static void unused_register_latch(void)
{
	R[T0] = 0x1234;
	T_LOAD(MTC0(T0, 7), MFC0(T1, 22));
	t_step(2);
	CHECK_EQ(R[T1], 0x1234);
}

void t_suite_cop0(void)
{
	RUN(prid_and_config);
	RUN(write_masks);
	RUN(mtc0_sign_extends_64bit_registers);
	RUN(syscall_and_eret);
	RUN(break_exception);
	RUN(bev_vectors);
	RUN(nested_exception_keeps_epc);
	RUN(reserved_instruction);
	RUN(cop2_unusable);
	RUN(traps);
	RUN(trap_immediate);
	RUN(misaligned_fetch);
	RUN(count_half_rate);
	RUN(count_write);
	RUN(compare_interrupt);
	RUN(software_interrupt);
	RUN(interrupt_blocked_by_exl_and_ie);
	RUN(mi_interrupt_on_ip2);
	RUN(eret_from_error_level);
	RUN(kuseg_unmapped_with_erl);
	RUN(tlb_mapped_access);
	RUN(tlb_read_back);
	RUN(tlb_probe_miss);
	RUN(tlb_large_pages);
	RUN(tlb_refill_on_miss);
	RUN(tlb_refill_store);
	RUN(tlb_miss_with_exl_uses_general_vector);
	RUN(tlb_invalid_entry);
	RUN(tlb_modified);
	RUN(tlb_asid_mismatch);
	RUN(user_mode_cannot_fetch_kseg0);
	RUN(user_mode_restrictions);
	RUN(random_in_range);
	RUN(unused_register_latch);
}
