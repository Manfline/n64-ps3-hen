/*
 * Salsa64 — pruebas del AI: cola de dos DMA, AI_STATUS y AI_LENGTH,
 * temporización por AI_DACRATE, interrupción al empezar cada DMA, el
 * acarreo de los límites de 8 KB y las muestras que llegan al sumidero.
 */
#include "test.h"

#include <string.h>

#define AI_REG(r)  (0x04500000u + 4 * (r))
#define MI_INTR    0x04300008u
#define NTSC_CLOCK 48681812ull
#define PAL_CLOCK  49656530ull
#define DACRATE    1520u  /* 32 006 Hz en NTSC */

static struct salsa64_bus *bus(void)
{
	return &t_n64.bus;
}

static uint32_t ai_rd(enum salsa64_ai_reg r)
{
	return salsa64_bus_read32(bus(), AI_REG(r));
}

static void ai_wr(enum salsa64_ai_reg r, uint32_t v)
{
	salsa64_bus_write32(bus(), AI_REG(r), v, 0xFFFFFFFFu);
}

static bool ai_irq(void)
{
	return (salsa64_bus_read32(bus(), MI_INTR) >> SALSA64_IRQ_AI) & 1;
}

static void spin(void)
{
	T_LOAD(BEQ(0, 0, 0xFFFF), NOP);
}

static void run(uint64_t cycles)
{
	salsa64_n64_run(&t_n64, cycles);
}

/* Ciclos que dura un DMA de `bytes` con DACRATE y el reloj NTSC. */
static uint64_t dma_cycles(uint32_t bytes, uint32_t dacrate, uint64_t clock)
{
	uint64_t num = (uint64_t)(bytes / 4) * 93750000ull * (dacrate + 1);
	return (num + clock - 1) / clock;
}

/* Sumidero de prueba: guarda cada entrega. */
#define SINK_MAX 8
static struct {
	unsigned n;
	uint32_t bytes[SINK_MAX], rate[SINK_MAX];
	uint64_t cycle[SINK_MAX];
	uint8_t first[SINK_MAX][8];
} sink;

static void sink_push(void *ctx, const uint8_t *p, uint32_t bytes, uint32_t rate, uint64_t cycle)
{
	(void)ctx;
	if (sink.n < SINK_MAX) {
		sink.bytes[sink.n] = bytes;
		sink.rate[sink.n] = rate;
		sink.cycle[sink.n] = cycle;
		memcpy(sink.first[sink.n], p, bytes < 8 ? bytes : 8);
	}
	sink.n++;
}

static void setup(void)
{
	spin();
	memset(&sink, 0, sizeof sink);
	salsa64_ai_set_sink(bus(), sink_push, NULL);
	ai_wr(SALSA64_AI_DACRATE, DACRATE);
	ai_wr(SALSA64_AI_BITRATE, 15);
	ai_wr(SALSA64_AI_CONTROL, 1);
}

static void enqueue(uint32_t addr, uint32_t len)
{
	ai_wr(SALSA64_AI_DRAM_ADDR, addr);
	ai_wr(SALSA64_AI_LEN, len);
}

static void status_and_mirrors(void)
{
	spin();
	CHECK_EQ(ai_rd(SALSA64_AI_STATUS), SALSA64_AI_STATUS_ONES);
	ai_wr(SALSA64_AI_CONTROL, 1);
	CHECK_EQ(ai_rd(SALSA64_AI_STATUS), SALSA64_AI_STATUS_ONES | SALSA64_AI_STATUS_ENABLED);
	ai_wr(SALSA64_AI_DACRATE, DACRATE);
	enqueue(0x10000, 0x800);
	/* Los registros de solo escritura se leen como AI_LENGTH. */
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0x800);
	CHECK_EQ(ai_rd(SALSA64_AI_DRAM_ADDR), 0x800);
	CHECK_EQ(ai_rd(SALSA64_AI_CONTROL), 0x800);
	CHECK_EQ(ai_rd(SALSA64_AI_DACRATE), 0x800);
	CHECK_EQ(ai_rd(SALSA64_AI_BITRATE), 0x800);
	CHECK_EQ(salsa64_ai_rate(bus()), 32006);
	/* Los 3 bits bajos de la dirección y del largo se ignoran. */
	ai_wr(SALSA64_AI_DRAM_ADDR, 0x20007);
	ai_wr(SALSA64_AI_LEN, 0x407);
	CHECK_EQ(bus()->ai.addr[1], 0x20000);
	CHECK_EQ(bus()->ai.len[1], 0x400);
}

static void queue_of_two_and_interrupts(void)
{
	setup();
	uint64_t t0 = t_n64.cpu.cycles;
	enqueue(0x10000, 0x800);
	CHECK(ai_irq());  /* al empezar, no al terminar */
	CHECK_EQ(ai_rd(SALSA64_AI_STATUS),
		 SALSA64_AI_STATUS_ONES | SALSA64_AI_STATUS_ENABLED | SALSA64_AI_STATUS_BUSY);
	ai_wr(SALSA64_AI_STATUS, 0);  /* cualquier escritura la baja */
	CHECK(!ai_irq());

	enqueue(0x20000, 0x400);
	CHECK(!ai_irq());
	CHECK_EQ(ai_rd(SALSA64_AI_STATUS), SALSA64_AI_STATUS_ONES | SALSA64_AI_STATUS_ENABLED |
						    SALSA64_AI_STATUS_BUSY | SALSA64_AI_STATUS_FULL);
	/* Con la cola llena, un tercero se ignora. */
	enqueue(0x30000, 0x200);
	CHECK_EQ(bus()->ai.count, 2);
	CHECK_EQ(bus()->ai.len[1], 0x400);
	CHECK_EQ(sink.n, 1);
	CHECK_EQ(sink.bytes[0], 0x800);
	CHECK_EQ(sink.rate[0], 32006);
	CHECK_EQ(sink.cycle[0], t0);

	/* Termina el primero: empieza el segundo con su interrupción. */
	uint64_t d1 = dma_cycles(0x800, DACRATE, NTSC_CLOCK);
	run(d1 - 1);
	CHECK(!ai_irq());
	CHECK_EQ(sink.n, 1);
	run(1);
	CHECK(ai_irq());
	CHECK_EQ(sink.n, 2);
	CHECK_EQ(sink.bytes[1], 0x400);
	CHECK_EQ(sink.cycle[1], t0 + d1);
	CHECK_EQ(ai_rd(SALSA64_AI_STATUS),
		 SALSA64_AI_STATUS_ONES | SALSA64_AI_STATUS_ENABLED | SALSA64_AI_STATUS_BUSY);
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0x400);
	ai_wr(SALSA64_AI_STATUS, 0);

	/* Termina el segundo: la cola queda vacía y no hay interrupción. */
	run(dma_cycles(0x400, DACRATE, NTSC_CLOCK));
	CHECK(!ai_irq());
	CHECK_EQ(ai_rd(SALSA64_AI_STATUS), SALSA64_AI_STATUS_ONES | SALSA64_AI_STATUS_ENABLED);
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0);
	CHECK_EQ(bus()->ai.dmas, 2);
}

static void length_counts_down(void)
{
	setup();
	enqueue(0x10000, 0x1000);  /* 1024 muestras */
	/* 100 muestras: 100 × 93,75 MHz × 1521 / 48,681812 MHz = 292 909,7 ciclos. */
	run(292909);
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0x1000 - 99 * 4);
	run(1);
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0x1000 - 100 * 4);
}

static void dacrate_and_region(void)
{
	setup();
	salsa64_vi_set_tv(bus(), SALSA64_TV_PAL);
	salsa64_vi_start(bus());
	CHECK_EQ(salsa64_ai_rate(bus()), 32647);  /* 49,656530 MHz / 1521 */
	enqueue(0x10000, 0x800);
	CHECK_EQ(sink.rate[0], 32647);
	uint64_t t0 = t_n64.cpu.cycles;
	run(dma_cycles(0x800, DACRATE, PAL_CLOCK) - 1);
	CHECK_EQ(bus()->ai.count, 1);
	run(1);
	CHECK_EQ(bus()->ai.count, 0);
	CHECK_EQ(t_n64.cpu.cycles - t0, dma_cycles(0x800, DACRATE, PAL_CLOCK));

	/* Cambiar AI_DACRATE a mitad de un DMA: lo que falta va a la nueva
	 * frecuencia (la mitad de rápido). */
	salsa64_vi_set_tv(bus(), SALSA64_TV_NTSC);
	salsa64_vi_start(bus());
	enqueue(0x10000, 0x800);
	run(dma_cycles(0x400, DACRATE, NTSC_CLOCK));
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0x400);
	ai_wr(SALSA64_AI_DACRATE, 2 * DACRATE + 1);
	uint64_t t1 = t_n64.cpu.cycles;
	while (bus()->ai.count)
		run(1000);
	uint64_t d = t_n64.cpu.cycles - t1, want = dma_cycles(0x400, 2 * DACRATE + 1, NTSC_CLOCK);
	CHECK(d >= want && d < want + 1000);
}

static void dma_enable_stops_progress(void)
{
	setup();
	ai_wr(SALSA64_AI_CONTROL, 0);
	enqueue(0x10000, 0x800);
	CHECK(ai_irq());       /* se encola y avisa aunque no avance */
	CHECK_EQ(sink.n, 0);   /* pero no suena */
	run(dma_cycles(0x800, DACRATE, NTSC_CLOCK) * 2);
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0x800);
	CHECK_EQ(ai_rd(SALSA64_AI_STATUS), SALSA64_AI_STATUS_ONES | SALSA64_AI_STATUS_BUSY);
	ai_wr(SALSA64_AI_CONTROL, 1);
	CHECK_EQ(sink.n, 1);
	run(dma_cycles(0x400, DACRATE, NTSC_CLOCK));
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0x400);
	/* Apagarlo a la mitad congela lo que falta. */
	ai_wr(SALSA64_AI_CONTROL, 0);
	run(dma_cycles(0x800, DACRATE, NTSC_CLOCK));
	CHECK_EQ(ai_rd(SALSA64_AI_LEN), 0x400);
	ai_wr(SALSA64_AI_CONTROL, 1);
	run(dma_cycles(0x400, DACRATE, NTSC_CLOCK));
	CHECK_EQ(bus()->ai.count, 0);
	CHECK_EQ(sink.n, 1);  /* las muestras se entregan una sola vez */
}

static void samples_reach_the_sink(void)
{
	setup();
	t_wr32(0x10000, 0x7FFF8000);  /* L = 32767, R = −32768 */
	t_wr32(0x10004, 0x00010002);
	enqueue(0x80010000, 0x800);   /* la dirección es física: 24 bits */
	CHECK_EQ(sink.n, 1);
	static const uint8_t want[8] = { 0x7F, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00, 0x02 };
	CHECK(memcmp(sink.first[0], want, 8) == 0);

	/* Fuera de la RDRAM (8 MB) se lee cero. */
	run(dma_cycles(0x800, DACRATE, NTSC_CLOCK));
	sink.n = 0;
	enqueue(0x7FFFF8, 0x10);
	CHECK_EQ(sink.n, 2);
	CHECK_EQ(sink.bytes[0], 8);
	CHECK_EQ(sink.bytes[1], 8);
	static const uint8_t zero[8];
	CHECK(memcmp(sink.first[1], zero, 8) == 0);
}

static void delayed_carry_at_8k(void)
{
	setup();
	t_wr32(0x12000, 0x11112222);  /* lo que pidió el programa */
	t_wr32(0x14000, 0x33334444);  /* lo que lee el AI: 0x2000 más */
	enqueue(0x11800, 0x800);      /* termina en 0x12000: límite de 8 KB */
	enqueue(0x12000, 0x100);
	run(dma_cycles(0x800, DACRATE, NTSC_CLOCK));
	CHECK_EQ(sink.n, 2);
	CHECK_EQ(sink.first[1][0], 0x33);
	CHECK_EQ(bus()->ai.addr[0], 0x14000);
	/* Este no termina en un límite: el siguiente se lee donde se pidió. */
	enqueue(0x12000, 0x100);
	run(dma_cycles(0x100, DACRATE, NTSC_CLOCK));
	CHECK_EQ(sink.n, 3);
	CHECK_EQ(sink.first[2][0], 0x11);
}

static void reset_empties_the_queue(void)
{
	setup();
	enqueue(0x10000, 0x800);
	enqueue(0x10800, 0x800);
	salsa64_n64_reset(&t_n64);
	spin();
	CHECK_EQ(bus()->ai.count, 0);
	CHECK_EQ(ai_rd(SALSA64_AI_STATUS), SALSA64_AI_STATUS_ONES);
	CHECK(bus()->ai.push == sink_push);  /* el sumidero sobrevive */
	CHECK_EQ(t_n64.sched.when[SALSA64_EV_AI_DMA], SALSA64_SCHED_NEVER);
}

void t_suite_ai(void)
{
	RUN(status_and_mirrors);
	RUN(queue_of_two_and_interrupts);
	RUN(length_counts_down);
	RUN(dacrate_and_region);
	RUN(dma_enable_stops_progress);
	RUN(samples_reach_the_sink);
	RUN(delayed_carry_at_8k);
	RUN(reset_empties_the_queue);
	salsa64_ai_set_sink(bus(), NULL, NULL);
}
