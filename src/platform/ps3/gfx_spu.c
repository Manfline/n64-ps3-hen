/*
 * Salsa64 — backend gráfico en un SPE. Ver gfx_spu.h.
 */
#include "platform/ps3/gfx_spu.h"

#include <string.h>
#include <sys/systime.h>

#include "platform/ps3/log.h"
#include "rsp/gfx/hle.h"
#include "spu_gfx_bin.h"

/* El hash con SIMD del SPE (identidad de las texturas en la caché) debe dar
 * lo mismo que el de C: tramos que cortan bloques de 16 bytes y que dan la
 * vuelta al anillo. Devuelve los casos que fallaron. */
static int check_hash(spukit_worker *w)
{
	static uint8_t pat[4096] __attribute__((aligned(128)));
	static const uint32_t cases[][3] = {
		{ 4096, 0, 4096 }, { 4096, 8, 24 }, { 4096, 4088, 16 }, { 4096, 1000, 2000 },
		{ 2048, 2040, 520 }, { 4096, 3, 13 }, { 16, 0, 16 },
	};
	for (int i = 0; i < 4096; i++)
		pat[i] = (uint8_t)(i * 31 + (i >> 8));
	int bad = 0;
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		struct gfx_hash_job hj = { (uint64_t)(uintptr_t)pat, cases[c][0], cases[c][1], cases[c][2] };
		const spukit_result *r = spukit_wait(w, spukit_post(w, GFX_JOB_HASH, &hj, sizeof hj));
		struct gfx_hash h;
		uint32_t key[2], spu[2];
		gfx_hash_init(&h);
		gfx_hash_ring(&h, pat, hj.size, hj.start, hj.len);
		gfx_hash_final(&h, key);
		memcpy(spu, r->data, sizeof spu);
		if (r->error || spu[0] != key[0] || spu[1] != key[1]) {
			salsa64_log("hash del SPE: caso %u da %08x%08x, el PPE %08x%08x", c, spu[0], spu[1], key[0], key[1]);
			bad++;
		}
	}
	return bad;
}

static void spu_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	struct salsa64_gfx_spu *s = ctx;
	salsa64_gfx_job(&s->job, s->bus, task, gbi);
	s->sink.acquire(s->sink.ctx, &s->job);
	if (s->nocache)
		s->job.flags |= GFX_JOB_NOCACHE;
	s->seq = spukit_post(s->worker, GFX_JOB_TASK, &s->job, sizeof s->job);
	s->pending = true;
}

#define POLL_SLEEP_US 100

static bool spu_done(void *ctx, bool idle)
{
	struct salsa64_gfx_spu *s = ctx;
	if (!s->pending || spukit_done(s->worker, s->seq))
		return true;
	s->polls++;
	if (idle) {
		sysUsleep(POLL_SLEEP_US);
		s->sleep_us += POLL_SLEEP_US;
		return spukit_done(s->worker, s->seq);
	}
	return false;
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
	s->tex_old += s->res.tex_old;
	s->tex_arena += s->res.tex_arena;
	s->tex_evicted += s->res.tex_evicted;
	s->t_hash += s->res.t_hash;
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
	s->backend.done = spu_done;
	s->bus = bus;
	s->sink = *sink;
	s->worker = spukit_worker_start(spu_gfx_bin, "salsa64_gfx", 0);
	if (!s->worker)
		return spukit_last_error();
	int bad = check_hash(s->worker);
	salsa64_log("hash de texturas en el SPE: %s", bad ? "FALLO (sin caché de texturas)" : "igual al del PPE");
	s->nocache = bad != 0;
	return 0;
}

void salsa64_gfx_spu_shutdown(struct salsa64_gfx_spu *s)
{
	if (s->pending)
		spu_finish(s);
	if (s->worker)
		spukit_worker_stop(s->worker);
	s->worker = NULL;
}
