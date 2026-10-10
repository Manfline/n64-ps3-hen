/*
 * Retro64 — la máquina: CPU + bus + planificador. Ver n64.h.
 */
#include "core/n64.h"

static void stop_event(void *ctx, uint64_t when)
{
	(void)when;
	((struct retro64_cpu *)ctx)->running = 0;
}

void retro64_n64_init(struct retro64_n64 *n64, uint8_t *rdram, uint32_t rdram_size)
{
	retro64_sched_init(&n64->sched);
	retro64_bus_init(&n64->bus, rdram, rdram_size);
	retro64_cpu_init(&n64->cpu, &n64->bus, &n64->sched);
	retro64_sched_handler(&n64->sched, RETRO64_EV_STOP, stop_event, &n64->cpu);
}

void retro64_n64_reset(struct retro64_n64 *n64)
{
	retro64_bus_reset(&n64->bus);
	retro64_cpu_reset(&n64->cpu);
}

void retro64_n64_run(struct retro64_n64 *n64, uint64_t cycles)
{
	retro64_sched_set(&n64->sched, RETRO64_EV_STOP, n64->cpu.cycles + cycles);
	retro64_cpu_run(&n64->cpu);
}

void retro64_n64_stop(struct retro64_n64 *n64)
{
	retro64_sched_set(&n64->sched, RETRO64_EV_STOP, n64->cpu.cycles);
}
