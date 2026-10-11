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

void salsa64_gfx_sync_init(struct salsa64_gfx_sync *s, struct gfx_state *state, const struct salsa64_bus *bus,
			   const struct salsa64_gfx_sink *sink)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = sync_start;
	s->backend.finish = sync_finish;
	s->state = state;
	s->bus = bus;
	s->sink = *sink;
	gfx_init(state);
}
