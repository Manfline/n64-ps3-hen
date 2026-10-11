/*
 * Salsa64 — RDP sobre el RSX. Ver rdp.h.
 *
 * Hilos (docs/hilos-ppe.md): el sink (acquire y done) corre en el hilo de
 * emulación; todo lo demás, en el de presentación. Las tareas terminadas
 * esperan en una cola de SETS entradas hasta que salsa64_rdp_flush las
 * traduce. Un juego de búferes queda libre cuando se tradujo (contador
 * `translated`) y el RSX pasó su barrera (`fence`, que el hilo de
 * emulación consulta con rsxkit_fence_done: solo lee una etiqueta).
 *
 * Caché de texturas (gfx.h): otra región mapeada que administra la HLE.
 * La tarea n lleva el número n + 1 y, como done_task, la última tarea
 * cuya barrera ya pasó, nunca la última traducida (salsa64_rdp_dump lee
 * sus texturas).
 */
#include "rdp/rdp.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/systime.h>

#include "rsp/gfx/gfx.h"
#include "rsp/gfx/hle.h"

#define SETS        3
#define VTX_BYTES   (2u << 20)
#define CMD_BYTES   (512u << 10)
#define TEX_BYTES   (4u << 20)
#define SET_BYTES   (VTX_BYTES + CMD_BYTES + TEX_BYTES)
#define SETS_BYTES  (((SETS * SET_BYTES) + 0xFFFFFu) & ~0xFFFFFu)
#define CACHE_BYTES (8u << 20)  /* caché de texturas (docs/memory-budget.md) */
#define POOL_BYTES  (SETS_BYTES + CACHE_BYTES)
#define MAX_TARGETS 4
#define MAX_DEPTHS  2
#define STALE_FIELDS 30  /* un destino sin dibujar en tantos campos ya no se presenta */

_Static_assert(sizeof(struct gfx_vertex) == sizeof(rsxkit_vertex), "vértice de la HLE = vértice de rsxkit");
_Static_assert(sizeof(((struct gfx_draw *)0)->k) == sizeof(((rsxkit_draw *)0)->k), "términos k");
_Static_assert((int)GFX_K_T0 == (int)RSXKIT_K_T0 && (int)GFX_K_T1A == (int)RSXKIT_K_T1A &&
		       (int)GFX_K_ONE == (int)RSXKIT_K_ONE, "orden de k");
_Static_assert((int)GFX_CULL_BACK == (int)RSXKIT_CULL_BACK && (int)GFX_CULL_FRONT == (int)RSXKIT_CULL_FRONT,
	       "cull");
_Static_assert(GFX_DEPTH_TEST == RSXKIT_DEPTH_TEST && GFX_DEPTH_WRITE == RSXKIT_DEPTH_WRITE &&
		       GFX_DEPTH_DECAL == RSXKIT_DEPTH_OFFSET, "profundidad");

struct set {
	uint8_t *vtx, *cmd, *tex;
	uint32_t vtx_off, tex_off;  /* offsets del RSX */
	uint32_t fence;             /* la pone el hilo de presentación al traducir */
	volatile uint32_t fence_task;  /* número de la tarea antes de `fence` */
	struct gfx_result res;      /* la deja done; la lee flush */
};

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

struct target {
	bool used;
	bool drawn;              /* dibujado desde la última presentación */
	uint32_t addr, width, lw, lh, bpp;
	uint32_t last_field;
	rsxkit_target *t;
};

struct depth {
	bool used;
	uint32_t addr, w, h;
	bool clear_pending;
	float clear_rect[4];
	float clear_value;
	rsxkit_depth *d;
};

static struct {
	bool ready;
	uint32_t scale;
	uint8_t *pool;
	uint32_t pool_off;
	uint8_t *cache;              /* región de la caché de texturas */
	uint32_t cache_off;
	uint32_t done_task;          /* última tarea que el RSX terminó (hilo de emulación) */
	struct set sets[SETS];
	/* Tareas: acquire y done avanzan `acquired` y `finished` (hilo de
	 * emulación); flush avanza `translated` (presentación). La tarea n usa
	 * el juego n % SETS. */
	volatile uint32_t acquired, finished, translated;
	struct target targets[MAX_TARGETS];
	/* Lo presentado en el último campo desde el RSX (NULL: la RDRAM). */
	const struct target *shown;
	uint32_t shown_origin, shown_field;
	struct depth depths[MAX_DEPTHS];
	struct target *bound_t;
	struct depth *bound_d;
	uint32_t field;
	unsigned last_set;           /* juego de búferes de la última tarea dibujada */
	struct gfx_result last_res;
	struct salsa64_rdp_stats stats;
} rdp;

static void acquire(void *ctx, struct gfx_job *job);
static void done(void *ctx, const struct gfx_job *job, const struct gfx_result *res);
static const struct salsa64_gfx_sink sink = { NULL, acquire, done };

int salsa64_rdp_init(uint32_t scale)
{
	memset(&rdp, 0, sizeof rdp);
	const rsxkit_screen *sc = rsxkit_screen_info();
	rdp.scale = scale ? scale : (sc->height >= 240 ? sc->height / 240 : 1);
	rdp.pool = memalign(1u << 20, POOL_BYTES);
	if (!rdp.pool)
		return RSXKIT_ENOMEM;
	int rc = rsxkit_map_main(rdp.pool, POOL_BYTES, &rdp.pool_off);
	if (rc != RSXKIT_OK)
		return rc;
	for (unsigned i = 0; i < SETS; i++) {
		struct set *s = &rdp.sets[i];
		uint32_t base = i * SET_BYTES;
		s->vtx = rdp.pool + base;
		s->cmd = s->vtx + VTX_BYTES;
		s->tex = s->cmd + CMD_BYTES;
		s->vtx_off = rdp.pool_off + base;
		s->tex_off = rdp.pool_off + base + VTX_BYTES + CMD_BYTES;
	}
	rdp.cache = rdp.pool + SETS_BYTES;
	rdp.cache_off = rdp.pool_off + SETS_BYTES;
	rdp.ready = true;
	return RSXKIT_OK;
}

static void release_targets(void)
{
	for (int i = 0; i < MAX_TARGETS; i++)
		if (rdp.targets[i].used)
			rsxkit_target_destroy(rdp.targets[i].t);
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (rdp.depths[i].used)
			rsxkit_depth_destroy(rdp.depths[i].d);
	memset(rdp.targets, 0, sizeof rdp.targets);
	memset(rdp.depths, 0, sizeof rdp.depths);
	rdp.bound_t = NULL;
	rdp.bound_d = NULL;
	rdp.shown = NULL;
}

void salsa64_rdp_shutdown(void)
{
	if (!rdp.ready)
		return;
	rsxkit_finish();
	release_targets();
	/* La memoria mapeada queda reservada hasta el fin del proceso. */
	rdp.ready = false;
}

uint32_t salsa64_rdp_reset(void)
{
	if (!rdp.ready)
		return 0;
	rsxkit_finish();
	uint32_t pending = rdp.acquired - rdp.translated;
	release_targets();
	/* Lo que es de un juego vuelve a como lo deja salsa64_rdp_init; la
	 * memoria mapeada (juegos de búferes y caché de texturas) se conserva. */
	rdp.done_task = 0;
	for (unsigned i = 0; i < SETS; i++) {
		rdp.sets[i].fence = 0;
		rdp.sets[i].fence_task = 0;
		memset(&rdp.sets[i].res, 0, sizeof rdp.sets[i].res);
	}
	rdp.acquired = rdp.finished = rdp.translated = 0;
	rdp.shown_origin = rdp.shown_field = 0;
	rdp.field = 0;
	rdp.last_set = 0;
	memset(&rdp.last_res, 0, sizeof rdp.last_res);
	memset(&rdp.stats, 0, sizeof rdp.stats);
	return pending;
}

const struct salsa64_gfx_sink *salsa64_rdp_sink(void)
{
	return &sink;
}

static void acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
	uint32_t n = rdp.acquired;
	struct set *s = &rdp.sets[n % SETS];
	/* Libre cuando su tarea anterior (n − SETS) ya se tradujo y el RSX pasó
	 * la barrera que la siguió. */
	if (n >= SETS) {
		uint64_t t0 = 0;
		for (;;) {
			bool translated = (int32_t)(rdp.translated - (n - SETS + 1)) >= 0;
			lwsync();
			if (translated && rsxkit_fence_done(s->fence))
				break;
			if (!t0) {
				t0 = sysGetSystemTime();
				rdp.stats.set_waits++;
			}
			sysUsleep(50);
		}
		if (t0)
			rdp.stats.us_set_wait += sysGetSystemTime() - t0;
	}
	/* Tareas que el RSX ya terminó: la de cada juego cuya barrera pasó (las
	 * hace en orden). fence_task se lee antes que fence: si es la nueva,
	 * fence también lo es (translate las escribe al revés). */
	for (unsigned k = 0; k < SETS; k++) {
		uint32_t ft = rdp.sets[k].fence_task;
		lwsync();
		if (ft && (int32_t)(ft - rdp.done_task) > 0 && rsxkit_fence_done(rdp.sets[k].fence))
			rdp.done_task = ft;
	}
	uint32_t last = rdp.translated, done = rdp.done_task;
	if ((int32_t)(done - (last - 1)) > 0)
		done = last - 1;
	job->task = n + 1;
	job->done_task = done;
	job->cache_ea = (uint64_t)(uintptr_t)rdp.cache;
	job->cache_cap = CACHE_BYTES;
	job->vtx_ea = (uint64_t)(uintptr_t)s->vtx;
	job->vtx_cap = VTX_BYTES;
	job->cmd_ea = (uint64_t)(uintptr_t)s->cmd;
	job->cmd_cap = CMD_BYTES;
	job->tex_ea = (uint64_t)(uintptr_t)s->tex;
	job->tex_cap = TEX_BYTES;
	rdp.acquired = n + 1;
}

/* Destino de una color image (o el menos usado, reciclado). */
static struct target *get_target(uint32_t addr, uint32_t width, uint32_t siz)
{
	struct target *free_t = NULL, *oldest = NULL;
	for (int i = 0; i < MAX_TARGETS; i++) {
		struct target *t = &rdp.targets[i];
		if (!t->used) {
			if (!free_t)
				free_t = t;
			continue;
		}
		if (t->addr == addr && t->width == width)
			return t;
		if (!oldest || (int32_t)(t->last_field - oldest->last_field) < 0)
			oldest = t;
	}
	struct target *t = free_t ? free_t : oldest;
	if (!t)
		return NULL;
	if (t->used) {
		if (rdp.bound_t == t)
			rdp.bound_t = NULL;
		rsxkit_target_destroy(t->t);
		t->used = false;
	}
	uint32_t lw = width ? width : 320, lh = lw * 3 / 4;
	t->t = rsxkit_target_create(lw * rdp.scale, lh * rdp.scale);
	if (!t->t)
		return NULL;
	t->used = true;
	t->addr = addr;
	t->width = width;
	t->lw = lw;
	t->lh = lh;
	t->bpp = siz == G_IM_SIZ_32b ? 4 : siz == G_IM_SIZ_8b ? 1 : 2;
	t->last_field = rdp.field;
	t->drawn = false;
	rdp.stats.targets_created++;
	return t;
}

static struct depth *get_depth(uint32_t addr, const struct target *t)
{
	uint32_t w = t->lw * rdp.scale, h = t->lh * rdp.scale;
	struct depth *free_d = NULL;
	for (int i = 0; i < MAX_DEPTHS; i++) {
		struct depth *d = &rdp.depths[i];
		if (d->used && d->addr == addr && d->w >= w && d->h >= h)
			return d;
		if (!d->used && !free_d)
			free_d = d;
	}
	struct depth *d = free_d ? free_d : &rdp.depths[addr & 1];
	if (d->used) {
		if (rdp.bound_d == d)
			rdp.bound_d = NULL;
		rsxkit_depth_destroy(d->d);
		d->used = false;
	}
	d->d = rsxkit_depth_create(w, h);
	if (!d->d)
		return NULL;
	d->used = true;
	d->addr = addr;
	d->w = w;
	d->h = h;
	d->clear_pending = false;
	return d;
}

static void bind(struct target *t, struct depth *d)
{
	if (t == rdp.bound_t && d == rdp.bound_d)
		return;
	rsxkit_bind(t->t, d ? d->d : NULL, (float)t->lw, (float)t->lh);
	rdp.bound_t = t;
	rdp.bound_d = d;
	if (d && d->clear_pending) {
		rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, d->clear_value, d->clear_rect);
		d->clear_pending = false;
	}
}

/* Borrado del z-buffer: se aplica ahora si está ligado, o al ligarlo. */
static void zclear(const struct gfx_zclear *z)
{
	struct depth *d = NULL;
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (rdp.depths[i].used && rdp.depths[i].addr == z->h.a)
			d = &rdp.depths[i];
	float rect[4] = { z->rect[0], z->rect[1], z->rect[2], z->rect[3] };
	/* Z de 18 bits de la N64 en el valor de relleno (14 bits + dz). */
	float value = (float)(z->h.b >> 2) / 16383.0f;
	if (d && d == rdp.bound_d) {
		rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, value, rect);
		return;
	}
	if (d) {
		d->clear_pending = true;
		memcpy(d->clear_rect, rect, sizeof rect);
		d->clear_value = value;
	}
	/* Si aún no existe, nacerá en 1 (borrada). */
}

static void to_rsxkit(const struct gfx_draw *g, const struct set *s, rsxkit_draw *d)
{
	static const uint8_t blend[] = {
		[GFX_BLEND_OFF] = RSXKIT_BLEND_OFF, [GFX_BLEND_ALPHA] = RSXKIT_BLEND_ALPHA,
		[GFX_BLEND_ADD] = RSXKIT_BLEND_ADD, [GFX_BLEND_KEEP] = RSXKIT_BLEND_NONE,
	};
	static const uint8_t wrap[] = {
		[GFX_WRAP_CLAMP] = RSXKIT_WRAP_CLAMP, [GFX_WRAP_REPEAT] = RSXKIT_WRAP_REPEAT,
		[GFX_WRAP_MIRROR] = RSXKIT_WRAP_MIRROR, [3] = RSXKIT_WRAP_CLAMP,
	};
	memset(d, 0, offsetof(rsxkit_draw, k));
	d->blend = g->blend < sizeof blend ? blend[g->blend] : RSXKIT_BLEND_OFF;
	d->cull = g->cull;
	d->depth = g->depth;
	d->alpha_ref = g->atest ? (g->aref ? g->aref : 1) : 0;
	d->ntex = g->ntex > 2 ? 2 : g->ntex;
	d->fog = g->fog;
	for (int i = 0; i < 4; i++) {
		d->scissor[i] = g->scissor[i] / 4.0f;
		d->fog_color[i] = ((g->fog_color >> (24 - 8 * i)) & 0xFF) / 255.0f;
	}
	for (int i = 0; i < d->ntex; i++) {
		const struct gfx_tex *gt = &g->tex[i];
		rsxkit_texture *t = &d->tex[i];
		t->offset = (gt->where == GFX_TEX_CACHE ? rdp.cache_off : s->tex_off) + gt->offset;
		t->pitch = gt->pitch;
		t->width = gt->width;
		t->height = gt->height;
		t->location = RSXKIT_MEM_MAIN;
		t->format = RSXKIT_RGBA8888;
		t->wrap_s = wrap[GFX_TEX_WRAP_S(gt->flags)];
		t->wrap_t = wrap[GFX_TEX_WRAP_T(gt->flags)];
		t->filter = (gt->flags & GFX_TEX_BILINEAR) ? RSXKIT_FILTER_LINEAR : RSXKIT_FILTER_NEAREST;
		memcpy(t->xf, gt->xf, sizeof t->xf);
	}
	memcpy(d->k, g->k, sizeof d->k);
}

static void done(void *ctx, const struct gfx_job *job, const struct gfx_result *res)
{
	(void)ctx, (void)job;
	uint32_t n = rdp.finished;
	rdp.sets[n % SETS].res = *res;
	lwsync();
	rdp.finished = n + 1;
}

/* Traduce la salida de la tarea n a comandos del RSX. */
static void translate(uint32_t n)
{
	uint64_t t0 = sysGetSystemTime();
	struct set *s = &rdp.sets[n % SETS];
	const struct gfx_result *res = &s->res;
	rsxkit_vertices(s->vtx_off, RSXKIT_MEM_MAIN);
	rdp.bound_t = NULL;
	rdp.bound_d = NULL;
	struct target *cur = NULL;

	uint32_t off = 0;
	rsxkit_draw rd;
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		switch (c->op) {
		case GFX_OP_TARGET:
			cur = get_target(c->a, c->b, c->c & 0xFF);
			break;
		case GFX_OP_ZCLEAR:
			zclear((const struct gfx_zclear *)c);
			break;
		case GFX_OP_DRAW: {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			if (!cur || !g->h.b)
				break;
			struct depth *d = rdp.bound_t == cur ? rdp.bound_d : NULL;
			if (g->depth)
				d = get_depth(g->zimg, cur);
			bind(cur, d);
			cur->last_field = rdp.field;
			cur->drawn = true;
			to_rsxkit(g, s, &rd);
			rsxkit_draw_triangles(&rd, g->h.a, g->h.b);
			rdp.stats.draws++;
			rdp.stats.triangles += g->h.b / 3;
			break;
		}
		default:
			break;
		}
		off += c->size;
	}
	s->fence = rsxkit_fence();
	lwsync();
	s->fence_task = n + 1;
	rdp.last_res = *res;
	rdp.last_set = (unsigned)(s - rdp.sets);
	rdp.stats.tasks++;
	rdp.stats.us_translate += sysGetSystemTime() - t0;
}

uint32_t salsa64_rdp_flush(uint32_t upto)
{
	uint32_t n = rdp.translated, done_n = rdp.finished;
	lwsync();  /* los resultados, después de ver `finished` */
	if (upto == ~0u)
		upto = done_n;
	while ((int32_t)(n - done_n) < 0 && (int32_t)(n - upto) < 0) {
		translate(n);
		lwsync();  /* la barrera del juego, antes de darlo por traducido */
		rdp.translated = ++n;
	}
	return n;
}

uint32_t salsa64_rdp_tasks_done(void)
{
	return rdp.finished;
}

bool salsa64_rdp_present(const struct salsa64_vi_frame *f, uint32_t field, const rsxkit_view *view,
			 rsxkit_rect *out)
{
	rdp.field = field;
	if (!rdp.ready)
		return false;
	/* rdp.field solo avanza cuando el VI muestra algo: lo dibujado mientras
	 * estuvo en negro (SM64 lo apaga 26 campos al cambiar de escena) quedó
	 * con el campo de antes y parecería viejo al volver la imagen. */
	for (int i = 0; i < MAX_TARGETS; i++)
		if (rdp.targets[i].drawn) {
			rdp.targets[i].last_field = field;
			rdp.targets[i].drawn = false;
		}
	/* Puede haber más de uno: tras un cambio de resolución, la misma
	 * dirección con el ancho de antes (GoldenEye pasa de 320 a 440 al ir a
	 * las carpetas y, en campos alternos, salía la imagen vieja). Se toma el
	 * del ancho que muestra el VI y, entre iguales, el dibujado hace menos. */
	const struct target *best = NULL;
	for (int i = 0; i < MAX_TARGETS; i++) {
		const struct target *t = &rdp.targets[i];
		if (!t->used || field - t->last_field > STALE_FIELDS)
			continue;
		uint32_t line = t->width * t->bpp;
		if (f->origin < t->addr || f->origin >= t->addr + line * t->lh)
			continue;
		if (best) {
			bool tw = t->width == f->width, bw = best->width == f->width;
			if (bw && !tw)
				continue;
			if (bw == tw && (int32_t)(t->last_field - best->last_field) <= 0)
				continue;
		}
		best = t;
	}
	if (!best && rdp.shown && rdp.shown->used && f->origin == rdp.shown_origin &&
	    field - rdp.shown_field <= 2) {
		/* El juego sostiene la misma imagen sin dibujar (GoldenEye, un
		 * segundo mientras carga las carpetas): con la HLE la RDRAM de ese
		 * framebuffer no tiene lo que dibujó el RSX (salían datos viejos,
		 * ruido), así que mientras el VI no cambie de origen se sigue
		 * presentando el mismo destino. */
		const struct target *t = rdp.shown;
		if (f->origin >= t->addr && f->origin < t->addr + t->width * t->bpp * t->lh)
			best = t;
	}
	rdp.shown = best;
	rdp.shown_origin = f->origin;
	rdp.shown_field = field;
	if (!best)
		return false;
	uint32_t line = best->width * best->bpp;
	uint32_t y0 = (f->origin - best->addr) / line;
	uint32_t w = f->width < best->lw ? f->width : best->lw;
	uint32_t h = f->height < best->lh - y0 ? f->height : best->lh - y0;
	rsxkit_rect src = { 0, (int32_t)(y0 * rdp.scale), w * rdp.scale, h * rdp.scale };
	rsxkit_target_present(best->t, &src, f->width, f->height, view, out);
	return true;
}

bool salsa64_rdp_dump(const char *path)
{
	if (!rdp.ready || !rdp.last_res.cmd_bytes)
		return false;
	const struct set *s = &rdp.sets[rdp.last_set];
	const struct gfx_result *r = &rdp.last_res;
	uint32_t max = 2 * (r->cmd_bytes / sizeof(struct gfx_draw)) + 1;
	uint8_t *cmd = malloc(r->cmd_bytes);
	uint32_t *src = malloc(max * 2 * sizeof *src), *len = src + max;
	if (!cmd || !src) {
		free(cmd);
		free(src);
		return false;
	}
	memcpy(cmd, s->cmd, r->cmd_bytes);
	/* Las texturas de la caché se copian detrás del arena (una vez cada
	 * una) y los dibujos pasan a apuntar ahí. */
	uint32_t arena = (r->tex_bytes + 127) & ~127u, tex_bytes = arena, n = 0;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= r->cmd_bytes;) {
		struct gfx_cmd *c = (struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		struct gfx_draw *d = (struct gfx_draw *)c;
		for (int i = 0; c->op == GFX_OP_DRAW && i < d->ntex && i < 2; i++) {
			struct gfx_tex *t = &d->tex[i];
			if (t->where != GFX_TEX_CACHE)
				continue;
			uint32_t k = 0, dst = arena;
			while (k < n && src[k] != t->offset)
				dst += (len[k++] + 127) & ~127u;
			if (k == n && n < max) {
				src[n] = t->offset;
				len[n++] = (uint32_t)t->pitch * t->height;
				tex_bytes += (len[k] + 127) & ~127u;
			}
			t->where = GFX_TEX_ARENA;
			t->offset = dst;
		}
		off += c->size;
	}
	FILE *f = fopen(path, "wb");
	static const uint8_t zero[128];
	struct gfx_dump_header h = { GFX_DUMP_MAGIC, r->cmd_bytes, r->vtx_bytes, tex_bytes };
	bool ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(cmd, 1, h.cmd_bytes, f) == h.cmd_bytes &&
		  fwrite(s->vtx, 1, h.vtx_bytes, f) == h.vtx_bytes && fwrite(s->tex, 1, r->tex_bytes, f) == r->tex_bytes &&
		  fwrite(zero, 1, arena - r->tex_bytes, f) == arena - r->tex_bytes;
	for (uint32_t k = 0; ok && k < n; k++) {
		uint32_t pad = ((len[k] + 127) & ~127u) - len[k];
		ok = fwrite(rdp.cache + src[k], 1, len[k], f) == len[k] && fwrite(zero, 1, pad, f) == pad;
	}
	if (f)
		fclose(f);
	free(cmd);
	free(src);
	return ok;
}

void salsa64_rdp_get_stats(struct salsa64_rdp_stats *out)
{
	*out = rdp.stats;
}
