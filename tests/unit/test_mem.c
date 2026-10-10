/*
 * Salsa64 — pruebas de cargas y almacenamientos: anchos, extensión de
 * signo, accesos no alineados (LWL/LWR/SWL/SWR/LDL/LDR/SDL/SDR), errores de
 * dirección y LL/SC.
 */
#include "cpu/rec.h"
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

/* ---- LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR contra un modelo byte a byte --------- */

/* Modelo independiente de interp.c: LWL y LDL llevan los bytes desde la
 * dirección hasta el final de la palabra (o doble palabra) a la parte alta
 * del registro; LWR y LDR, los del principio hasta la dirección a la parte
 * baja. LWL y LWR con la palabra completa extienden el signo; LWR parcial
 * deja los 32 bits altos. SWL, SWR, SDL y SDR escriben esos mismos bytes. */
static uint64_t un_state = 0x243F6A8885A308D3ull;

static uint64_t un_rand(void)
{
	un_state ^= un_state >> 12;
	un_state ^= un_state << 25;
	un_state ^= un_state >> 27;
	return un_state * 0x2545F4914F6CDD1Dull;
}

static uint8_t rd8(uint32_t pa)
{
	return (uint8_t)(t_rd32(pa & ~3u) >> (24 - 8 * (pa & 3)));
}

static bool un_left(unsigned op)
{
	return op == 0x22 || op == 0x1A || op == 0x2A || op == 0x2C;
}

static unsigned un_size(unsigned op)
{
	return (op == 0x22 || op == 0x26 || op == 0x2A || op == 0x2E) ? 4 : 8;
}

static uint64_t ref_load(unsigned op, uint64_t old, uint32_t pa)
{
	unsigned size = un_size(op), o = pa & (size - 1);
	uint64_t v = size == 4 ? (uint32_t)old : old;
	if (un_left(op)) {
		for (unsigned j = 0; j < size - o; j++) {
			unsigned sh = 8 * (size - 1 - j);
			v = (v & ~(0xFFull << sh)) | (uint64_t)rd8(pa + j) << sh;
		}
	} else {
		for (unsigned j = 0; j <= o; j++)
			v = (v & ~(0xFFull << 8 * j)) | (uint64_t)rd8(pa - j) << 8 * j;
	}
	if (size == 8)
		return v;
	if (un_left(op) || o == 3)
		return (uint64_t)(int64_t)(int32_t)(uint32_t)v;
	return (old & 0xFFFFFFFF00000000ull) | (uint32_t)v;
}

/* Escribe en mem (copia de la RDRAM desde `base`) lo que haría el
 * almacenamiento. */
static void ref_store(unsigned op, uint64_t v, uint32_t pa, uint8_t *mem, uint32_t base)
{
	unsigned size = un_size(op), o = pa & (size - 1);
	if (un_left(op))
		for (unsigned j = 0; j < size - o; j++)
			mem[pa + j - base] = (uint8_t)(v >> 8 * (size - 1 - j));
	else
		for (unsigned j = 0; j <= o; j++)
			mem[pa - j - base] = (uint8_t)(v >> 8 * j);
}

#define UN_BYTES 32

static void un_fill(uint8_t *mem)
{
	for (unsigned w = 0; w < UN_BYTES; w += 8)
		t_wr64(T_DATA_PA + w, un_rand());
	for (unsigned b = 0; b < UN_BYTES; b++)
		mem[b] = rd8(T_DATA_PA + b);
}

/* Llamadas del código generado al intérprete (0 sin recompilador). */
static uint64_t exec_calls(void)
{
	return CPU->rec ? CPU->rec->stats.exec_calls : 0;
}

static bool un_mem_equal(const uint8_t *mem)
{
	for (unsigned b = 0; b < UN_BYTES; b++)
		if (rd8(T_DATA_PA + b) != mem[b])
			return false;
	return true;
}

/* Cada instrucción sola, con cada desplazamiento dentro de la doble
 * palabra, y destino en distintos registros (A1 y V0 son fijos con
 * SALSA64_REC_OPT_PIN). */
static void unaligned_every_offset(void)
{
	static const unsigned ops[] = { 0x22, 0x26, 0x1A, 0x1B, 0x2A, 0x2E, 0x2C, 0x2D };
	static const unsigned rts[] = { T1, A1, V0 };
	uint8_t mem[UN_BYTES];
	uint64_t calls = exec_calls();

	for (unsigned n = 0; n < 8; n++) {
		for (unsigned off = 0; off < 16; off++) {
			unsigned op = ops[n], rt = rts[(n + off) % 3];
			bool store = op >= 0x28;
			int16_t imm = (int16_t)((int)off - 4);
			uint32_t pa = T_DATA_PA + 8 + (uint32_t)(int32_t)imm;
			uint64_t old = un_rand();

			un_fill(mem);
			R[T0] = T_DATA + 8;
			R[rt] = old;
			t_wr32(T_CODE_PA, M_I(op, T0, rt, imm));
			salsa64_cpu_set_pc(CPU, T_CODE);
			uint64_t want = old;
			if (store)
				ref_store(op, old, pa, mem, T_DATA_PA);
			else
				want = ref_load(op, old, pa);
			t_step(1);
			CHECK_EQ(R[rt], want);
			CHECK(un_mem_equal(mem));
			CHECK_EQ(CPU->pc, T_CODE + 4);
		}
	}
	CHECK_EQ(exec_calls(), calls);  /* todo en nativo */
}

/* LWL + LWR y SWL + SWR sobre los mismos 4 bytes, en los dos órdenes (el
 * recompilador los junta), y como referencia una pareja que no se junta
 * (otro registro). */
static void unaligned_pairs(void)
{
	uint8_t mem[UN_BYTES];
	uint64_t calls = exec_calls();

	for (unsigned off = 0; off < 8; off++) {
		for (unsigned form = 0; form < 6; form++) {
			int16_t imm = (int16_t)off;
			uint32_t pa = T_DATA_PA + 8 + off;
			uint64_t v = un_rand();
			bool store = form >= 3;
			unsigned l = store ? 0x2A : 0x22, r = store ? 0x2E : 0x26;
			unsigned rt2 = (form % 3) == 2 ? T2 : T1;  /* 2: la segunda va a otro registro */
			uint32_t first = M_I(l, T0, T1, imm), second = M_I(r, T0, rt2, imm + 3);
			if (form % 3 == 1) {  /* al revés */
				uint32_t t = first;
				first = second;
				second = t;
			}

			un_fill(mem);
			R[T0] = T_DATA + 8;
			R[T1] = v;
			R[T2] = ~v;
			t_wr32(T_CODE_PA, first);
			t_wr32(T_CODE_PA + 4, second);
			salsa64_cpu_set_pc(CPU, T_CODE);
			uint64_t want1 = v, want2 = ~v;
			if (store) {
				if (form % 3 == 1) {
					ref_store(r, rt2 == T1 ? v : ~v, pa + 3, mem, T_DATA_PA);
					ref_store(l, v, pa, mem, T_DATA_PA);
				} else {
					ref_store(l, v, pa, mem, T_DATA_PA);
					ref_store(r, rt2 == T1 ? v : ~v, pa + 3, mem, T_DATA_PA);
				}
			} else if (form % 3 == 1) {
				want1 = ref_load(l, ref_load(r, v, pa + 3), pa);
			} else if (rt2 == T1) {
				want1 = ref_load(r, ref_load(l, v, pa), pa + 3);
			} else {
				want1 = ref_load(l, v, pa);
				want2 = ref_load(r, ~v, pa + 3);
			}
			t_step(2);
			CHECK_EQ(R[T1], want1);
			CHECK_EQ(R[T2], want2);
			CHECK(un_mem_equal(mem));
			CHECK_EQ(CPU->pc, T_CODE + 8);
		}
	}
	CHECK_EQ(exec_calls(), calls);
}

/* Un bloque con más registros que los del PPE: doce usados tres veces y la
 * base seis se quedan con ellos, y los destinos del par y de las sueltas
 * van y vienen de cpu->gpr. */
static void unaligned_uncached_registers(void)
{
	static const unsigned fill[] = { S0, S1, S2, S3, S4, S5, S6, S7, T1, T2, T3, T4 };
	uint32_t code[48];
	unsigned n = 0;
	for (unsigned rep = 0; rep < 3; rep++)
		for (unsigned k = 0; k < 12; k++)
			code[n++] = ADDIU(fill[k], fill[k], 1);
	code[n++] = LWL(T8, 2, T0);
	code[n++] = LWR(T8, 5, T0);
	code[n++] = LWR(T9, 9, T0);
	code[n++] = SWL(T5, 0x11, T0);
	code[n++] = SWR(T5, 0x14, T0);
	code[n++] = SDR(T9, 0x1C, T0);
	t_load(code, n);

	t_wr64(T_DATA_PA, 0x0011223344556677ull);
	t_wr64(T_DATA_PA + 8, 0x8899AABBCCDDEEFFull);
	for (unsigned k = 0; k < 12; k++)
		R[fill[k]] = 0;
	R[T0] = T_DATA;
	R[T5] = 0x0123456789ABCDEFull;
	R[T8] = 0x5555555555555555ull;
	R[T9] = 0xAAAAAAAAAAAAAAAAull;
	t_step(n);
	CHECK_EQ(R[S7], 3);
	CHECK_EQ(R[T4], 3);
	CHECK_EQ(R[T8], 0x0000000022334455ull);
	CHECK_EQ(R[T9], 0xAAAAAAAAAAAA8899ull);
	CHECK_EQ(t_rd64(T_DATA_PA + 0x10), 0x0089ABCDEF000000ull);
	CHECK_EQ(t_rd64(T_DATA_PA + 0x18), 0xAAAAAA8899000000ull);
	CHECK_EQ(CPU->pc, T_CODE + 4 * n);
}

/* Fuera de la RDRAM (DMEM del RSP) y con fallo de TLB: el intérprete. */
static void unaligned_slow_paths(void)
{
	R[T0] = 0xFFFFFFFFA4000000ull;
	R[T1] = 0x11223344AABBCCDDull;
	T_LOAD(SWL(T1, 0x41, T0), SWR(T1, 0x44, T0), LWL(T2, 0x41, T0), LWR(T2, 0x44, T0),
	       LWR(T3, 0x41, T0));
	t_step(5);
	CHECK_EQ(R[T2], 0xFFFFFFFFAABBCCDDull);
	CHECK_EQ(R[T3] & 0xFFFF, 0x00AA);

	R[T0] = 0x00400120;  /* KUSEG sin entrada en la TLB */
	R[T4] = 0x77;
	T_LOAD(LWL(T4, 1, T0), LWR(T4, 4, T0));
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(2);  /* la segunda ya es la primera del vector */
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_TLBL);
	CHECK_EQ(CPU->epc, T_CODE);
	CHECK_EQ(CPU->pc, T_VEC_TLB + 4);
	CHECK_EQ(CPU->bad_vaddr, 0x00400121);
	CHECK_EQ(R[T4], 0x77);
}

/* Código que se reescribe: el par y una suelta escriben la instrucción que
 * viene después en el mismo bloque. */
static void unaligned_store_into_code(void)
{
	R[T0] = T_CODE;
	R[T1] = ADDIU(T2, ZERO, 7);
	R[T3] = ADDIU(T4, ZERO, 9);
	T_LOAD(SWL(T1, 12, T0), SWR(T1, 15, T0), SWL(T3, 16, T0), NOP, NOP);
	t_step(5);
	CHECK_EQ(R[T2], 7);
	CHECK_EQ(R[T4], 9);
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
	RUN(unaligned_every_offset);
	RUN(unaligned_pairs);
	RUN(unaligned_uncached_registers);
	RUN(unaligned_slow_paths);
	RUN(unaligned_store_into_code);
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
