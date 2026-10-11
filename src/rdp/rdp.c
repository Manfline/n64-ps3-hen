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
 * Destinos de render: la tabla (qué color image tiene superficie, cuál se
 * recicla, cuál se presenta) es targets.c, portable y con pruebas en el
 * host; aquí solo viven las superficies de rsxkit, una por ranura.
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

#include "rdp/targets.h"
#include "rsp/gfx/gfx.h"
#include "rsp/gfx/hle.h"

#define SETS        3
#define VTX_BYTES   (2u << 20)
/* gfx_draw son 480 bytes: 1600 dibujos por tarea. El arena de texturas
 * casi no se usa con la caché (docs/memory-budget.md): le cede 256 KB. */
#define CMD_BYTES   (768u << 10)
#define TEX_BYTES   ((4u << 20) - (256u << 10))
#define SET_BYTES   (VTX_BYTES + CMD_BYTES + TEX_BYTES)
#define SETS_BYTES  (((SETS * SET_BYTES) + 0xFFFFFu) & ~0xFFFFFu)
#define CACHE_BYTES (8u << 20)  /* caché de texturas (docs/memory-budget.md) */
/* Efectos de framebuffer (docs/framebuffer.md): dos búferes para las copias
 * RSX → RDRAM (RGB565 a la resolución de la N64, hasta 640 × 480) y uno
 * para cebar una superficie con lo que el CPU dejó en la RDRAM (RGBA5551). */
#define RB_MAX_W    640
#define RB_MAX_H    480
#define RB_PITCH    ((RB_MAX_W * 2 + 63) & ~63u)
#define RB_SLOT     (RB_PITCH * RB_MAX_H)
#define RB_SLOTS    2
/* Copias espejo de las imágenes auxiliares chicas (iconos, pantallas), sin
 * esperar: búferes de 64 KB (hasta 128 × 256 a 16 bits). */
#define MIRROR_BYTES 65536u
#define MIRRORS      4
#define FB_BYTES    (((RB_SLOTS + 1) * RB_SLOT + MIRRORS * MIRROR_BYTES + 0xFFFFFu) & ~0xFFFFFu)
#define POOL_BYTES  (SETS_BYTES + CACHE_BYTES + FB_BYTES)
#define MAX_DEPTHS  2
#define RB_PER_TASK 4   /* color images que una tarea puede pedir copiar */
#define RB_WAIT_US  200000  /* la emulación no espera una copia más que esto */

_Static_assert(sizeof(struct gfx_vertex) == sizeof(rsxkit_vertex), "vértice de la HLE = vértice de rsxkit");
_Static_assert(sizeof(((struct gfx_draw *)0)->k) == sizeof(((rsxkit_draw *)0)->k), "términos k");
_Static_assert(sizeof(((struct gfx_draw *)0)->factors) == sizeof(((rsxkit_draw *)0)->factors), "factores");
_Static_assert((int)GFX_K_T0 == (int)RSXKIT_K_T0 && (int)GFX_K_T1A == (int)RSXKIT_K_T1A &&
		       (int)GFX_K_ONE == (int)RSXKIT_K_ONE, "orden de k");
_Static_assert((int)GFX_F_T0 == (int)RSXKIT_FP_T0 && (int)GFX_F_T0A == (int)RSXKIT_FP_T0A &&
		       (int)GFX_F_T1 == (int)RSXKIT_FP_T1 && (int)GFX_F_T1A == (int)RSXKIT_FP_T1A &&
		       (int)GFX_F_C == (int)RSXKIT_FP_C && (int)GFX_F_CA == (int)RSXKIT_FP_CA, "factores");
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
	/* Color images de la tarea que hay que copiar a la RDRAM al traducirla
	 * (lo decide done, en el hilo de emulación, que después espera). */
	uint32_t rb_addr[RB_PER_TASK];
	uint32_t rb_n;
};

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

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
	struct rdp_targets tg;                    /* la tabla de destinos (targets.c) */
	rsxkit_target *surf[RDP_TARGETS_MAX];     /* la superficie de cada ranura */
	bool surf_dirty[RDP_TARGETS_MAX];         /* dibujada desde la última rsxkit_texture_sync */
	rsxkit_target *scratch;      /* copia del destino ligado cuando un dibujo lo lee (SELF) */
	uint32_t scratch_w, scratch_h;
	int scratch_slot;            /* ranura copiada en esta tarea, o -1 */
	struct depth depths[MAX_DEPTHS];
	int bound_t;                 /* ranura ligada, o -1 */
	struct depth *bound_d;
	unsigned last_set;           /* juego de búferes de la última tarea dibujada */
	struct gfx_result last_res;
	struct salsa64_rdp_stats stats;
	/* Efectos de framebuffer (docs/framebuffer.md). */
	int fbfx;                    /* 0 apagado, 1 render a textura, 2 también copias */
	uint8_t *rdram;              /* la RDRAM emulada (NULL: sin copias ni cebado) */
	uint32_t rdram_size;
	void (*wake)(void);          /* despierta al hilo de presentación */
	struct rdp_vi_ring vi_ring;  /* orígenes recientes del VI (hilo de emulación) */
	uint8_t *rb;                 /* búferes de copia (RB_SLOTS) y de cebado, mapeados */
	uint32_t rb_off;
	volatile uint32_t readback_done;  /* última tarea con sus copias ya en la RDRAM */
	bool rb_late_logged;
	/* Copias espejo en vuelo: el RSX escribe el búfer; cuando pasa la
	 * barrera se convierte a la RDRAM (readbacks_finish). */
	struct mirror {
		bool used;
		uint32_t addr, width, lw, rows, fence;
	} mirrors[MIRRORS];
} rdp;

static void acquire(void *ctx, struct gfx_job *job);
static void done(void *ctx, const struct gfx_job *job, const struct gfx_result *res);
static const struct salsa64_gfx_sink sink = { NULL, acquire, done };

int salsa64_rdp_init(uint32_t scale)
{
	memset(&rdp, 0, sizeof rdp);
	rdp_targets_init(&rdp.tg);
	rdp.bound_t = -1;
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
	rdp.rb = rdp.cache + CACHE_BYTES;
	rdp.rb_off = rdp.cache_off + CACHE_BYTES;
	rdp.fbfx = 2;
	rdp.ready = true;
	return RSXKIT_OK;
}

void salsa64_rdp_set_memory(uint8_t *rdram, uint32_t rdram_size, void (*wake)(void))
{
	rdp.rdram = rdram;
	rdp.rdram_size = rdram_size;
	rdp.wake = wake;
}

void salsa64_rdp_vi(uint32_t origin, uint32_t width)
{
	rdp_vi_ring_push(&rdp.vi_ring, origin, width);
}

void salsa64_rdp_fbfx(int level)
{
	rdp.fbfx = level < 0 ? 0 : level > 2 ? 2 : level;
}

static void release_targets(void)
{
	for (int i = 0; i < RDP_TARGETS_MAX; i++)
		if (rdp.surf[i])
			rsxkit_target_destroy(rdp.surf[i]);
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (rdp.depths[i].used)
			rsxkit_depth_destroy(rdp.depths[i].d);
	if (rdp.scratch)
		rsxkit_target_destroy(rdp.scratch);
	rdp.scratch = NULL;
	rdp.scratch_w = rdp.scratch_h = 0;
	memset(rdp.surf, 0, sizeof rdp.surf);
	memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
	memset(rdp.depths, 0, sizeof rdp.depths);
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
	rdp_targets_init(&rdp.tg);  /* la tabla de destinos, vacía */
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
	rdp.readback_done = 0;
	rdp.rb_late_logged = false;
	memset(rdp.mirrors, 0, sizeof rdp.mirrors);
	memset(&rdp.vi_ring, 0, sizeof rdp.vi_ring);
	memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
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

/* Hasta qué línea llegan las tijeras de los dibujos que siguen a un
 * TARGET (hasta el siguiente): el alto mínimo de su superficie. */
static uint32_t scissor_bottom(const struct set *s, uint32_t off, uint32_t cmd_bytes)
{
	uint32_t bottom = 0;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->op == GFX_OP_TARGET || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW) {
			uint32_t y1 = (((const struct gfx_draw *)c)->scissor[3] + 3) / 4;
			if (y1 > bottom)
				bottom = y1;
		}
		off += c->size;
	}
	return bottom;
}

/* Ranura (con superficie) de una color image; -1 si el RSX no tiene
 * memoria para una nueva. */
/* Escribe en la RDRAM, como RGBA5551, `rows` filas de una copia RGB565 de
 * `lw` píxeles por fila con paso `pitch`. */
static void to_rdram(const uint8_t *src, uint32_t pitch, uint32_t addr, uint32_t width, uint32_t lw, uint32_t rows)
{
	for (uint32_t y = 0; y < rows; y++)
		rsxkit_pixels_565_to_5551((uint16_t *)(rdp.rdram + addr + y * width * 2), (const uint16_t *)(src + y * pitch),
					  lw);
}

/* Copias espejo cuya barrera ya pasó: a la RDRAM y el búfer queda libre. */
static void readbacks_finish(bool wait)
{
	for (int k = 0; k < MIRRORS; k++) {
		struct mirror *m = &rdp.mirrors[k];
		if (!m->used)
			continue;
		if (wait)
			rsxkit_fence_wait(m->fence);
		else if (!rsxkit_fence_done(m->fence))
			continue;
		uint32_t pitch = (m->lw * 2 + 63) & ~63u;
		to_rdram(rdp.rb + (RB_SLOTS + 1) * RB_SLOT + k * MIRROR_BYTES, pitch, m->addr, m->width, m->lw, m->rows);
		m->used = false;
		rdp.stats.mirrors++;
	}
}

/* Ceba la superficie de la ranura con lo que hay en la RDRAM (lo que el
 * CPU dibujó o dejó ahí): la N64 siempre dibuja encima de la memoria. Solo
 * color images de 16 bits que caben en el búfer de cebado. */
static void prime(int i)
{
	const struct rdp_target *t = &rdp.tg.t[i];
	if (!rdp.rdram || rdp.fbfx < 1 || t->bpp != 2 || t->lw > RB_MAX_W || t->lh > RB_MAX_H)
		return;
	uint32_t line = t->width * 2, bytes = line * t->lh;
	if (t->addr + bytes > rdp.rdram_size)
		return;
	uint8_t *dst = rdp.rb + RB_SLOTS * RB_SLOT;
	uint32_t pitch = (t->lw * 2 + 63) & ~63u;
	for (uint32_t y = 0; y < t->lh; y++)
		memcpy(dst + y * pitch, rdp.rdram + t->addr + y * line, t->lw * 2);
	if (rsxkit_target_fill(rdp.surf[i], rdp.rb_off + RB_SLOTS * RB_SLOT, pitch, t->lw, t->lh, RSXKIT_RGBA5551) ==
	    RSXKIT_OK) {
		rdp.bound_t = -1;  /* rsxkit_target_fill ligó la superficie sin profundidad */
		rdp.stats.primes++;
	}
}

static int get_target(uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh)
{
	bool fresh;
	int i = rdp_targets_get(&rdp.tg, addr, width, siz, min_lh, &fresh);
	if (!fresh) {
		/* Sin dibujar desde hace tareas: lo que vale está en la RDRAM. */
		if (rdp.tg.task - rdp.tg.t[i].last_task > RDP_TARGETS_FRESH)
			prime(i);
		return i;
	}
	if (rdp.surf[i]) {
		if (rdp.bound_t == i)
			rdp.bound_t = -1;
		rsxkit_target_destroy(rdp.surf[i]);
		rdp.surf[i] = NULL;
	}
	const struct rdp_target *t = &rdp.tg.t[i];
	rdp.surf[i] = rsxkit_target_create(t->lw * rdp.scale, t->lh * rdp.scale);
	if (!rdp.surf[i]) {
		rdp.tg.t[i].used = false;
		return -1;
	}
	rdp.surf_dirty[i] = false;
	rdp.stats.targets_created++;
	prime(i);
	return i;
}

static struct depth *get_depth(uint32_t addr, int slot)
{
	const struct rdp_target *t = &rdp.tg.t[slot];
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

static void bind(int slot, struct depth *d)
{
	if (slot == rdp.bound_t && d == rdp.bound_d)
		return;
	const struct rdp_target *t = &rdp.tg.t[slot];
	rsxkit_bind(rdp.surf[slot], d ? d->d : NULL, (float)t->lw, (float)t->lh);
	rdp.bound_t = slot;
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

/* Si la textura viene de un destino dibujado hace poco, se lee de su
 * superficie (a la escala del RSX) en vez de la copia decodificada de la
 * RDRAM. Devuelve true si hay que sincronizar antes del dibujo (la
 * superficie se dibujó desde la última vez). */
static bool texture_from_target(const struct gfx_tex *gt, int cur, rsxkit_texture *t)
{
	if (rdp.fbfx < 1 || !gt->src_addr || gt->src_siz != 2u)
		return false;
	int slot;
	uint32_t x0, y0;
	enum rdp_texsrc src = rdp_targets_texture(&rdp.tg, gt->src_addr, gt->src_pitch, 2, cur, &slot, &x0, &y0);
	if ((src != RDP_TEXSRC_RSX && src != RDP_TEXSRC_SELF) || !rdp.surf[slot])
		return false;
	const struct rdp_target *tg = &rdp.tg.t[slot];
	/* Una textura que se repite a lo ancho no cabe en filas enteras. */
	if (GFX_TEX_WRAP_S(gt->flags) != GFX_WRAP_CLAMP && gt->width != tg->lw)
		return false;
	const rsxkit_target *from = rdp.surf[slot];
	if (src == RDP_TEXSRC_SELF) {
		/* Lee lo que está dibujando: una copia de la superficie tal como
		 * está ahora (una por tarea; las franjas de Zelda leen cada una
		 * filas que aún no reescribió). */
		if (rdp.scratch_slot != slot) {
			uint32_t w, h;
			rsxkit_target_size(rdp.surf[slot], &w, &h);
			if (rdp.scratch && (rdp.scratch_w != w || rdp.scratch_h != h)) {
				rsxkit_target_destroy(rdp.scratch);
				rdp.scratch = NULL;
			}
			if (!rdp.scratch) {
				rdp.scratch = rsxkit_target_create(w, h);
				rdp.scratch_w = w;
				rdp.scratch_h = h;
			}
			if (!rdp.scratch || rsxkit_target_copy(rdp.scratch, rdp.surf[slot]) != RSXKIT_OK)
				return false;
			rdp.bound_t = -1;  /* rsxkit_target_copy dejó ligado el scratch */
			rdp.scratch_slot = slot;
		}
		from = rdp.scratch;
	}
	rsxkit_texture nt;
	rsxkit_target_texture(from, y0 * rdp.scale, gt->height * rdp.scale, &nt);
	if (!nt.height)
		return false;
	t->offset = nt.offset;
	t->pitch = nt.pitch;
	t->width = nt.width;
	t->height = nt.height;
	t->location = nt.location;
	t->format = nt.format;
	t->wrap_s = RSXKIT_WRAP_CLAMP;
	/* u recorre la fila entera del destino: la textura empieza en x0. */
	float w = (float)gt->width, lw = (float)tg->lw;
	t->xf[0] = gt->xf[0] * w / lw;
	t->xf[2] = ((float)x0 + gt->xf[2] * w) / lw;
	return src == RDP_TEXSRC_RSX && rdp.surf_dirty[slot];
}

/* G_RM_VISCVG: en vez de dibujar, deja en la RDRAM «cobertura plena» en
 * las filas del rectángulo (el CPU de Zelda salta los píxeles cubiertos
 * del todo al filtrar el fondo de la pausa). */
static void viscvg(int cur, const struct gfx_draw *g)
{
	const struct rdp_target *t = &rdp.tg.t[cur];
	if (!rdp.rdram || rdp.fbfx < 2 || t->bpp != 2)
		return;
	uint32_t y0 = g->scissor[1] / 4, y1 = (g->scissor[3] + 3) / 4, x0 = g->scissor[0] / 4, x1 = (g->scissor[2] + 3) / 4;
	if (y1 > t->lh)
		y1 = t->lh;
	if (x1 > t->width)
		x1 = t->width;
	for (uint32_t y = y0; y < y1 && x1 > x0; y++) {
		uint32_t a = t->addr + (y * t->width + x0) * 2;
		if (a + (x1 - x0) * 2 > rdp.rdram_size)
			break;
		memset(rdp.rdram + a, 0xFF, (x1 - x0) * 2);
	}
}

static void to_rsxkit(const struct gfx_draw *g, const struct set *s, int cur, rsxkit_draw *d, bool *sync)
{
	*sync = false;
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
	d->nterms = g->nterms;
	memcpy(d->factors, g->factors, sizeof d->factors);
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
		if (texture_from_target(gt, cur, t))
			*sync = true;
	}
	memcpy(d->k, g->k, sizeof d->k);
}

/* Color images de la tarea que el CPU va a leer (rdp_readback_wanted):
 * las copia el hilo de presentación al traducirla; mientras, la emulación
 * espera aquí, para que la RDRAM esté al día cuando llegue la interrupción
 * del DP. Solo recorre las cabeceras de los registros. */
static void readbacks_wanted(struct set *s, const struct gfx_result *res)
{
	s->rb_n = 0;
	if (!rdp.rdram || rdp.fbfx < 2)
		return;
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_TARGET && rdp_readback_wanted(&rdp.vi_ring, c->a, c->b, c->c & 0xFF)) {
			uint32_t k = 0;
			while (k < s->rb_n && s->rb_addr[k] != c->a)
				k++;
			if (k == s->rb_n && k < RB_PER_TASK)
				s->rb_addr[s->rb_n++] = c->a;
		}
		off += c->size;
	}
}

static void done(void *ctx, const struct gfx_job *job, const struct gfx_result *res)
{
	(void)ctx, (void)job;
	uint32_t n = rdp.finished;
	struct set *s = &rdp.sets[n % SETS];
	s->res = *res;
	readbacks_wanted(s, res);
	lwsync();
	rdp.finished = n + 1;
	if (!s->rb_n)
		return;
	if (rdp.wake)
		rdp.wake();
	uint64_t t0 = sysGetSystemTime();
	rdp.stats.readback_waits++;
	while ((int32_t)(rdp.readback_done - (n + 1)) < 0) {
		sysUsleep(50);
		if (sysGetSystemTime() - t0 > RB_WAIT_US) {
			rdp.stats.readback_late++;
			break;
		}
	}
	rdp.stats.us_readback_wait += sysGetSystemTime() - t0;
}

/* Traduce la salida de la tarea n a comandos del RSX. */
static void translate(uint32_t n)
{
	uint64_t t0 = sysGetSystemTime();
	struct set *s = &rdp.sets[n % SETS];
	const struct gfx_result *res = &s->res;
	rsxkit_vertices(s->vtx_off, RSXKIT_MEM_MAIN);
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
	rdp_targets_task(&rdp.tg);
	rdp.scratch_slot = -1;
	readbacks_finish(false);
	int cur = -1;
	bool drawn[RDP_TARGETS_MAX] = { false };
	uint32_t drawn_rows[RDP_TARGETS_MAX] = { 0 };  /* hasta dónde llegaron las tijeras */

	uint32_t off = 0;
	rsxkit_draw rd;
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		switch (c->op) {
		case GFX_OP_TARGET:
			cur = get_target(c->a, c->b, c->c & 0xFF, scissor_bottom(s, off + c->size, res->cmd_bytes));
			break;
		case GFX_OP_ZCLEAR:
			zclear((const struct gfx_zclear *)c);
			break;
		case GFX_OP_DRAW: {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			if (cur < 0 || !g->h.b)
				break;
			if (g->viscvg) {
				viscvg(cur, g);
				break;
			}
			bool sync;
			to_rsxkit(g, s, cur, &rd, &sync);
			if (sync) {
				/* Lee una superficie dibujada en este mismo tramo. */
				rsxkit_texture_sync();
				memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
			}
			struct depth *d = rdp.bound_t == cur ? rdp.bound_d : NULL;
			if (g->depth)
				d = get_depth(g->zimg, cur);
			bind(cur, d);
			rdp_targets_drawn(&rdp.tg, cur);
			drawn[cur] = true;
			rdp.surf_dirty[cur] = true;
			if ((uint32_t)(g->scissor[3] + 3) / 4 > drawn_rows[cur])
				drawn_rows[cur] = (g->scissor[3] + 3) / 4;
			rsxkit_draw_triangles(&rd, g->h.a, g->h.b);
			rdp.stats.draws++;
			if (g->nterms)
				rdp.stats.draws_generated++;
			rdp.stats.triangles += g->h.b / 3;
			break;
		}
		default:
			break;
		}
		off += c->size;
	}
	/* Copias a la RDRAM que pidió done: el RSX reduce cada destino a su
	 * búfer y, pasada la barrera, el PPE lo convierte y lo escribe. */
	struct { int slot; uint32_t buf, rows; } rb[RB_SLOTS];
	uint32_t nrb = 0;
	bool sync_rb[RDP_TARGETS_MAX] = { false };
	for (uint32_t k = 0; k < s->rb_n && nrb < RB_SLOTS; k++) {
		for (int i = 0; i < RDP_TARGETS_MAX; i++) {
			const struct rdp_target *t = &rdp.tg.t[i];
			uint32_t rows = drawn_rows[i] < t->lh ? drawn_rows[i] : t->lh;
			if (!drawn[i] || !rows || t->addr != s->rb_addr[k] || t->bpp != 2 || t->lw > RB_MAX_W ||
			    t->lh > RB_MAX_H || t->addr + t->width * 2 * rows > rdp.rdram_size)
				continue;
			if (!rdp_targets_readback(&rdp.tg, i, true))
				continue;
			rsxkit_rect src = { 0, 0, t->lw * rdp.scale, rows * rdp.scale };
			if (rsxkit_target_read(rdp.surf[i], &src, rdp.rb_off + nrb * RB_SLOT, RB_PITCH, t->lw, rows,
					       RSXKIT_RGB565) != RSXKIT_OK)
				continue;
			rdp.bound_t = -1;
			sync_rb[i] = true;
			rb[nrb].slot = i;
			rb[nrb].buf = nrb;
			rb[nrb].rows = rows;
			nrb++;
			break;
		}
	}
	/* Copias espejo de las imágenes auxiliares chicas dibujadas en la
	 * tarea (nunca presentadas, de otro ancho que el VI): la RDRAM las
	 * tendrá cuando un dibujo las lea ya viejas (Link en la pantalla de
	 * equipo de Zelda, 395 tareas después). No se espera: se convierten en
	 * una traducción posterior, cuando el RSX pasó la barrera. */
	int mirror_k[MIRRORS];
	uint32_t nmirror = 0;
	for (int i = 0; rdp.rdram && rdp.fbfx >= 2 && i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &rdp.tg.t[i];
		uint32_t rows = drawn_rows[i] < t->lh ? drawn_rows[i] : t->lh;
		uint32_t pitch = (t->lw * 2 + 63) & ~63u;
		if (!drawn[i] || sync_rb[i] || !rows || t->presented || t->bpp != 2 || t->width == rdp.tg.vi_width ||
		    pitch * rows > MIRROR_BYTES || t->addr + t->width * 2 * rows > rdp.rdram_size)
			continue;
		int k = 0;
		while (k < MIRRORS && rdp.mirrors[k].used)
			k++;
		if (k == MIRRORS) {
			rdp.stats.mirrors_skipped++;
			continue;
		}
		rsxkit_rect src = { 0, 0, t->lw * rdp.scale, rows * rdp.scale };
		if (rsxkit_target_read(rdp.surf[i], &src, rdp.rb_off + (RB_SLOTS + 1) * RB_SLOT + k * MIRROR_BYTES, pitch,
				       t->lw, rows, RSXKIT_RGB565) != RSXKIT_OK)
			continue;
		rdp.bound_t = -1;
		struct mirror *m = &rdp.mirrors[k];
		m->used = true;
		m->addr = t->addr;
		m->width = t->width;
		m->lw = t->lw;
		m->rows = rows;
		mirror_k[nmirror++] = k;
	}
	s->fence = rsxkit_fence();
	lwsync();
	s->fence_task = n + 1;
	for (uint32_t k = 0; k < nmirror; k++)
		rdp.mirrors[mirror_k[k]].fence = s->fence;
	if (nrb) {
		uint64_t t1 = sysGetSystemTime();
		rsxkit_fence_wait(s->fence);
		for (uint32_t k = 0; k < nrb; k++) {
			const struct rdp_target *t = &rdp.tg.t[rb[k].slot];
			to_rdram(rdp.rb + rb[k].buf * RB_SLOT, RB_PITCH, t->addr, t->width, t->lw, rb[k].rows);
			rdp.stats.readbacks++;
		}
		readbacks_finish(false);  /* pasó la barrera: los espejos también */
		rdp.stats.us_readback += sysGetSystemTime() - t1;
	}
	lwsync();  /* la RDRAM, antes de decir que está */
	rdp.readback_done = n + 1;
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
			 const struct salsa64_overscan *crop, rsxkit_rect *out)
{
	if (!rdp.ready)
		return false;
	struct rdp_present p;
	rdp_targets_present(&rdp.tg, f, field, &p);
	if (p.slot < 0 || !rdp.surf[p.slot] || !p.w || !p.h)
		return false;
	rdp.stats.age_sum += p.age;
	if (p.age > rdp.stats.age_max)
		rdp.stats.age_max = p.age;
	/* Recorte del overscan: la región se achica por los cuatro lados y se
	 * presenta como una imagen más chica (misma proporción de pantalla). */
	uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
	if (crop) {
		cl = crop->left, cr = crop->right, ct = crop->top, cb = crop->bottom;
		if (cl + cr >= p.w)
			cl = cr = 0;
		if (ct + cb >= p.h)
			ct = cb = 0;
	}
	uint32_t w = p.w - cl - cr, h = p.h - ct - cb;
	rsxkit_rect src = { (int32_t)(cl * rdp.scale), (int32_t)((p.y0 + ct) * rdp.scale), w * rdp.scale,
			    h * rdp.scale };
	/* Si la región cabe en la pantalla, a 1:1 (modo entero con su tamaño
	 * en píxeles de la superficie: factor 1, píxeles cuadrados, centrada;
	 * en 480/576 el píxel no es cuadrado y el ancho se estira igual que
	 * antes). Ajustarla a la pantalla estiraba el recorte del overscan
	 * (912 × 684 a 960 × 720, ×1,053) y el filtro lineal repetía una fila
	 * cada ~19: las líneas en el logo de N64 de Zelda. Lo que sobra queda
	 * negro, que es como se borra el cuadro. Sin recorte no cambia nada:
	 * 960 × 720 cabía justo. */
	const rsxkit_screen *sc = rsxkit_screen_info();
	if (view && view->scale == RSXKIT_SCALE_FIT && src.w <= sc->width && src.h <= sc->height) {
		rsxkit_view one = { RSXKIT_SCALE_INTEGER, view->filter, 0.0f };
		rsxkit_target_present(rdp.surf[p.slot], &src, src.w, src.h, &one, out);
		return true;
	}
	rsxkit_target_present(rdp.surf[p.slot], &src, w, h, view, out);
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
	rsxkit_fp_stats fp;
	rsxkit_fp_get_stats(&fp);
	out->fp_programs = fp.programs;
	out->fp_generated = fp.generated;
	out->fp_evictions = fp.evictions;
	out->fp_loads = fp.loads;
	out->tex_rsx = rdp.tg.tex_rsx;
	out->tex_rdram = rdp.tg.tex_rdram;
	out->tex_self = rdp.tg.tex_self;
}
