/*
 * Salsa64 — pruebas de la detección de bucles de espera (cpu/idle.c).
 *
 * Cada escenario corre dos veces desde el mismo ciclo, con la detección
 * apagada y encendida, y el estado final (ciclos, registros, EPC, Count,
 * RDRAM) tiene que ser idéntico: saltar un bucle de espera no puede cambiar
 * nada visible.
 */
#include "test.h"

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
	return CPU->idle_reject[(pc >> 2) & (SALSA64_IDLE_CACHE - 1)] == ((uint32_t)pc ^ t_rd32(pa));
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

/* Un almacenamiento en el cuerpo: se descarta al primer intento. */
static void setup_store(void)
{
	R[16] = T_DATA;
	T_LOAD(SW(0, 0x20, 16), BEQ(0, 0, 0xFFFE), NOP);
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
	CHECK(rejected(CODE(1)));
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

void t_suite_idle(void)
{
	RUN(idle_self_loop);
	RUN(idle_vi_wait_exact);
	RUN(idle_counter_not_skipped);
	RUN(idle_mmio_poll_not_skipped);
	RUN(idle_delay_slot_write);
	RUN(idle_store_not_skipped);
	RUN(idle_toggle);
}
