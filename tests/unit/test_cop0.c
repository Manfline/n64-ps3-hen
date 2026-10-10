/*
 * Salsa64 — pruebas de COP0: registros, excepciones, ERET, Count/Compare,
 * interrupciones, TLB y restricciones de modo usuario.
 */
#include "test.h"

#define KSU_USER 0x10u

#define tlb_set t_tlb_set
#define LO      T_LO

static void prid_and_config(void)
{
	T_LOAD(MFC0(T0, SALSA64_CP0_PRID), MFC0(T1, SALSA64_CP0_CONFIG));
	t_step(2);
	CHECK_EQ(R[T0], SALSA64_PRID_VALUE);
	CHECK_EQ(R[T1], 0x7006E463);
}

static void write_masks(void)
{
	R[T0] = 0xFFFFFFFF;
	R[T1] = 0xFFFFFFE0;  /* sin KSU, ERL, EXL, IE */
	T_LOAD(MTC0(T0, SALSA64_CP0_CAUSE), MFC0(T2, SALSA64_CP0_CAUSE),
	       MTC0(T1, SALSA64_CP0_STATUS), MFC0(T3, SALSA64_CP0_STATUS));
	t_step(4);
	CHECK_EQ(R[T2], 0x300);
	CHECK_EQ(R[T3], 0xFFFFFFFFFF57FFE0ull);
}

/* MTC0 transfiere los 64 bits del GPR, como DMTC0 (nemu64-test). */
static void mtc0_writes_64_bits(void)
{
	R[T0] = 0xFFFFFFFF80001234ull;
	R[T1] = 0x0000001234567890ull;
	T_LOAD(MTC0(T0, SALSA64_CP0_EPC), DMFC0(T2, SALSA64_CP0_EPC),
	       MTC0(T1, SALSA64_CP0_EPC), DMFC0(T3, SALSA64_CP0_EPC), MFC0(T4, SALSA64_CP0_EPC));
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
	CHECK_EXC(SALSA64_EXC_SYS, T_CODE, 0, T_VEC_GEN);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_EPC, T_CODE + 4);  /* el manejador salta el SYSCALL */
	t_step(2);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
	CHECK_EQ(R[T0], 1);
}

static void break_exception(void)
{
	T_LOAD(NOP, BREAK);
	t_step(2);
	CHECK_EXC(SALSA64_EXC_BP, T_CODE + 4, 0, T_VEC_GEN);
}

static void bev_vectors(void)
{
	t_set_status(T_STATUS | SALSA64_ST_BEV);
	T_LOAD(SYSCALL);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_SYS, T_CODE, 0, 0xFFFFFFFFBFC00380ull);
}

static void nested_exception_keeps_epc(void)
{
	t_set_status(T_STATUS | SALSA64_ST_EXL);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_EPC, 0x1234);
	T_LOAD(SYSCALL);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_SYS, 0x1234, 0, T_VEC_GEN);
}

static void reserved_instruction(void)
{
	T_LOAD(0x70000000u);  /* opcode 0x1C */
	t_step(1);
	CHECK_EXC(SALSA64_EXC_RI, T_CODE, 0, T_VEC_GEN);
}

static void cop2_unusable(void)
{
	T_LOAD(0x48000000u);  /* COP2 */
	t_step(1);
	CHECK_EXC(SALSA64_EXC_CPU, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 2);
}

static void traps(void)
{
	R[T0] = R[T1] = 5;
	T_LOAD(TNE(T0, T1), TGE(T0, T1));
	t_step(2);
	CHECK_EXC(SALSA64_EXC_TR, T_CODE + 4, 0, T_VEC_GEN);
}

static void trap_immediate(void)
{
	R[T0] = (uint64_t)-3;
	T_LOAD(TEQI(T0, -3));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TR, T_CODE, 0, T_VEC_GEN);
}

static void misaligned_fetch(void)
{
	R[T0] = T_CODE + 0x22;
	T_LOAD(JR(T0), NOP);
	t_step(3);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE + 0x22, 0, T_VEC_GEN);
	CHECK_EQ(CPU->bad_vaddr, T_CODE + 0x22);
}

static void count_half_rate(void)
{
	T_LOAD(MFC0(T0, SALSA64_CP0_COUNT));
	t_wr32(T_CODE_PA + 80, MFC0(T1, SALSA64_CP0_COUNT));  /* 20 instrucciones después */
	t_step(21);
	CHECK_EQ((uint32_t)(R[T1] - R[T0]), 10);
}

static void count_write(void)
{
	R[T0] = 0x12345;
	T_LOAD(MTC0(T0, SALSA64_CP0_COUNT), MFC0(T1, SALSA64_CP0_COUNT));
	t_step(2);
	CHECK(R[T1] - 0x12345 <= 1);
}

static void compare_interrupt(void)
{
	t_set_status(T_STATUS | SALSA64_ST_IE | 0x8000);  /* IM7 */
	T_LOAD(MFC0(T1, SALSA64_CP0_COUNT), ADDIU(T1, T1, 10), MTC0(T1, SALSA64_CP0_COMPARE));
	t_step(17);  /* 16 ciclos tras leer Count: todavía no */
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
	CHECK_EQ(CPU->cause & 0x8000, 0);

	unsigned k = 0;
	while (k < 10 && !(CPU->status & SALSA64_ST_EXL)) {
		t_step(1);
		k++;
	}
	CHECK(k >= 3 && k <= 5);
	CHECK(CPU->cause & 0x8000);
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_INT);
	CHECK_EQ(CPU->pc, T_VEC_GEN);

	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_COMPARE, 0);
	CHECK_EQ(CPU->cause & 0x8000, 0);
}

static void software_interrupt(void)
{
	t_set_status(T_STATUS | SALSA64_ST_IE | 0x100);  /* IM0 */
	R[T0] = 0x100;
	T_LOAD(MTC0(T0, SALSA64_CP0_CAUSE), NOP);
	t_step(2);
	CHECK_EXC(SALSA64_EXC_INT, T_CODE + 4, 0, T_VEC_GEN);
}

static void interrupt_blocked_by_exl_and_ie(void)
{
	R[T0] = 0x100;
	t_set_status(T_STATUS | SALSA64_ST_EXL | SALSA64_ST_IE | 0x100);
	T_LOAD(MTC0(T0, SALSA64_CP0_CAUSE), NOP, NOP);
	t_step(2);
	CHECK_EQ(CPU->pc, T_CODE + 8);
	t_set_status(T_STATUS | 0x100);  /* sin IE */
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 12);
}

static void mi_interrupt_on_ip2(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	salsa64_bus_write32(bus, 0x0430000C, 0x800, ~0u);  /* máscara DP */
	t_set_status(T_STATUS | SALSA64_ST_IE | 0x400);
	salsa64_mi_raise(bus, SALSA64_IRQ_DP);
	CHECK(CPU->cause & 0x400);
	T_LOAD(NOP);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_INT, T_CODE, 0, T_VEC_GEN);
	salsa64_bus_write32(bus, 0x04300000, 0x800, ~0u);  /* MI_MODE: limpiar DP */
	CHECK_EQ(CPU->cause & 0x400, 0);
}

static void eret_from_error_level(void)
{
	t_set_status(T_STATUS | SALSA64_ST_ERL);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ERROREPC, T_CODE + 0x20);
	T_LOAD(ERET);
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 0x20);
	CHECK_EQ(CPU->status & SALSA64_ST_ERL, 0);
}

static void kuseg_unmapped_with_erl(void)
{
	t_wr32(T_DATA_PA, 0xABCD1234);
	t_set_status(T_STATUS | SALSA64_ST_ERL);
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
	T_LOAD(MTC0(T0, SALSA64_CP0_ENTRYHI), MTC0(T1, SALSA64_CP0_ENTRYLO0),
	       MTC0(T2, SALSA64_CP0_ENTRYLO1), MTC0(ZERO, SALSA64_CP0_PAGEMASK),
	       MTC0(T3, SALSA64_CP0_INDEX), TLBWI,
	       LW(T4, 0x10, ZERO), LW(T5, 0x1010, ZERO), SW(T7, 0x14, ZERO),
	       TLBP, MFC0(T6, SALSA64_CP0_INDEX));
	t_step(11);
	CHECK_EQ(R[T4], 0xFFFFFFFFAAAA5555ull);
	CHECK_EQ(R[T5], 0x12345678);
	CHECK_EQ(t_rd32(0x10014), 0x77);
	CHECK_EQ(R[T6], 5);
}

static void tlb_read_back(void)
{
	tlb_set(7, 0x00402000 | 0x42, LO(0x20, 1, 1, 1), LO(0x21, 0, 1, 1), 0x6000);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYLO0, 0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_PAGEMASK, 0);
	R[T0] = 7;
	T_LOAD(MTC0(T0, SALSA64_CP0_INDEX), TLBR, MFC0(T1, SALSA64_CP0_ENTRYHI),
	       MFC0(T2, SALSA64_CP0_ENTRYLO0), MFC0(T3, SALSA64_CP0_ENTRYLO1),
	       MFC0(T4, SALSA64_CP0_PAGEMASK));
	t_step(6);
	CHECK_EQ(R[T1], 0x00400042);  /* VPN2 enmascarado por la página de 16 KB */
	CHECK_EQ(R[T2], LO(0x20, 1, 1, 1));
	CHECK_EQ(R[T3], LO(0x21, 0, 1, 1));
	CHECK_EQ(R[T4], 0x6000);
}

static void tlb_probe_miss(void)
{
	R[T0] = 0x7FFFE000;
	T_LOAD(MTC0(T0, SALSA64_CP0_ENTRYHI), TLBP, MFC0(T1, SALSA64_CP0_INDEX));
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
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 0x33);
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TLBL, T_CODE, 0, T_VEC_TLB);
	CHECK_EQ(CPU->bad_vaddr, 0x00400120);
	CHECK_EQ(CPU->context & 0x7FFFF0, (0x00400120 >> 13) << 4);
	CHECK_EQ(CPU->entry_hi, 0x00400033);  /* VPN2 de la falla, ASID intacto */
}

static void tlb_refill_store(void)
{
	R[T0] = 0x00400120;
	T_LOAD(SW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TLBS, T_CODE, 0, T_VEC_TLB);
}

static void tlb_miss_with_exl_uses_general_vector(void)
{
	t_set_status(T_STATUS | SALSA64_ST_EXL);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_TLBL);
	CHECK_EQ(CPU->pc, T_VEC_GEN);
}

static void tlb_invalid_entry(void)
{
	tlb_set(2, 0x00400000, LO(0x10, 1, 0, 1), LO(0x11, 1, 1, 1), 0);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TLBL, T_CODE, 0, T_VEC_GEN);
}

static void tlb_modified(void)
{
	tlb_set(3, 0x00400000, LO(0x10, 0, 1, 1), LO(0x11, 1, 1, 1), 0);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0), SW(T1, 0, T0));
	t_step(2);
	CHECK_EXC(SALSA64_EXC_MOD, T_CODE + 4, 0, T_VEC_GEN);
}

static void tlb_asid_mismatch(void)
{
	tlb_set(4, 0x00400000 | 0x01, LO(0x10, 1, 1, 0), LO(0x11, 1, 1, 0), 0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 0x02);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TLBL, T_CODE, 0, T_VEC_TLB);
	t_set_status(T_STATUS);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 0x01);
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 4);
}

/* ---- Código mapeado y en KSEG1 ----------------------------------------------
 * Con el recompilador, estos bloques van por dirección virtual y física
 * (rec.c): el mapeado revisa su traducción al entrar, también si llega por
 * un enlace. */

#define V_CODE 0x00400000u  /* kuseg; par de páginas de 4 KB: 0x00400000 y 0x00401000 */

/* En `pa`: T0 += inc y vuelve por RA. */
static void put_inc(uint32_t pa, int16_t inc)
{
	t_wr32(pa, ADDIU(T0, T0, inc));
	t_wr32(pa + 4, JR(RA));
	t_wr32(pa + 8, NOP);
}

static void tlb_exec_mapped_call(void)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	/* Página par: JAL a la impar; al volver, suma y regresa a KSEG0. */
	t_wr32(0x10000, ADDIU(T1, ZERO, 7));
	t_wr32(0x10004, JAL(V_CODE + 0x1100));
	t_wr32(0x10008, ADDIU(T0, ZERO, 5));
	t_wr32(0x1000C, ADDU(T2, T0, T1));
	t_wr32(0x10010, JR(T7));
	t_wr32(0x10014, NOP);
	t_wr32(0x11100, ADDIU(T0, T0, 1));
	t_wr32(0x11104, JR(RA));
	t_wr32(0x11108, ADDIU(T1, T1, 2));
	R[T7] = T_CODE + 12;
	T_LOAD(LUI(S0, V_CODE >> 16), JR(S0), NOP, ADDIU(T3, ZERO, 1));
	t_step(13);
	CHECK_EQ(R[T0], 6);
	CHECK_EQ(R[T1], 9);
	CHECK_EQ(R[T2], 15);
	CHECK_EQ(R[RA], V_CODE + 12);
	CHECK_EQ(R[T3], 1);
	CHECK_EQ(CPU->pc, T_CODE + 16);
}

/* Bucle mapeado que llama a una función mapeada: el JR de vuelta encuentra
 * el bloque en el código generado (exit_jr) desde la segunda vuelta. */
static void tlb_exec_mapped_loop(void)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	t_wr32(0x10000, JAL(V_CODE + 0x100));
	t_wr32(0x10004, ADDIU(T1, T1, 1));
	t_wr32(0x10008, J(V_CODE));
	t_wr32(0x1000C, NOP);
	put_inc(0x10100, 2);
	salsa64_cpu_set_pc(CPU, V_CODE);
	t_step(7 * 20);
	CHECK_EQ(R[T0], 40);
	CHECK_EQ(R[T1], 20);
	CHECK_EQ(R[RA], V_CODE + 8);
	CHECK_EQ(CPU->pc, V_CODE);
}

/* Bucle en KSEG0: llama a V_CODE, que salta (J, enlazable) a la página
 * impar, y después reescribe EntryLo1 con T5. */
static void load_remap_loop(void)
{
	t_wr32(0x10000, J(V_CODE + 0x1000));
	t_wr32(0x10004, NOP);
	R[S0] = V_CODE;
	T_LOAD(JALR(RA, S0), NOP, MTC0(T5, SALSA64_CP0_ENTRYLO1), TLBWI, J(T_CODE_PA), NOP);
}

static void tlb_exec_remap(void)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	put_inc(0x11000, 1);
	put_inc(0x21000, 100);
	load_remap_loop();
	R[T5] = LO(0x21, 1, 1, 1);
	t_step(11);
	CHECK_EQ(R[T0], 1);
	CHECK_EQ(CPU->pc, T_CODE);
	t_step(11);
	CHECK_EQ(R[T0], 101);  /* la página impar ahora es 0x21000 */
	t_step(11);
	CHECK_EQ(R[T0], 201);
	R[T5] = LO(0x11, 1, 1, 1);
	t_step(11);
	CHECK_EQ(R[T0], 301);
	t_step(11);
	CHECK_EQ(R[T0], 302);  /* de vuelta en 0x11000 */
	CHECK_EQ(CPU->pc, T_CODE);
}

/* Dos entradas para la misma página con ASID distinto: cambiar EntryHi
 * cambia el código que corre. */
static void tlb_exec_asid(void)
{
	tlb_set(11, V_CODE | 1, LO(0x10, 1, 1, 0), LO(0x11, 1, 1, 0), 0);
	tlb_set(12, V_CODE | 2, LO(0x20, 1, 1, 0), LO(0x21, 1, 1, 0), 0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 1);
	put_inc(0x10000, 1);
	put_inc(0x20000, 100);
	R[S0] = V_CODE;
	R[T5] = 2;
	T_LOAD(JALR(RA, S0), NOP, MTC0(T5, SALSA64_CP0_ENTRYHI), J(T_CODE_PA), NOP);
	t_step(8);
	CHECK_EQ(R[T0], 1);
	t_step(8);
	CHECK_EQ(R[T0], 101);
	R[T5] = 1;
	t_step(8);
	CHECK_EQ(R[T0], 201);
	t_step(8);
	CHECK_EQ(R[T0], 202);
	CHECK_EQ(CPU->pc, T_CODE);
}

/* El código mapeado se reescribe con un SW desde `base` (la dirección
 * mapeada o la de KSEG0) y la siguiente llamada corre la instrucción nueva. */
static void code_write_loop(uint64_t base)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	put_inc(0x10000, 1);
	salsa64_cpu_set_pc(CPU, T_CODE);
	R[T0] = 0;
	R[S0] = V_CODE;
	R[S1] = base;
	R[T6] = ADDIU(T0, T0, 100);
	T_LOAD(JALR(RA, S0), NOP, SW(T6, 0, S1), J(T_CODE_PA), NOP);
	t_step(8);
	CHECK_EQ(R[T0], 1);
	t_step(8);
	CHECK_EQ(R[T0], 101);
	R[T6] = ADDIU(T0, T0, 1000);
	t_step(8);
	CHECK_EQ(R[T0], 201);
	t_step(8);
	CHECK_EQ(R[T0], 1201);
}

static void tlb_exec_code_write(void)
{
	code_write_loop(V_CODE);
	code_write_loop(0xFFFFFFFF80010000ull);
}

static void exec_kseg1(void)
{
	t_wr32(0x10000, ADDIU(T0, ZERO, 3));
	t_wr32(0x10004, JAL(0x10100));
	t_wr32(0x10008, NOP);
	t_wr32(0x1000C, ADDIU(T1, T0, 1));
	t_wr32(0x10100, JR(RA));
	t_wr32(0x10104, ADDIU(T0, T0, 10));
	salsa64_cpu_set_pc(CPU, 0xFFFFFFFFA0010000ull);
	t_step(6);
	CHECK_EQ(R[T0], 13);
	CHECK_EQ(R[T1], 14);
	CHECK_EQ(R[RA], 0xFFFFFFFFA001000Cull);
	CHECK_EQ(CPU->pc, 0xFFFFFFFFA0010010ull);
}

/* Código en la ROM del cartucho, por la TLB y por KSEG1. */
static void exec_rom(void)
{
	static uint8_t rom[0x2000];
	static const uint32_t code[] = { ADDIU(T0, T0, 0x55), JR(RA), NOP };
	for (unsigned k = 0; k < 3; k++)
		for (unsigned b = 0; b < 4; b++)
			rom[0x1000 + 4 * k + b] = (uint8_t)(code[k] >> (24 - 8 * b));
	salsa64_bus_set_rom(&t_n64.bus, rom, sizeof rom);
	tlb_set(10, V_CODE, LO(0x10000, 0, 1, 1), LO(0x10001, 0, 1, 1), 0);
	R[S0] = V_CODE + 0x1000;
	R[S1] = 0xFFFFFFFFB0001000ull;
	T_LOAD(JALR(RA, S0), NOP, JALR(RA, S1), NOP, ADDIU(T1, T0, 1));
	t_step(11);
	CHECK_EQ(R[T0], 0xAA);
	CHECK_EQ(R[T1], 0xAB);
	CHECK_EQ(CPU->pc, T_CODE + 20);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

/* Dos páginas a 16 MB caen en la misma entrada de la TLB del recompilador:
 * se turnan, sin cambiar el resultado. */
static void tlb_exec_same_slot(void)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	tlb_set(13, V_CODE + 0x01000000, LO(0x30, 1, 1, 1), LO(0x31, 1, 1, 1), 0);
	t_wr32(0x10000, JAL(V_CODE + 0x01000000));
	t_wr32(0x10004, NOP);
	t_wr32(0x10008, J(V_CODE));
	t_wr32(0x1000C, NOP);
	put_inc(0x30000, 1);
	R[S0] = V_CODE;
	T_LOAD(JR(S0), NOP);
	t_step(2 + 7 * 5);
	CHECK_EQ(R[T0], 5);
	CHECK_EQ(CPU->pc, V_CODE);
}

static void user_mode_cannot_fetch_kseg0(void)
{
	t_set_status((T_STATUS & ~SALSA64_ST_CU0) | KSU_USER);
	T_LOAD(NOP);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE, 0, T_VEC_GEN);
}

static void user_mode_restrictions(void)
{
	/* Página 0 de kuseg → física 0x10000; ahí va el código de usuario. */
	tlb_set(0, 0, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	t_wr32(0x10000, MFC0(T0, SALSA64_CP0_STATUS));
	t_wr32(0x10004, DADDU(T1, T2, T3));
	R[T2] = 0x100000000ull;
	R[T3] = 1;

	t_set_status((T_STATUS & ~SALSA64_ST_CU0) | KSU_USER);
	salsa64_cpu_set_pc(CPU, 0);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_CPU, 0, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 0);

	t_set_status((T_STATUS & ~SALSA64_ST_CU0) | KSU_USER);
	salsa64_cpu_set_pc(CPU, 4);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_RI, 4, 0, T_VEC_GEN);  /* 64 bits sin UX */

	t_set_status((T_STATUS & ~SALSA64_ST_CU0) | KSU_USER | SALSA64_ST_UX);
	salsa64_cpu_set_pc(CPU, 4);
	t_step(1);
	CHECK_EQ(R[T1], 0x100000001ull);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
}

static void random_in_range(void)
{
	R[T0] = 10;
	T_LOAD(MTC0(T0, SALSA64_CP0_WIRED), MFC0(T1, SALSA64_CP0_RANDOM));
	t_wr32(T_CODE_PA + 0x100, MFC0(T2, SALSA64_CP0_RANDOM));
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

/* Sin COP2, con Status.CU2 los movimientos usan un latch de 64 bits; el resto
 * da RI con Cause.CE = 2 (nemu64-test, cop_unusable). */
static void cop2_latch(void)
{
	t_set_status(T_STATUS | SALSA64_ST_CU2);
	R[T0] = 0x0123456789ABCDEFull;
	T_LOAD(M_R(0x12, 0x04, T0, 5, 0, 0),   /* MTC2 */
	       M_R(0x12, 0x00, T1, 6, 0, 0),   /* MFC2 */
	       M_R(0x12, 0x01, T2, 7, 0, 0),   /* DMFC2 */
	       M_R(0x12, 0x03, T3, 7, 0, 0));  /* DCFC2: RI */
	t_step(4);
	CHECK_EQ(R[T1], 0xFFFFFFFF89ABCDEFull);
	CHECK_EQ(R[T2], 0x0123456789ABCDEFull);
	CHECK_EXC(SALSA64_EXC_RI, T_CODE + 12, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 2);

	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_CPU, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 2);
}

/* KX = 1: xkphys (0x9000...) llega a la memoria física; con bits 58–32
 * distintos de 0 es error de dirección. */
static void xkphys_with_kx(void)
{
	t_wr32(0x2000, 0xCAFEF00D);
	t_set_status(T_STATUS | SALSA64_ST_KX);
	R[T0] = 0x9000000000002000ull;
	R[T1] = 0x9000000100002000ull;
	T_LOAD(LW(T2, 0, T0), LW(T3, 0, T1));
	t_step(2);
	CHECK_EQ(R[T2], 0xFFFFFFFFCAFEF00Dull);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE + 4, 0, T_VEC_GEN);
	CHECK_EQ(CPU->bad_vaddr, 0x9000000100002000ull);
}

void t_suite_cop0(void)
{
	RUN(prid_and_config);
	RUN(write_masks);
	RUN(mtc0_writes_64_bits);
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
	RUN(tlb_exec_mapped_call);
	RUN(tlb_exec_mapped_loop);
	RUN(tlb_exec_remap);
	RUN(tlb_exec_asid);
	RUN(tlb_exec_code_write);
	RUN(exec_kseg1);
	RUN(exec_rom);
	RUN(tlb_exec_same_slot);
	RUN(user_mode_cannot_fetch_kseg0);
	RUN(user_mode_restrictions);
	RUN(random_in_range);
	RUN(unused_register_latch);
	RUN(cop2_latch);
	RUN(xkphys_with_kx);
}
