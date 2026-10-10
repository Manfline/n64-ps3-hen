/*
 * Salsa64 — backend gráfico en un SPE. Ver gfx_spu.h.
 */
#include "platform/ps3/gfx_spu.h"

#include <string.h>
#include <sys/systime.h>

#include "spu_gfx_bin.h"

static void spu_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	struct salsa64_gfx_spu *s = ctx;
	salsa64_gfx_job(&s->job, s->bus, task, gbi);
	s->sink.acquire(s->sink.ctx, &s->job);
	s->seq = spukit_post(s->worker, GFX_JOB_TASK, &s->job, sizeof s->job);
	s->pending = true;
}

static uint32_t spu_finish(void *ctx)
{
	struct salsa64_gfx_spu *s = ctx;
	if (!s->pending)
		return 0;
	s->pending = false;
	uint64_t t0 = 0;
	if (!spukit_done(s->worker, s->seq)) {
		t0 = sysGetSystemTime();
		s->waits++;
	}
	const spukit_result *r = spukit_wait(s->worker, s->seq);
	if (t0)
		s->wait_us += sysGetSystemTime() - t0;
	memcpy(&s->res, r->data, sizeof s->res);
	if (r->error)
		memset(&s->res, 0, sizeof s->res);
	s->tasks++;
	s->spu_ticks += r->ticks;
	s->t_vtx += s->res.t_vtx;
	s->t_state += s->res.t_state;
	s->t_tex += s->res.t_tex;
	s->t_load += s->res.t_load;
	s->textures += s->res.textures;
	s->tex_reused += s->res.tex_reused;
	s->loads += s->res.loads;
	s->load_reads += s->res.load_reads;
	s->sink.done(s->sink.ctx, &s->job, &s->res);
	return (s->res.flags & GFX_RES_FULLSYNC) ? SALSA64_GFX_FULLSYNC : 0;
}

int salsa64_gfx_spu_init(struct salsa64_gfx_spu *s, const struct salsa64_bus *bus,
			 const struct salsa64_gfx_sink *sink)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = spu_start;
	s->backend.finish = spu_finish;
	s->bus = bus;
	s->sink = *sink;
	s->worker = spukit_worker_start(spu_gfx_bin, "salsa64_gfx", 0);
	return s->worker ? 0 : spukit_last_error();
}

void salsa64_gfx_spu_shutdown(struct salsa64_gfx_spu *s)
{
	if (s->pending)
		spu_finish(s);
	if (s->worker)
		spukit_worker_stop(s->worker);
	s->worker = NULL;
}
