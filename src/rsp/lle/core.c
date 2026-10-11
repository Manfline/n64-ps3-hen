/*
 * Salsa64 — LLE del RSP en el hilo del CPU (host y PPE): el conductor que
 * une el intérprete (lle.h) con el bus.
 *
 * El RSP corre a 62,5 MHz y el VR4300 a 93,75: dos instrucciones del RSP
 * por cada tres ciclos del CPU (una instrucción por ciclo, sin la
 * segmentación real). El microcódigo avanza en rebanadas: un evento del
 * planificador cada LLE_SLICE_CYCLES ejecuta lo que corresponde al tiempo
 * transcurrido, y cada acceso del CPU a un registro del SP o del DP
 * (`sync`) lo pone al día antes, así un bucle del CPU que sondea
 * SP_STATUS ve el RSP avanzar entre lecturas como en la consola.
 *
 * La DMEM y la IMEM son las del bus (bus->sp_mem): lo que el CPU escribe
 * en ellas lo ve el RSP en el acto y al revés. COP0 va por los registros
 * del bus (salsa64_sp_read/write y salsa64_dp_read/write): el DMA, el
 * semáforo, las señales y los comandos del RDP se comportan igual que
 * desde el CPU.
 */
#include "rsp/lle/core.h"

#include <string.h>

#include "core/sched.h"
#include "cpu/vr4300.h"

#define LLE_SLICE_CYCLES 1536u  /* ciclos del CPU entre rebanadas (1024 instrucciones) */
#define LLE_BURST_MAX    4096u  /* instrucciones por llamada a lle_run */

static uint64_t cpu_now(const struct salsa64_lle_host *h)
{
	return h->bus->cpu ? h->bus->cpu->cycles : 0;
}

static inline bool bus_halted(const struct salsa64_lle_host *h)
{
	return (__atomic_load_n(&h->bus->sp.regs[SALSA64_SP_STATUS], __ATOMIC_SEQ_CST) & SALSA64_SP_STATUS_HALT) != 0;
}

/* Ejecuta las instrucciones que corresponden hasta el ciclo `now`. */
static void run_due(struct salsa64_lle_host *h, uint64_t now)
{
	if (h->in_slice || !h->running)
		return;
	uint64_t elapsed = now > h->last ? now - h->last : 0;
	uint64_t units = elapsed * h->ipc_num + h->credit;
	uint64_t budget = units / h->ipc_den;
	h->credit = (uint32_t)(units % h->ipc_den);
	h->last = now;

	h->rsp.halt = bus_halted(h);
	h->in_slice = true;
	while (budget > 0 && !h->rsp.halt) {
		uint32_t n = lle_run(&h->rsp, budget < LLE_BURST_MAX ? (uint32_t)budget : LLE_BURST_MAX);
		budget -= n;
		h->instructions += n;
	}
	h->in_slice = false;
	h->bus->sp.pc = h->rsp.pc;
	if (h->rsp.halt) {
		uint64_t len = h->instructions - h->task_start;
		h->tasks_done++;
		if (len > h->task_max)
			h->task_max = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
		h->running = false;
		salsa64_sched_cancel(h->bus->rsp.sched, SALSA64_EV_RSP_LLE);
	}
}

static void slice_event(void *ctx, uint64_t when)
{
	struct salsa64_lle_host *h = ctx;
	h->slices++;
	run_due(h, when);
	if (h->running)
		salsa64_sched_set(h->bus->rsp.sched, SALSA64_EV_RSP_LLE, when + LLE_SLICE_CYCLES);
}

static void host_start(void *ctx)
{
	struct salsa64_lle_host *h = ctx;
	/* Si el CPU escribió SP_PC desde la última vez, de ahí; si solo quitó
	 * HALT, sigue donde iba (con su delay slot pendiente). */
	if (h->bus->sp.pc_writes != h->pc_writes) {
		h->pc_writes = h->bus->sp.pc_writes;
		lle_set_pc(&h->rsp, h->bus->sp.pc);
	}
	h->rsp.halt = false;
	h->starts++;
	if (h->running)
		return;
	h->running = true;
	h->last = cpu_now(h);
	h->credit = 0;
	h->task_start = h->instructions;
	salsa64_sched_set(h->bus->rsp.sched, SALSA64_EV_RSP_LLE, h->last + LLE_SLICE_CYCLES);
}

void salsa64_lle_host_speed(struct salsa64_lle_host *h, double ipc)
{
	if (ipc <= 0.0)
		return;
	h->ipc_den = 1000;
	h->ipc_num = (uint32_t)(ipc * 1000.0 + 0.5);
	if (!h->ipc_num)
		h->ipc_num = 1;
	h->credit = 0;
}

static void host_sync(void *ctx)
{
	struct salsa64_lle_host *h = ctx;
	if (!h->running || h->in_slice)
		return;
	h->syncs++;
	run_due(h, cpu_now(h));
}

static void host_reset(void *ctx)
{
	struct salsa64_lle_host *h = ctx;
	h->running = false;
	h->in_slice = false;
	salsa64_sched_cancel(h->bus->rsp.sched, SALSA64_EV_RSP_LLE);
	lle_reset(&h->rsp);
}

void salsa64_lle_host_init(struct salsa64_lle_host *h, struct salsa64_bus *bus)
{
	memset(h, 0, sizeof *h);
	h->bus = bus;
	h->ipc_num = 2;
	h->ipc_den = 3;
	h->backend.ctx = h;
	h->backend.start = host_start;
	h->backend.sync = host_sync;
	h->backend.reset = host_reset;
	lle_init(&h->rsp, bus->sp_mem, bus->sp_mem + LLE_MEM_SIZE);
	h->rsp.io = h;
	salsa64_sched_handler(bus->rsp.sched, SALSA64_EV_RSP_LLE, slice_event, h);
}

uint32_t salsa64_lle_host_run(struct salsa64_lle_host *h, uint32_t max)
{
	h->rsp.halt = bus_halted(h);
	h->in_slice = true;
	uint32_t n = lle_run(&h->rsp, max);
	h->in_slice = false;
	h->instructions += n;
	h->bus->sp.pc = h->rsp.pc;
	return n;
}

/* ---- COP0 sobre el bus ---- */

uint32_t lle_io_read(struct rsp_lle *s, unsigned reg)
{
	struct salsa64_lle_host *h = s->io;
	if (reg < 8)
		return salsa64_sp_read(h->bus, 0x40000u | (reg << 2));
	return salsa64_dp_read(h->bus, (reg - 8) << 2);
}

void lle_io_write(struct rsp_lle *s, unsigned reg, uint32_t v)
{
	struct salsa64_lle_host *h = s->io;
	if (reg < 8) {
		salsa64_sp_write(h->bus, 0x40000u | (reg << 2), v);
		if (reg == LLE_C0_STATUS)
			s->halt = bus_halted(h);
	} else {
		salsa64_dp_write(h->bus, (reg - 8) << 2, v);
	}
}

void lle_io_break(struct rsp_lle *s)
{
	struct salsa64_lle_host *h = s->io;
	uint32_t st = __atomic_or_fetch(&h->bus->sp.regs[SALSA64_SP_STATUS],
					SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE, __ATOMIC_SEQ_CST);
	if (st & SALSA64_SP_STATUS_INTR_BREAK)
		salsa64_mi_raise(h->bus, SALSA64_IRQ_SP);
	s->halt = true;
}

void lle_io_poll(struct rsp_lle *s)
{
	(void)s;
}
