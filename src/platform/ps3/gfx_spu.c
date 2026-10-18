/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — backend gráfico en un SPE. Ver gfx_spu.h.
 */
#include "platform/ps3/gfx_spu.h"

#include <string.h>
#include <sys/systime.h>

#include "core/bus.h"
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

/* Estadísticas de un trabajo que terminó (tarea o lista del CPU). */
static void account_res(struct salsa64_gfx_spu *s, const struct gfx_result *res, uint64_t ticks)
{
	s->spu_ticks += ticks;
	s->t_vtx += res->t_vtx;
	s->t_state += res->t_state;
	s->t_tex += res->t_tex;
	s->t_load += res->t_load;
	s->textures += res->textures;
	s->tex_reused += res->tex_reused;
	s->loads += res->loads;
	s->load_reads += res->load_reads;
	s->tex_old += res->tex_old;
	s->tex_arena += res->tex_arena;
	s->tex_evicted += res->tex_evicted;
	s->t_hash += res->t_hash;
	s->skipped += res->skipped;
	s->bg += res->bg;
	s->tex_target += res->tex_target;
}

/* Un trabajo que el SPE devolvió con error no dibujó (o no sabemos qué): termina
 * como cortado, con su interrupción del DP; sin ella el juego se quedaría
 * esperando al RDP para siempre. */
static void job_error(struct salsa64_gfx_spu *s, const spukit_result *r, struct gfx_result *res, const char *what)
{
	memset(res, 0, sizeof *res);
	res->flags |= GFX_RES_CUT;
	if (s->errors++ < 4)
		salsa64_log("gfx: el SPE devolvió el error %d en %s (van %u): termina como cortada", (int)r->error, what,
			    s->errors);
}

static void account(struct salsa64_gfx_spu *s, uint64_t ticks)
{
	s->tasks++;
	account_res(s, &s->res, ticks);
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
		job_error(s, r, &s->res, "una tarea");
	account(s, r->ticks);
	salsa64_gfx_rdram_fills(s->bus, &s->job, &s->res);
	s->sink.done(s->sink.ctx, &s->job, &s->res);
	return salsa64_gfx_task_flags(&s->res);
}

/* Lista del RDP que escribe el CPU: el mismo SPE y el mismo estado que las
 * tareas (el de las dos HLE separadas se pisaba la caché de texturas: los
 * fondos de Killer Instinct Gold salían rotos). La tarea en vuelo, si la
 * hay, se recoge antes para que su salida y la de la lista lleguen al
 * consumidor en el orden en que se dieron. Síncrono: el CPU espera su
 * SYNC_FULL (la interrupción del DP) y la lista es corta. */
static uint32_t spu_dpc_run(void *ctx, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	struct salsa64_gfx_spu *s = ctx;
	if (dmem && s->xbus)
		return s->xbus->run(s->xbus->ctx, start, end, dmem);
	if (s->pending)
		salsa64_rsp_quiesce((struct salsa64_bus *)s->bus);
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)s->bus->rdram;
	job.rdram_size = s->bus->rdram_size;
	job.dmem_ea = (uint64_t)(uintptr_t)dmem;
	job.dl = start;
	job.dl_end = end;
	job.flags = GFX_JOB_DPC | (dmem ? GFX_JOB_XBUS : 0);
	s->sink.acquire(s->sink.ctx, &job);
	if (s->nocache)
		job.flags |= GFX_JOB_NOCACHE;
	uint64_t t0 = sysGetSystemTime();
	const spukit_result *r = spukit_wait(s->worker, spukit_post(s->worker, GFX_JOB_TASK, &job, sizeof job));
	s->dpc_us += sysGetSystemTime() - t0;
	memcpy(&res, r->data, sizeof res);
	if (r->error)
		job_error(s, r, &res, "una lista del CPU");
	s->dpc_jobs++;
	account_res(s, &res, r->ticks);
	salsa64_gfx_rdram_fills(s->bus, &job, &res);
	s->sink.done(s->sink.ctx, &job, &res);
	return (res.flags & (GFX_RES_FULLSYNC | GFX_RES_CUT)) ? SALSA64_GFX_FULLSYNC : 0;
}


/* Estados guardados: el SPE copia el tramo crudo de su gfx_state (registros
 * del RDP, tiles y TMEM) al mismo lugar de esta imagen, o lo trae de ella; el
 * formato portable se arma aquí (rsp/gfx/state.c). Si el SPE dice que el
 * tramo está en otro lugar de su struct (otra disposición), no se guarda ni
 * se carga nada de la HLE. Sin tarea en vuelo. */
static struct gfx_state shadow __attribute__((aligned(128)));

static bool spu_state(struct salsa64_gfx_spu *s, uint32_t kind)
{
	if (s->pending || !s->worker)
		return false;
	struct gfx_state_job sj = { (uint64_t)(uintptr_t)((uint8_t *)&shadow + GFX_RAW_START) };
	const spukit_result *r = spukit_wait(s->worker, spukit_post(s->worker, kind, &sj, sizeof sj));
	uint32_t where[2];
	memcpy(where, r->data, sizeof where);
	if (r->error || where[0] != GFX_RAW_START || where[1] != GFX_RAW_END) {
		salsa64_log("estado de la HLE gráfica: el SPE no lo dio (error %d, tramo %u–%u en el SPE, %u–%u aquí)",
			    r->error, where[0], where[1], (unsigned)GFX_RAW_START, (unsigned)GFX_RAW_END);
		return false;
	}
	return true;
}

static bool spu_save_state(void *ctx, uint8_t *buf)
{
	if (!spu_state(ctx, GFX_JOB_STATE_SAVE))
		return false;
	gfx_state_save(&shadow, buf);
	return true;
}

static bool spu_load_state(void *ctx, const uint8_t *buf)
{
	/* Lo que comparte el tramo y no es del formato (tex_gen, el final del
	 * estado de Rare) se conserva: primero se trae lo que tiene el SPE. */
	if (!spu_state(ctx, GFX_JOB_STATE_SAVE) || !gfx_state_load(&shadow, buf))
		return false;
	return spu_state(ctx, GFX_JOB_STATE_LOAD);
}

int salsa64_gfx_spu_init(struct salsa64_gfx_spu *s, const struct salsa64_bus *bus,
			 const struct salsa64_gfx_sink *sink)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = spu_start;
	s->backend.finish = spu_finish;
	s->backend.done = spu_done;
	s->backend.state_size = GFX_STATE_BYTES;
	s->backend.save_state = spu_save_state;
	s->backend.load_state = spu_load_state;
#ifdef SALSA64_SPU_S2DEX_FULL
	s->backend.s2dex = true;
#endif
	s->dpc.ctx = s;
	s->dpc.run = spu_dpc_run;
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
