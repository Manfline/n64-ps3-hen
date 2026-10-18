/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — la máquina: CPU + bus + planificador. Ver n64.h.
 */
#include "core/n64.h"

static void stop_event(void *ctx, uint64_t when)
{
	(void)when;
	((struct salsa64_cpu *)ctx)->running = 0;
}

void salsa64_n64_init(struct salsa64_n64 *n64, uint8_t *rdram, uint32_t rdram_size)
{
	salsa64_sched_init(&n64->sched);
	salsa64_bus_init(&n64->bus, rdram, rdram_size);
	salsa64_cpu_init(&n64->cpu, &n64->bus, &n64->sched);
	salsa64_sched_handler(&n64->sched, SALSA64_EV_STOP, stop_event, &n64->cpu);
	salsa64_vi_init(&n64->bus, &n64->sched);
	salsa64_rsp_init(&n64->bus, &n64->sched);
	salsa64_ai_init(&n64->bus, &n64->sched);
	salsa64_vi_start(&n64->bus);
	n64->nmi_pending = false;
	n64->nmi_at = 0;
}

void salsa64_n64_reset(struct salsa64_n64 *n64)
{
	salsa64_bus_reset(&n64->bus);
	salsa64_cpu_reset(&n64->cpu);
	salsa64_vi_start(&n64->bus);
}

void salsa64_n64_reset_warm(struct salsa64_n64 *n64)
{
	salsa64_bus_reset_warm(&n64->bus);  /* RCP, PIF y RSP (espera su tarea en vuelo); la RDRAM queda */
	salsa64_cpu_reset(&n64->cpu);
	/* El contador de campos sigue (quien mira los campos, como las pruebas
	 * del host o los avisos, no ve el tiempo retroceder). */
	uint32_t fields = n64->bus.vi.fields;
	salsa64_vi_start(&n64->bus);
	n64->bus.vi.fields = fields;
	n64->nmi_pending = false;
	n64->nmi_at = 0;
}

void salsa64_n64_prenmi(struct salsa64_n64 *n64)
{
	if (n64->nmi_pending)
		return;
	n64->nmi_pending = true;
	n64->nmi_at = n64->cpu.cycles + SALSA64_NMI_DELAY_CYCLES;
	salsa64_cpu_set_ip(&n64->cpu, SALSA64_IP_PRENMI, true);
}

bool salsa64_n64_nmi_due(const struct salsa64_n64 *n64)
{
	return n64->nmi_pending && n64->cpu.cycles >= n64->nmi_at;
}

void salsa64_n64_run(struct salsa64_n64 *n64, uint64_t cycles)
{
	salsa64_sched_set(&n64->sched, SALSA64_EV_STOP, n64->cpu.cycles + cycles);
	salsa64_cpu_run(&n64->cpu);
}

uint64_t salsa64_n64_run_field(struct salsa64_n64 *n64)
{
	uint64_t start = n64->cpu.cycles;
	uint32_t field = n64->bus.vi.fields;
	/* El evento de fin de campo y el de parada caen en el mismo ciclo y se
	 * atienden en el mismo despacho; el bucle cubre un salsa64_n64_stop. */
	while (n64->bus.vi.fields == field)
		salsa64_n64_run(n64, salsa64_vi_cycles_left(&n64->bus));
	return n64->cpu.cycles - start;
}

void salsa64_n64_stop(struct salsa64_n64 *n64)
{
	salsa64_sched_set(&n64->sched, SALSA64_EV_STOP, n64->cpu.cycles);
}
