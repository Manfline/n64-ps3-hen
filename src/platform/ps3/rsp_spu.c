/*
 * Salsa64 — RSP a bajo nivel en un SPE. Ver rsp_spu.h.
 */
#include "platform/ps3/rsp_spu.h"

#include <string.h>
#include <sys/systime.h>

#include "platform/ps3/log.h"
#include "spu_rsp_bin.h"

#define JOB_MAX_INSTR 200000u  /* ~3 ms del RSP real; luego otro trabajo */
#define POLL_SLEEP_US 100

static struct lle_log lle_log_mem __attribute__((aligned(128)));
static uint8_t xbus_image[LLE_MEM_SIZE] __attribute__((aligned(16)));

static void post_run(struct salsa64_rsp_spu *s, uint32_t pc)
{
	struct salsa64_bus *bus = s->bus;
	struct lle_job *j = &s->job;
	memset(j, 0, sizeof *j);
	j->sp_ea = (uint64_t)(uintptr_t)&bus->sp;
	j->mem_ea = (uint64_t)(uintptr_t)bus->sp_mem;
	j->rdram_ea = (uint64_t)(uintptr_t)bus->rdram;
	j->log_ea = (uint64_t)(uintptr_t)s->log;
	j->rdram_size = bus->rdram_size;
	j->pc = pc;
	j->max_instr = JOB_MAX_INSTR;
	j->dp_status = bus->dp[SALSA64_DP_STATUS];
	j->dp_start = bus->dp[SALSA64_DP_START];
	j->dp_end = bus->dp[SALSA64_DP_END];
	j->dp_current = bus->dp[SALSA64_DP_CURRENT];
	s->seq = spukit_post(s->worker, LLE_JOB_RUN, j, sizeof *j);
	s->pending = true;
	s->jobs++;
}

/* Aplica el resultado de un trabajo terminado. */
static void collect(struct salsa64_rsp_spu *s, const spukit_result *r)
{
	struct salsa64_bus *bus = s->bus;
	struct lle_result *res = &s->res;
	s->spu_ticks += r->ticks;
	if (r->error) {
		s->failed++;
		memset(res, 0, sizeof *res);
		res->flags = LLE_RES_HALTED;
		return;
	}
	memcpy(res, r->data, sizeof *res);
	s->instructions += res->instructions;
	s->task_instr += res->instructions;
	bus->sp.pc = res->pc;

	if (res->flags & LLE_RES_WRITTEN_ALL) {
		salsa64_bus_rdram_written(bus, 0, bus->rdram_size);
	} else {
		for (uint32_t i = 0; i < res->nwritten && i < LLE_WRITTEN_MAX; i++)
			salsa64_bus_rdram_written(bus, res->written[i].start, res->written[i].len);
	}
	/* Tramos del RDP, en orden; los de xbus con los comandos capturados. */
	for (uint32_t i = 0; i < res->dpc_entries && i < LLE_DPC_MAX; i++) {
		const struct lle_dpc_entry *e = &s->log->entries[i];
		s->dpc_entries++;
		if (!e->xbus) {
			salsa64_dp_submit(bus, e->start, e->end, NULL);
		} else if (e->cap_off != ~0u && e->end > e->start && e->end - e->start <= LLE_MEM_SIZE) {
			uint32_t len = e->end - e->start;
			for (uint32_t k = 0; k < len; k++)
				xbus_image[(e->start + k) & (LLE_MEM_SIZE - 1)] = s->log->capture[e->cap_off + k];
			salsa64_dp_submit(bus, e->start, e->end, xbus_image);
		} else {
			s->dpc_lost++;
		}
	}
	if (res->flags & LLE_RES_CAPTURE_FULL)
		s->dpc_lost++;
	bus->dp[SALSA64_DP_STATUS] = (bus->dp[SALSA64_DP_STATUS] & ~(SALSA64_DP_STATUS_XBUS | SALSA64_DP_STATUS_FREEZE |
							      SALSA64_DP_STATUS_FLUSH)) |
				     (res->dp_status & (SALSA64_DP_STATUS_XBUS | SALSA64_DP_STATUS_FREEZE |
							SALSA64_DP_STATUS_FLUSH));
	if (res->irq & LLE_IRQ_SP) {
		s->irqs++;
		salsa64_mi_raise(bus, SALSA64_IRQ_SP);
	}
	if (res->flags & LLE_RES_HALTED) {
		if (s->task_instr > s->task_max)
			s->task_max = s->task_instr > UINT32_MAX ? UINT32_MAX : (uint32_t)s->task_instr;
	}
}

/* Espera el trabajo en vuelo y lo aplica; true si el RSP se detuvo. */
static bool wait_job(struct salsa64_rsp_spu *s)
{
	uint64_t t0 = 0;
	if (!spukit_done(s->worker, s->seq)) {
		t0 = sysGetSystemTime();
		s->waits++;
	}
	const spukit_result *r = spukit_wait(s->worker, s->seq);
	if (t0)
		s->wait_us += sysGetSystemTime() - t0;
	s->pending = false;
	collect(s, r);
	return (s->res.flags & LLE_RES_HALTED) != 0;
}

static void spu_start(void *ctx)
{
	struct salsa64_rsp_spu *s = ctx;
	if (s->pending && !wait_job(s)) {
		/* El CPU lo detuvo y lo reanuda antes de que el SPE terminara:
		 * el trabajo que sigue parte de donde quedó. */
	}
	s->tasks++;
	s->task_instr = 0;
	post_run(s, s->bus->sp.pc);
}

static bool spu_done(void *ctx, bool idle)
{
	struct salsa64_rsp_spu *s = ctx;
	for (;;) {
		if (!s->pending)
			return true;
		if (!spukit_done(s->worker, s->seq)) {
			s->polls++;
			if (idle) {
				sysUsleep(POLL_SLEEP_US);
				s->sleep_us += POLL_SLEEP_US;
				if (!spukit_done(s->worker, s->seq))
					return false;
			} else {
				return false;
			}
		}
		if (wait_job(s))
			return true;
		/* Agotó el cupo sin detenerse: sigue en otro trabajo. */
		post_run(s, s->res.pc);
		idle = false;
	}
}

static void spu_finish(void *ctx)
{
	struct salsa64_rsp_spu *s = ctx;
	if (s->pending)
		wait_job(s);
}

static void spu_reset(void *ctx)
{
	struct salsa64_rsp_spu *s = ctx;
	if (s->pending)
		wait_job(s);
	spukit_wait(s->worker, spukit_post(s->worker, LLE_JOB_RESET, NULL, 0));
}

int salsa64_rsp_spu_init(struct salsa64_rsp_spu *s, struct salsa64_bus *bus)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = spu_start;
	s->backend.sync = NULL;
	s->backend.reset = spu_reset;
	s->backend.done = spu_done;
	s->backend.finish = spu_finish;
	s->bus = bus;
	s->log = &lle_log_mem;
	s->worker = spukit_worker_start(spu_rsp_bin, "salsa64_rsp", 0);
	if (!s->worker)
		return spukit_last_error();
	spukit_wait(s->worker, spukit_post(s->worker, LLE_JOB_RESET, NULL, 0));
	return 0;
}

int salsa64_rsp_spu_shutdown(struct salsa64_rsp_spu *s)
{
	int rc = 0;
	if (s->pending)
		wait_job(s);
	if (s->worker)
		rc = spukit_worker_stop(s->worker);
	s->worker = NULL;
	return rc;
}
