/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — backend síncrono de la HLE gráfica. Ver backend.h.
 */
#include <string.h>

#include "rsp/gfx/backend.h"
#include "rsp/gfx/hle.h"

void salsa64_gfx_job(struct gfx_job *job, const struct salsa64_bus *bus, const struct salsa64_task *task,
		     enum salsa64_gbi gbi)
{
	memset(job, 0, sizeof *job);
	job->rdram_ea = (uint64_t)(uintptr_t)bus->rdram;
	job->rdram_size = bus->rdram_size;
	job->gbi = gbi;
	job->dl = task->data_ptr & 0x00FFFFFF;
	job->ucode = task->ucode & 0x00FFFFFF;
	/* El de Perfect Dark (sin texto) se toma como NoN: sin recorte el autor lo
	 * jugó bien en la PS3 y GLide64 dibuja los muros cercanos de su intro
	 * que el recorte quitaría; no está verificado contra hardware. */
	if (bus->rsp.gfx_nonear || gbi == SALSA64_GBI_F3DPD)
		job->flags |= GFX_JOB_NONEAR;
}

uint32_t salsa64_gfx_task_flags(const struct gfx_result *res)
{
	uint32_t f = 0;
	if (res->flags & GFX_RES_FULLSYNC)
		f |= SALSA64_GFX_FULLSYNC;
	if ((res->flags & GFX_RES_CUT) || res->dl_errors)
		f |= SALSA64_GFX_CUT;
	return f;
}

static void sync_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	struct salsa64_gfx_sync *s = ctx;
	salsa64_gfx_job(&s->job, s->bus, task, gbi);
	if (s->reset) {
		s->job.flags |= GFX_JOB_RESET;
		s->reset = false;
	}
	s->sink.acquire(s->sink.ctx, &s->job);
	gfx_run(s->state, &s->job, &s->res);
	s->pending = true;
}

static uint32_t sync_finish(void *ctx)
{
	struct salsa64_gfx_sync *s = ctx;
	if (!s->pending)
		return 0;
	s->pending = false;
	s->sink.done(s->sink.ctx, &s->job, &s->res);
	return salsa64_gfx_task_flags(&s->res);
}

void gfx_state_save(const struct gfx_state *g, uint8_t *out)
{
	gfx_state_pack_regs(g, out);
	memcpy(out + GFX_STATE_REGS, g->tmem, sizeof g->tmem);
}

bool gfx_state_load(struct gfx_state *g, const uint8_t *in)
{
	if (!gfx_state_unpack_regs(g, in))
		return false;
	memcpy(g->tmem, in + GFX_STATE_REGS, sizeof g->tmem);
	return true;
}

static bool sync_save_state(void *ctx, uint8_t *buf)
{
	struct salsa64_gfx_sync *s = ctx;
	gfx_state_save(s->state, buf);
	return true;
}

static bool sync_load_state(void *ctx, const uint8_t *buf)
{
	struct salsa64_gfx_sync *s = ctx;
	s->reset = false;
	return gfx_state_load(s->state, buf);
}

static uint32_t sync_dpc_run(void *ctx, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	struct salsa64_gfx_sync *s = ctx;
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)s->bus->rdram;
	job.rdram_size = s->bus->rdram_size;
	job.dmem_ea = (uint64_t)(uintptr_t)dmem;
	job.dl = start;
	job.dl_end = end;
	job.flags = GFX_JOB_DPC | (dmem ? GFX_JOB_XBUS : 0);
	if (s->reset) {
		job.flags |= GFX_JOB_RESET;
		s->reset = false;
	}
	s->sink.acquire(s->sink.ctx, &job);
	gfx_run(s->state, &job, &res);
	s->sink.done(s->sink.ctx, &job, &res);
	s->dpc_runs++;
	s->dpc_sum.commands += res.commands;
	s->dpc_sum.triangles += res.triangles;
	s->dpc_sum.rects += res.rects;
	s->dpc_sum.draws += res.draws;
	s->dpc_sum.unknown_cmds += res.unknown_cmds;
	s->dpc_sum.dl_errors += res.dl_errors;
	s->dpc_sum.flags |= res.flags;
	return (res.flags & GFX_RES_FULLSYNC) ? SALSA64_GFX_FULLSYNC : 0;
}

void salsa64_gfx_sync_init(struct salsa64_gfx_sync *s, struct gfx_state *state, const struct salsa64_bus *bus,
			   const struct salsa64_gfx_sink *sink)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = sync_start;
	s->backend.finish = sync_finish;
	s->backend.state_size = GFX_STATE_BYTES;
	s->backend.save_state = sync_save_state;
	s->backend.load_state = sync_load_state;
	s->dpc.ctx = s;
	s->dpc.run = sync_dpc_run;
	s->state = state;
	s->bus = bus;
	s->sink = *sink;
	gfx_init(state);
}
