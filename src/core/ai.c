/*
 * Salsa64 — Audio Interface (AI).
 *
 * El AI lee por DMA muestras de 16 bits estéreo de la RDRAM al ritmo del
 * DAC y no les hace nada (n64brew.dev: Audio Interface):
 *
 *   frecuencia  reloj del VI / (AI_DACRATE + 1): 32 006 Hz con 1520 en NTSC
 *   cola        dos lugares: el DMA que suena y uno más. Escribir AI_LENGTH
 *               encola la dirección de AI_DRAM_ADDR; con la cola llena se
 *               ignora. AI_STATUS: BUSY (suena uno), FULL (hay otro).
 *   interrupción al EMPEZAR cada DMA, no al terminar: avisa que ya hay
 *               lugar para el siguiente búfer
 *   AI_LENGTH   se lee lo que falta del DMA en curso; los demás registros,
 *               salvo AI_STATUS, se leen como AI_LENGTH
 *   AI_CONTROL  bit 0: sin él el DMA no avanza
 *   acarreo     si un DMA termina justo en un límite de 8 KB, el siguiente
 *               se lee 0x2000 bytes más adelante (error del hardware que
 *               libultra compensa restando 0x2000; hay que imitarlo)
 *
 * No se simula muestra por muestra: el DMA en curso tiene un evento del
 * planificador en el ciclo en que termina, y lo que falta se calcula al
 * leer AI_LENGTH. Las muestras de cada DMA se entregan al sumidero cuando
 * empieza a sonar (el juego ya no las toca: libultra rota tres búferes).
 * Todo en enteros para que el host y la PS3 den los mismos ciclos.
 */
#include <string.h>

#include "core/bus.h"
#include "core/sched.h"
#include "cpu/vr4300.h"

#define VR4300_HZ  93750000ull
#define NTSC_CLOCK 48681812u
#define ADDR_MASK  0xFFFFF8u   /* 24 bits, alineada a 8 */
#define LEN_MASK   0x3FFF8u    /* 18 bits, múltiplo de 8 */

static uint64_t now(const struct salsa64_bus *bus)
{
	return bus->cpu ? bus->cpu->cycles : 0;
}

static uint32_t vi_clock(const struct salsa64_bus *bus)
{
	return bus->vi.clock_hz ? bus->vi.clock_hz : NTSC_CLOCK;
}

uint32_t salsa64_ai_rate(const struct salsa64_bus *bus)
{
	uint32_t div = (bus->ai.regs[SALSA64_AI_DACRATE] & 0x3FFF) + 1;
	return (vi_clock(bus) + div / 2) / div;
}

/* Ciclos del VR4300 que tardan `bytes` (4 por muestra estéreo), hacia arriba. */
static uint64_t bytes_to_cycles(const struct salsa64_bus *bus, uint32_t bytes)
{
	uint64_t num = (uint64_t)(bytes / 4) * VR4300_HZ * ((bus->ai.regs[SALSA64_AI_DACRATE] & 0x3FFF) + 1);
	uint64_t den = vi_clock(bus);
	return (num + den - 1) / den;
}

/* Muestras completas que sonaron en `cycles` ciclos. */
static uint64_t cycles_to_samples(const struct salsa64_bus *bus, uint64_t cycles)
{
	uint64_t div = (bus->ai.regs[SALSA64_AI_DACRATE] & 0x3FFF) + 1;
	return cycles * vi_clock(bus) / (VR4300_HZ * div);
}

static bool running(const struct salsa64_ai *ai)
{
	return ai->count && (ai->regs[SALSA64_AI_CONTROL] & 1);
}

/* Bytes que faltan del DMA en curso en el ciclo `t`. */
static uint32_t left_at(const struct salsa64_bus *bus, uint64_t t)
{
	const struct salsa64_ai *ai = &bus->ai;
	if (!ai->count)
		return 0;
	if (!running(ai) || t <= ai->mark)
		return ai->left;
	uint64_t done = cycles_to_samples(bus, t - ai->mark) * 4;
	return done >= ai->left ? 0 : ai->left - (uint32_t)done;
}

/* Lleva la cuenta al ciclo actual (antes de cambiar la frecuencia o el
 * permiso del DMA). */
static void settle(struct salsa64_bus *bus)
{
	struct salsa64_ai *ai = &bus->ai;
	uint64_t t = now(bus);
	ai->left = left_at(bus, t);
	ai->mark = t;
}

/* Entrega las muestras de [addr, addr + len) con la vuelta de 24 bits y
 * ceros fuera de la RDRAM. */
static void deliver(struct salsa64_bus *bus, uint32_t addr, uint32_t len)
{
	static const uint8_t zeros[4096];
	struct salsa64_ai *ai = &bus->ai;
	uint32_t rate = salsa64_ai_rate(bus);
	uint64_t t = now(bus);
	while (len) {
		uint32_t n = len;
		const uint8_t *p;
		if (addr < bus->rdram_size) {
			if (n > bus->rdram_size - addr)
				n = bus->rdram_size - addr;
			p = bus->rdram + addr;
		} else {
			uint32_t to_wrap = 0x1000000u - addr;
			n = n < sizeof zeros ? n : sizeof zeros;
			n = n < to_wrap ? n : to_wrap;
			p = zeros;
		}
		ai->push(ai->push_ctx, p, n, rate, t);
		addr = (addr + n) & 0xFFFFFF;
		len -= n;
	}
}

/* El DMA de [0] empieza a sonar (o vuelve a avanzar): la primera vez se
 * aplica el acarreo pendiente y se entregan sus muestras. */
static void begin(struct salsa64_bus *bus)
{
	struct salsa64_ai *ai = &bus->ai;
	if (!running(ai))
		return;
	if (!ai->started) {
		ai->started = true;
		if (ai->carry && ai->len[0]) {
			/* Solo la mitad alta (bits 13–23) recibe el acarreo. */
			ai->addr[0] = (ai->addr[0] & 0x1FFF) | ((ai->addr[0] + 0x2000) & 0xFFE000);
			ai->carry = false;
		}
		if (ai->len[0]) {
			ai->carry = ((ai->addr[0] + ai->len[0]) & 0x1FFF) == 0;
			ai->dmas++;
			if (ai->push)
				deliver(bus, ai->addr[0], ai->len[0]);
		}
	}
	ai->mark = now(bus);
	salsa64_sched_set(ai->sched, SALSA64_EV_AI_DMA, ai->mark + bytes_to_cycles(bus, ai->left));
}

/* [1] pasa a ser el DMA en curso. */
static void make_current(struct salsa64_bus *bus)
{
	struct salsa64_ai *ai = &bus->ai;
	ai->left = ai->len[0];
	ai->mark = now(bus);
	ai->started = false;
	salsa64_mi_raise(bus, SALSA64_IRQ_AI);
	begin(bus);
}

static void dma_event(void *ctx, uint64_t when)
{
	struct salsa64_bus *bus = ctx;
	struct salsa64_ai *ai = &bus->ai;
	(void)when;
	if (!ai->count)
		return;
	ai->left = 0;
	if (--ai->count) {
		ai->addr[0] = ai->addr[1];
		ai->len[0] = ai->len[1];
		make_current(bus);
	}
}

void salsa64_ai_init(struct salsa64_bus *bus, struct salsa64_sched *sched)
{
	bus->ai.sched = sched;
	salsa64_sched_handler(sched, SALSA64_EV_AI_DMA, dma_event, bus);
}

void salsa64_ai_reset(struct salsa64_bus *bus)
{
	struct salsa64_ai *ai = &bus->ai;
	struct salsa64_sched *sched = ai->sched;
	salsa64_ai_push_fn push = ai->push;
	void *push_ctx = ai->push_ctx;
	memset(ai, 0, sizeof *ai);
	ai->sched = sched;
	ai->push = push;
	ai->push_ctx = push_ctx;
	if (sched)
		salsa64_sched_cancel(sched, SALSA64_EV_AI_DMA);
}

void salsa64_ai_set_sink(struct salsa64_bus *bus, salsa64_ai_push_fn push, void *ctx)
{
	bus->ai.push = push;
	bus->ai.push_ctx = ctx;
}

uint32_t salsa64_ai_read(struct salsa64_bus *bus, uint32_t reg)
{
	const struct salsa64_ai *ai = &bus->ai;
	if ((reg >> 2) == SALSA64_AI_STATUS) {
		uint32_t s = SALSA64_AI_STATUS_ONES;
		if (ai->count)
			s |= SALSA64_AI_STATUS_BUSY;
		if (ai->count > 1)
			s |= SALSA64_AI_STATUS_FULL;
		if (ai->regs[SALSA64_AI_CONTROL] & 1)
			s |= SALSA64_AI_STATUS_ENABLED;
		return s;
	}
	return left_at(bus, now(bus));
}

void salsa64_ai_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	struct salsa64_ai *ai = &bus->ai;
	uint32_t i = reg >> 2;
	if (i >= SALSA64_AI_NREGS)
		return;
	switch (i) {
	case SALSA64_AI_STATUS:
		salsa64_mi_clear(bus, SALSA64_IRQ_AI);
		return;
	case SALSA64_AI_LEN:
		ai->regs[i] = v;
		if (ai->count == 2 || !ai->sched)
			return;
		ai->addr[ai->count] = ai->regs[SALSA64_AI_DRAM_ADDR] & ADDR_MASK;
		ai->len[ai->count] = v & LEN_MASK;
		if (ai->count++ == 0)
			make_current(bus);
		return;
	case SALSA64_AI_CONTROL:
	case SALSA64_AI_DACRATE:
		/* Lo que sonó hasta ahora, a la frecuencia y el permiso de antes. */
		settle(bus);
		ai->regs[i] = v;
		if (running(ai))
			begin(bus);
		else if (ai->sched)
			salsa64_sched_cancel(ai->sched, SALSA64_EV_AI_DMA);
		return;
	default:
		ai->regs[i] = v;
		return;
	}
}
