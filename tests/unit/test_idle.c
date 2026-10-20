/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas de la detección de bucles de espera (cpu/idle.c).
 *
 * Cada escenario corre dos veces desde el mismo ciclo, con la detección
 * apagada y encendida, y el estado final (ciclos, registros, EPC, Count,
 * RDRAM) tiene que ser idéntico: saltar un bucle de espera no puede cambiar
 * nada visible.
 */
#include "test.h"
#include "cpu/rec.h"

#include <string.h>

#define KSEG1(pa)     (0xFFFFFFFFA0000000ull | (pa))
#define MI_INTR_MASK  0x0430000Cu
#define VI_V_INTR     0x0440000Cu
#define NTSC_FIELD    1567041u
#define CODE(k)       (T_CODE_PA + 4 * (k))

struct snapshot {
	uint64_t cycles, pc, next_pc, epc;
	uint64_t gpr[32];
	uint32_t cause, status, count;
	uint8_t branch;
	uint8_t data[256];  /* RDRAM desde T_DATA_PA */
};

static void snapshot(struct snapshot *s)
{
	memset(s, 0, sizeof *s);
	s->cycles = CPU->cycles;
	s->pc = CPU->pc;
	s->next_pc = CPU->next_pc;
	s->branch = CPU->branch;
	s->epc = CPU->epc;
	memcpy(s->gpr, CPU->gpr, sizeof s->gpr);
	s->cause = CPU->cause;
	s->status = CPU->status;
	s->count = salsa64_cpu_count(CPU);
	memcpy(s->data, t_n64.bus.rdram + T_DATA_PA, sizeof s->data);
}

/* Máquina limpia en el ciclo 0 (como t_reset, pero con el mismo origen de
 * tiempo en las dos corridas: Count depende de la paridad del ciclo). */
static void restart(bool idle)
{
	CPU->cycles = 0;
	salsa64_n64_reset(&t_n64);
	salsa64_cpu_set_idle(CPU, idle);
	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE);
}

typedef void (*setup_fn)(void);

/* Corre `setup` + `cycles` sin y con detección; compara y devuelve los
 * ciclos saltados con la detección encendida. */
static uint64_t same_both_ways(setup_fn setup, uint64_t cycles)
{
	struct snapshot off, on;

	restart(false);
	setup();
	uint64_t idle0 = CPU->idle_cycles;
	salsa64_n64_run(&t_n64, cycles);
	snapshot(&off);
	CHECK_EQ(CPU->idle_cycles, idle0);

	restart(true);
	setup();
	idle0 = CPU->idle_cycles;
	salsa64_n64_run(&t_n64, cycles);
	snapshot(&on);

	CHECK_EQ(on.cycles, off.cycles);
	CHECK_EQ(on.pc, off.pc);
	CHECK_EQ(on.next_pc, off.next_pc);
	CHECK_EQ(on.branch, off.branch);
	CHECK_EQ(on.epc, off.epc);
	CHECK_EQ(on.cause, off.cause);
	CHECK_EQ(on.status, off.status);
	CHECK_EQ(on.count, off.count);
	for (int r = 0; r < 32; r++)
		CHECK_EQ(on.gpr[r], off.gpr[r]);
	CHECK(memcmp(on.data, off.data, sizeof on.data) == 0);
	return CPU->idle_cycles - idle0;
}

/* ¿Quedó el salto en `pa` en la tabla de descartados? */
static bool rejected(uint32_t pa)
{
	uint64_t pc = T_CODE + (pa - T_CODE_PA);
	return CPU->idle_reject[SALSA64_IDLE_SLOT(pc)] == ((uint32_t)pc ^ t_rd32(pa));
}

/* ---- Escenarios ---------------------------------------------------------- */

/* `b .` del hilo ocioso, sin interrupciones posibles. */
static void setup_self_loop(void)
{
	T_LOAD(BEQ(0, 0, 0xFFFF), NOP);
}

static void setup_j_self_loop(void)
{
	T_LOAD(NOP, J(T_CODE_PA + 4), NOP);
}

/* Manejador de la interrupción del VI en 0x80000180: la baja, cuenta el
 * campo en T_DATA y guarda Count, como el de vitest. */
static void load_vi_handler(void)
{
	static const uint32_t h[] = {
		LUI(26, 0xA440), SW(0, 0x10, 26),
		LUI(26, 0x8000), LW(27, 0x2000, 26), ADDIU(27, 27, 1), SW(27, 0x2000, 26),
		MFC0(27, 9), SW(27, 0x2004, 26),
		ERET, NOP,
	};
	for (unsigned k = 0; k < sizeof h / sizeof h[0]; k++)
		t_wr32(0x180 + 4 * k, h[k]);
	salsa64_bus_write32(&t_n64.bus, MI_INTR_MASK, 0x80, 0xFFFFFFFFu);
	salsa64_bus_write32(&t_n64.bus, VI_V_INTR, 2, 0xFFFFFFFFu);
	t_set_status(T_STATUS | SALSA64_ST_IE | (1u << (8 + 2)));
}

/* El bucle de vitest: espera a que el manejador cambie el contador. */
static void setup_vi_wait(void)
{
	load_vi_handler();
	T_LOAD(LUI(16, 0x8000),
	       LW(8, 0x2000, 16),        /* visto */
	       LW(9, 0x2000, 16),        /* espera: */
	       BEQ(9, 8, 0xFFFE),
	       NOP,
	       ADDIU(17, 17, 1),         /* cuadros */
	       BEQ(0, 0, 0xFFFA),        /* al LW de "visto" */
	       NOP);
}

/* Contador: estado que cambia en cada vuelta, nunca es de espera. */
static void setup_counter(void)
{
	R[8] = 5000;
	T_LOAD(ADDIU(8, 8, 0xFFFF), BNE(8, 0, 0xFFFE), NOP,
	       MFC0(9, 9), BEQ(0, 0, 0xFFFF), NOP);
}

/* Sondeo de VI_V_CURRENT (E/S que cambia con el tiempo): no se salta y
 * sale en el mismo ciclo. */
static void setup_mmio_poll(void)
{
	R[10] = 40;
	T_LOAD(LUI(9, 0xA440), LW(8, 0x10, 9), BNE(8, 10, 0xFFFE), NOP,
	       LUI(16, 0x8000), MFC0(11, 9), SW(11, 0x2000, 16),
	       BEQ(0, 0, 0xFFFF), NOP);
}

/* El delay slot escribe un registro que lee el cuerpo: la primera vuelta
 * no es punto fijo, la segunda sí. */
static void setup_delay_slot_write(void)
{
	R[16] = T_DATA;
	R[10] = T_DATA;
	T_LOAD(LW(9, 0, 10), BEQ(9, 0, 0xFFFE), ADDIU(10, 16, 0x10));
}

/* Un contador en la RDRAM: los registros vuelven a lo mismo (t0 = 0 al
 * final de cada vuelta) pero la memoria no. */
static void setup_store(void)
{
	R[16] = T_DATA;
	T_LOAD(LW(8, 0x20, 16), ADDIU(8, 8, 1), SW(8, 0x20, 16), BEQ(0, 0, 0xFFFC), ADDU(8, 0, 0));
}

/* Un almacenamiento que escribe siempre lo mismo: desde la segunda vuelta
 * la RDRAM ya tiene ese valor y es punto fijo. */
static void setup_silent_store(void)
{
	R[16] = T_DATA;
	R[9] = 5;
	T_LOAD(SW(9, 0x20, 16), BEQ(0, 0, 0xFFFE), NOP);
}

/* El hilo ocioso de `for (;;) osYieldThread();` en miniatura: deshabilita
 * interrupciones, guarda contexto, lee y escribe MI_INTR_MASK, pone EPC y
 * EXL y vuelve con ERET (que deja IE como estaba). Entre vuelta y vuelta
 * entran las interrupciones del VI, con el manejador de vitest. */
static void setup_yield_loop(void)
{
	load_vi_handler();
	R[20] = T_DATA;
	T_LOAD(JAL(T_CODE_PA + 0x20), NOP,       /* bucle: osYieldThread */
	       BEQ(0, 0, 0xFFFD), NOP,
	       NOP, NOP, NOP, NOP,
	       MFC0(8, 12), ADDIU(1, 0, 0xFFFE), AND(9, 8, 1), MTC0(9, 12),
	       SD(16, 0x30, 20), SD(31, 0x38, 20),
	       LUI(27, 0xA430), LW(26, 0xC, 27), SW(26, 0x40, 20),
	       ADDIU(26, 0, 0x80), SW(26, 0xC, 27),  /* poner la del VI: la misma máscara */
	       MTC0(31, 14), ORI(8, 8, 2), MTC0(8, 12),
	       ERET);
}

/* Lo mismo con un hilo que usa la FPU: guarda y repone FCR31 y un par de
 * registros (CFC1, SDC1, LDC1, CTC1). */
static void setup_yield_fpu_loop(void)
{
	load_vi_handler();
	R[20] = T_DATA;
	T_LOAD(JAL(T_CODE_PA + 0x20), NOP,
	       BEQ(0, 0, 0xFFFD), NOP,
	       NOP, NOP, NOP, NOP,
	       MFC0(8, 12), ADDIU(1, 0, 0xFFFE), AND(9, 8, 1), MTC0(9, 12),
	       CFC1(10, 31), SW(10, 0x48, 20), SDC1(20, 0x50, 20), SD(31, 0x38, 20),
	       LDC1(20, 0x50, 20), LW(10, 0x48, 20), CTC1(10, 31),
	       MTC0(31, 14), ORI(8, 8, 2), MTC0(8, 12),
	       ERET);
}

/* Un bucle de 40 instrucciones que vuelve con J (más de las 8 de antes). */
static void setup_long_loop(void)
{
	static uint32_t code[40];
	for (unsigned k = 0; k < 38; k++)
		code[k] = NOP;
	code[38] = J(T_CODE_PA);
	code[39] = NOP;
	t_load(code, 40);
}

/* Una espera que, después de cada interrupción, pasa una vez por algo que
 * no se simula (VI_V_CURRENT) y vuelve a esperar: no se descarta. */
static void setup_poll_after_irq(void)
{
	load_vi_handler();
	R[20] = T_DATA;
	T_LOAD(LW(8, 0, 20),                 /* campos atendidos */
	       BEQ(8, 17, 4), NOP,           /* nada nuevo: a esperar */
	       LUI(9, 0xA440), LW(10, 0x10, 9),
	       ADDU(17, 8, 0),               /* visto */
	       BEQ(0, 0, 0xFFF9), NOP);
}

/* ---- Pruebas --------------------------------------------------------------- */

static void idle_self_loop(void)
{
	uint64_t skipped = same_both_ways(setup_self_loop, 1000000);
	CHECK(skipped >= 999000);
	skipped = same_both_ways(setup_j_self_loop, 1000001);
	CHECK(skipped >= 999000);
}

static void idle_vi_wait_exact(void)
{
	uint64_t skipped = same_both_ways(setup_vi_wait, 3 * NTSC_FIELD + 1234);
	CHECK(skipped > 3 * NTSC_FIELD - 1000);
	CHECK_EQ(t_rd32(T_DATA_PA), 3);  /* campos atendidos */
	CHECK_EQ(R[17], 3);              /* vueltas del bucle principal */
	CHECK(rejected(CODE(6)));        /* el salto largo con otro salto adentro */
}

static void idle_counter_not_skipped(void)
{
	uint64_t skipped = same_both_ways(setup_counter, 20000);
	CHECK_EQ(R[8], 0);
	CHECK(rejected(CODE(1)));
	CHECK(skipped > 0);  /* el `b .` de después sí */
}

static void idle_mmio_poll_not_skipped(void)
{
	uint64_t skipped = same_both_ways(setup_mmio_poll, NTSC_FIELD / 4);
	CHECK(rejected(CODE(2)));
	CHECK(t_rd32(T_DATA_PA) != 0);
	CHECK(skipped > 0);
}

static void idle_delay_slot_write(void)
{
	uint64_t skipped = same_both_ways(setup_delay_slot_write, 100000);
	CHECK(skipped > 99000);
	CHECK_EQ(R[10], T_DATA + 0x10);
	CHECK(!rejected(CODE(1)));
}

static void idle_store_not_skipped(void)
{
	uint64_t skipped = same_both_ways(setup_store, 1000);
	CHECK_EQ(skipped, 0);
	CHECK(rejected(CODE(3)));
	CHECK(t_rd32(T_DATA_PA + 0x20) > 100);
}

static void idle_silent_store(void)
{
	uint64_t skipped = same_both_ways(setup_silent_store, 100000);
	CHECK(skipped > 99000);
	CHECK_EQ(t_rd32(T_DATA_PA + 0x20), 5);
	CHECK(!rejected(CODE(1)));
}

static void idle_yield_loop_exact(void)
{
	uint64_t skipped = same_both_ways(setup_yield_loop, 3 * NTSC_FIELD + 4321);
	CHECK(skipped > 3 * NTSC_FIELD - 5000);
	CHECK_EQ(t_rd32(T_DATA_PA), 3);  /* campos atendidos */
	CHECK(!rejected(CODE(2)));
	CHECK_EQ(t_rd32(T_DATA_PA + 0x3C), (uint32_t)(T_CODE + 8));  /* ra guardado */
}

/* `b .` en KSEG1: el intérprete lo analiza igual que en KSEG0. */
static void setup_kseg1_self_loop(void)
{
	T_LOAD(BEQ(0, 0, 0xFFFF), NOP);
	salsa64_cpu_set_pc(CPU, KSEG1(T_CODE_PA));
}

/* Código mapeado por la TLB (el hilo ocioso de GoldenEye): página par de
 * V_LOOP en la física 0x10000, impar (datos) en 0x11000. */
#define V_LOOP 0x00400000u

static void map_loop_page(uint32_t even_pfn)
{
	t_tlb_set(10, V_LOOP, T_LO(even_pfn, 1, 1, 1), T_LO(0x11, 1, 1, 1), 0);
}

/* `b .` mapeado. */
static void setup_mapped_self_loop(void)
{
	map_loop_page(0x10);
	t_wr32(0x10000, BEQ(0, 0, 0xFFFF));
	t_wr32(0x10004, NOP);
	salsa64_cpu_set_pc(CPU, V_LOOP);
}

/* Sondeo de una variable leída por la dirección mapeada (página impar). */
static void setup_mapped_poll(void)
{
	map_loop_page(0x10);
	t_wr32(0x10000, LW(8, 0x100, 16));
	t_wr32(0x10004, BEQ(8, 0, 0xFFFE));
	t_wr32(0x10008, NOP);
	t_wr32(0x11100, 0);
	R[16] = V_LOOP + 0x1000;
	salsa64_cpu_set_pc(CPU, V_LOOP);
}

/* El mismo salto en la misma dirección virtual, pero la TLB pasa la página a
 * otra física donde el delay slot cuenta: el análisis ve la página nueva y
 * deja de saltar. */
static void setup_mapped_remap(void)
{
	setup_mapped_self_loop();
	t_wr32(0x20000, BEQ(0, 0, 0xFFFF));
	t_wr32(0x20004, ADDIU(8, 8, 1));
	salsa64_n64_run(&t_n64, 50000);
	map_loop_page(0x20);
	salsa64_cpu_set_pc(CPU, V_LOOP);
}

static void idle_other_segments(void)
{
	uint64_t skipped = same_both_ways(setup_kseg1_self_loop, 1000000);
	CHECK(skipped >= 999000);
	skipped = same_both_ways(setup_mapped_self_loop, 1000000);
	CHECK(skipped >= 999000);
	CHECK_EQ(CPU->pc & ~4ull, V_LOOP);
	skipped = same_both_ways(setup_mapped_poll, 1000000);
	CHECK(skipped >= 999000);
	CHECK_EQ(R[8], 0);
}

static void idle_mapped_remap(void)
{
	uint64_t skipped = same_both_ways(setup_mapped_remap, 20000);
	CHECK_EQ(skipped, 0);
	CHECK(R[8] >= 9990 && R[8] <= 10000);  /* una vuelta (2 instrucciones) por cuenta */
	CHECK_EQ(CPU->idle_reject[SALSA64_IDLE_SLOT(V_LOOP)], V_LOOP ^ BEQ(0, 0, 0xFFFF));
}

/* Apagar y prender la detección vacía las tablas. */
static void idle_toggle(void)
{
	restart(false);
	setup_self_loop();
	uint64_t idle0 = CPU->idle_cycles;
	salsa64_n64_run(&t_n64, 100);
	CHECK(rejected(CODE(0)));
	CHECK_EQ(CPU->idle_cycles, idle0);
	salsa64_cpu_set_idle(CPU, true);
	CHECK(!rejected(CODE(0)));
	salsa64_n64_run(&t_n64, 100000);
	CHECK(CPU->idle_cycles - idle0 > 99000);
}

static void idle_yield_fpu_and_long_loops(void)
{
	uint64_t skipped = same_both_ways(setup_yield_fpu_loop, 2 * NTSC_FIELD + 777);
	CHECK(skipped > 2 * NTSC_FIELD - 5000);
	CHECK_EQ(t_rd32(T_DATA_PA), 2);
	skipped = same_both_ways(setup_long_loop, 100000);
	CHECK(skipped > 99000);
	CHECK(!rejected(CODE(38)));
}

static void idle_poll_after_irq(void)
{
	uint64_t skipped = same_both_ways(setup_poll_after_irq, 3 * NTSC_FIELD + 99);
	CHECK(skipped > 3 * NTSC_FIELD - 5000);
	CHECK_EQ(R[17], 3);
	CHECK(!rejected(CODE(6)));
}

/* Descartar una consulta conserva la traducción y su delay slot. Una
 * escritura real posterior tiene que invalidarla y ejecutar el código nuevo. */
static void idle_patch_keeps_code(void)
{
	for (unsigned likely = 0; likely < 2; likely++) {
		restart(true);
		uint32_t branch = BNE(T0, 0, -2) | (likely ? 0x40000000u : 0);
		uint32_t code[] = { ADDIU(T0, 0, 100), ADDIU(T0, T0, -1), branch, ADDIU(T1, T1, 1), J(CODE(4)), NOP };
		t_load(code, sizeof code / sizeof code[0]);
		R[T1] = 0;
		struct salsa64_rec *rec = CPU->rec;
		uint64_t killed = rec ? rec->stats.invalidated : 0, patched = rec ? rec->diag.idle_patched : 0;
		salsa64_n64_run(&t_n64, 500);
		CHECK_EQ(R[T0], 0);
		CHECK_EQ(R[T1], likely ? 99 : 100);
		if (rec && !(rec->opts & SALSA64_REC_OPT_IDLE_RECOMPILE)) {
			CHECK(rec->diag.idle_patched > patched);
			CHECK_EQ(rec->stats.invalidated, killed);
		}
		t_wr32(CODE(1), ADDIU(T0, T0, -2));
		R[T0] = 14;
		R[T1] = 0;
		salsa64_cpu_set_pc(CPU, T_CODE + 4);
		salsa64_n64_run(&t_n64, 100);
		CHECK_EQ(R[T0], 0);
		CHECK_EQ(R[T1], likely ? 6 : 7);
	}
}

static void setup_rejected_jump(void)
{
	T_LOAD(ADDIU(T0, T0, 1), J(CODE(0)), ADDIU(T1, T1, 1));
}

static void idle_patch_jump_exact(void)
{
	CHECK_EQ(same_both_ways(setup_rejected_jump, 1001), 0);
}

/* Dos saltos distintos comparten una ranura del detector. El descarte
 * de uno no debe obligar a retraducir el otro en cada vuelta. */
static void idle_patch_colliding_rejections(void)
{
	restart(true);
	const uint32_t a = 0x3AE0Cu, b = 0x3B824u;
	CHECK_EQ(SALSA64_IDLE_SLOT(0x80000000u | a), SALSA64_IDLE_SLOT(0x80000000u | b));
	uint32_t sub[] = { ADDIU(T0, 0, 3), ADDIU(T0, T0, -1), BNE(T0, 0, -2), NOP, JR(RA), NOP };
	for (unsigned k = 0; k < sizeof sub / sizeof sub[0]; k++) {
		t_wr32(a - 8 + 4 * k, sub[k]);
		t_wr32(b - 8 + 4 * k, sub[k]);
	}
	T_LOAD(JAL(0x3AE04u), NOP, JAL(0x3B81Cu), NOP, J(T_CODE_PA), ADDIU(T2, T2, 1));
	R[T2] = 0;
	struct salsa64_rec *rec = CPU->rec;
	uint64_t compiled = rec ? rec->stats.compiled : 0;
	salsa64_n64_run(&t_n64, 3000);
	CHECK_EQ(R[T2], 100);
	CHECK_EQ(R[T0], 0);
	CHECK_EQ(CPU->pc, T_CODE);
	if (rec && !(rec->opts & SALSA64_REC_OPT_IDLE_RECOMPILE))
		CHECK(rec->stats.compiled - compiled < 32);
}

void t_suite_idle(void)
{
	RUN(idle_patch_colliding_rejections);
	RUN(idle_patch_keeps_code);
	RUN(idle_patch_jump_exact);
	RUN(idle_self_loop);
	RUN(idle_vi_wait_exact);
	RUN(idle_counter_not_skipped);
	RUN(idle_mmio_poll_not_skipped);
	RUN(idle_delay_slot_write);
	RUN(idle_store_not_skipped);
	RUN(idle_silent_store);
	RUN(idle_yield_loop_exact);
	RUN(idle_yield_fpu_and_long_loops);
	RUN(idle_poll_after_irq);
	RUN(idle_toggle);
	RUN(idle_other_segments);
	RUN(idle_mapped_remap);
}
