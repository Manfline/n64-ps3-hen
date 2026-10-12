/* SPDX-License-Identifier: GPL-2.0-or-later */
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
/* Espejo del framebuffer presentado, cada campo, sin esperar (Mario Kart
 * reduce el cuadro con el CPU para la pantalla del túnel): un búfer grande
 * más, después de los RB_SLOTS y el de cebado. */
#define FRAME_SLOT  (RB_SLOTS + 1)
#define FB_BYTES    (((RB_SLOTS + 2) * RB_SLOT + MIRRORS * MIRROR_BYTES + 0xFFFFFu) & ~0xFFFFFu)
#define POOL_BYTES  (SETS_BYTES + CACHE_BYTES + FB_BYTES)
#define MAX_DEPTHS  RDP_DEPTHS_MAX  /* targets.h: cuentan en rdp_vram_worst */
#define RB_PER_TASK 8   /* color images que una tarea puede pedir copiar */
/* Una imagen pedida también en la tarea anterior ya no se espera (iconos
 * de cada cuadro, la intro de Conker); con una ventana mayor, Link
 * redibujado al cambiar de equipo en Zelda caía como «repetida» y el CPU
 * lo copiaba antes de tiempo. */
#define RB_HISTORY  1
/* Tareas durante las que el espejo del framebuffer respeta la zona de una
 * imagen auxiliar recién copiada que se solapa con él (Link se dibuja en
 * los primeros 14 KB del framebuffer de Zelda y el CPU lo copia de ahí; el
 * filtro de la pausa lee la cobertura ~1 s). */
#define RB_KEEP_TASKS 32
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
_Static_assert(GFX_FB_FRESH == RDP_TARGETS_FRESH, "la HLE y la tabla cuentan igual la antigüedad de un destino");

struct set {
	uint8_t *vtx, *cmd, *tex;
	uint32_t vtx_off, tex_off;  /* offsets del RSX */
	uint32_t fence;             /* la pone el hilo de presentación al traducir */
	volatile uint32_t fence_task;  /* número de la tarea antes de `fence` */
	struct gfx_result res;      /* la deja done; la lee flush */
	/* Color images de la tarea que hay que copiar a la RDRAM al traducirla
	 * (lo decide done, en el hilo de emulación). rb_sync: la emulación
	 * espera esa copia (la imagen acaba de aparecer: el CPU la va a leer
	 * ya, como el fondo de la pausa de Zelda); si no, se copia sin esperar
	 * (la misma imagen se pide en tareas seguidas: un efecto por cuadro
	 * recibe el cuadro anterior, como la intro de Conker). */
	uint32_t rb_addr[RB_PER_TASK], rb_width[RB_PER_TASK];
	uint32_t rb_rep[RB_PER_TASK];  /* tareas seguidas en que se venía pidiendo */
	bool rb_sync[RB_PER_TASK];
	uint32_t rb_n, rb_nsync;
	bool trace;  /* tarea con traza detallada en el log (ver readbacks_wanted) */
};

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

/* Avisos al log del frontend, acotados por sitio para no inundarlo. */
static void (*rdp_log)(const char *fmt, ...);
#define LOGN(limit, ...) \
	do { \
		static unsigned n_; \
		if (rdp_log && n_ < (limit)) { \
			n_++; \
			rdp_log(__VA_ARGS__); \
		} \
	} while (0)

void salsa64_rdp_set_log(void (*fn)(const char *fmt, ...))
{
	rdp_log = fn;
}

struct depth {
	bool used;
	uint32_t addr, w, h;
	uint32_t bytes;              /* lo que cuenta en rdp.vram */
	bool clear_pending;
	float clear_rect[4];
	float clear_value;
	rsxkit_depth *d;
};

static struct {
	bool ready;
	/* Escala-objetivo (240 × scale líneas); cada superficie tiene la suya
	 * (surf_scale, rdp_target_scale) y todo lo que pasa de coordenadas
	 * lógicas a píxeles de una superficie usa la de esa superficie. */
	uint32_t scale;
	bool scale_reduced;          /* la escala se bajó por memoria */
	uint32_t vram;               /* bytes vivos: destinos, scratch y profundidades */
	uint32_t zeta_bytes;         /* profundidad de la pantalla agrandada (apply_scale), en el tope */
	uint32_t surf_bytes[RDP_TARGETS_MAX];
	uint32_t surf_scale[RDP_TARGETS_MAX];  /* escala de la superficie de cada ranura */
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
	uint32_t scratch_w, scratch_h, scratch_bytes;
	uint32_t scratch_scale;      /* escala de la superficie copiada (la del scratch) */
	int scratch_slot;            /* ranura copiada en esta tarea, o -1 */
	uint32_t ppe_tex_off;        /* arena de la tarea en traducción: hasta dónde está ocupado */
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
		uint32_t addr, width, lw, rows, fence, bpp;
	} mirrors[MIRRORS];
	struct mirror frame;         /* el espejo del framebuffer presentado */
	/* Última tarea en que cada ranura se copió a la RDRAM (o se rellenó
	 * su cobertura): el espejo del framebuffer no pisa esa zona mientras
	 * el CPU pueda estar leyéndola. */
	uint32_t rb_task[RDP_TARGETS_MAX];
	struct mirror big[RB_SLOTS]; /* copias grandes sin esperar, en los búferes de RB_SLOTS */
	/* Color images pedidas en las últimas RB_HISTORY tareas (hilo de
	 * emulación): una que se repite ya no se espera. */
	uint32_t rb_prev[RB_HISTORY][RB_PER_TASK], rb_prev_rep[RB_HISTORY][RB_PER_TASK];
	uint32_t rb_prev_n[RB_HISTORY];
	uint32_t rb_prev_i;
	/* Traza detallada (color images, ranuras y copias de cada tarea) de las
	 * 6 primeras tareas con copias tras 10 sin ellas (abrir una pausa),
	 * hasta 3 veces por corrida. */
	uint32_t quiet_tasks, trace_left, trace_events;
	struct { int slot; char how[48]; } last_get;  /* qué hizo get_target (traza) */
	/* Lo último presentado, para repetirlo tal cual en una pausa
	 * (salsa64_rdp_present_again); surf NULL: nada que repetir. */
	struct {
		rsxkit_target *surf;
		rsxkit_rect src;
		uint32_t w, h;
		rsxkit_view view;
	} again;
} rdp;

static void acquire(void *ctx, struct gfx_job *job);
static void done(void *ctx, const struct gfx_job *job, const struct gfx_result *res);
static const struct salsa64_gfx_sink sink = { NULL, acquire, done };

/* Escala-objetivo para la pantalla en curso y la profundidad de la
 * pantalla que la acompaña (ver rdp.h: un destino sin z image se liga con
 * ella y rsxkit_bind no liga uno más grande): se agranda ahora, fuera del
 * bucle, al mayor destino posible a esa escala (rdp_zeta_reserve), o
 * vuelve al tamaño de la pantalla si alcanza. Si el RSX no tiene memoria
 * para agrandarla, se baja la escala (a 1× ningún destino pasa de 640 × 480
 * y siempre alcanza). */
static void apply_scale(uint32_t requested)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	uint32_t s = rdp_scale_pick(requested, sc->height, &rdp.scale_reduced);
	for (;;) {
		uint32_t w, h;
		bool grow = rdp_zeta_reserve(s, sc->width, sc->height, &w, &h);
		if (rsxkit_depth_reserve(w, h) == RSXKIT_OK) {
			rdp.zeta_bytes = grow ? rdp_surface_bytes(w, h) : 0;
			break;
		}
		if (s <= 1) {
			rdp.zeta_bytes = 0;
			break;
		}
		s--;
		rdp.scale_reduced = true;
	}
	rdp.scale = s;
}

int salsa64_rdp_init(uint32_t scale)
{
	memset(&rdp, 0, sizeof rdp);
	rdp_targets_init(&rdp.tg);
	rdp.bound_t = -1;
	apply_scale(scale);
	rdp.scratch_slot = -1;
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
	rdp.again.surf = NULL;
	for (int i = 0; i < RDP_TARGETS_MAX; i++)
		if (rdp.surf[i])
			rsxkit_target_destroy(rdp.surf[i]);
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (rdp.depths[i].used)
			rsxkit_depth_destroy(rdp.depths[i].d);
	if (rdp.scratch)
		rsxkit_target_destroy(rdp.scratch);
	rdp.scratch = NULL;
	rdp.scratch_w = rdp.scratch_h = rdp.scratch_bytes = rdp.scratch_scale = 0;
	rdp.scratch_slot = -1;
	memset(rdp.surf, 0, sizeof rdp.surf);
	memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
	memset(rdp.surf_bytes, 0, sizeof rdp.surf_bytes);
	memset(rdp.surf_scale, 0, sizeof rdp.surf_scale);
	rdp.vram = 0;
	memset(rdp.depths, 0, sizeof rdp.depths);
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
	rdp_targets_init(&rdp.tg);  /* la tabla de destinos, vacía */
}

uint32_t salsa64_rdp_scale(bool *reduced)
{
	if (reduced)
		*reduced = rdp.scale_reduced;
	return rdp.scale;
}

void salsa64_rdp_set_scale(uint32_t scale)
{
	if (!rdp.ready)
		return;
	rsxkit_finish();
	/* Todo lo que tiene superficie o tamaño de una superficie se suelta:
	 * destinos, scratch, profundidades. Las copias en vuelo (espejos, el
	 * cuadro, las grandes sin esperar) viven en memoria principal y no
	 * dependen de la escala, pero son del juego anterior: se descartan. */
	release_targets();
	memset(rdp.mirrors, 0, sizeof rdp.mirrors);
	memset(&rdp.frame, 0, sizeof rdp.frame);
	memset(rdp.big, 0, sizeof rdp.big);
	memset(rdp.rb_task, 0, sizeof rdp.rb_task);
	apply_scale(scale);
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
	memset(&rdp.frame, 0, sizeof rdp.frame);
	memset(rdp.big, 0, sizeof rdp.big);
	memset(rdp.rb_prev_n, 0, sizeof rdp.rb_prev_n);
	memset(rdp.rb_task, 0, sizeof rdp.rb_task);
	memset(&rdp.vi_ring, 0, sizeof rdp.vi_ring);
	memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
	rdp.last_set = 0;
	memset(&rdp.last_res, 0, sizeof rdp.last_res);
	memset(&rdp.stats, 0, sizeof rdp.stats);
	return pending;
}

static void readbacks_finish(bool wait);

void salsa64_rdp_settle(void)
{
	if (!rdp.ready)
		return;
	rsxkit_finish();
	readbacks_finish(true);
}

void salsa64_rdp_drop_targets(void)
{
	if (!rdp.ready)
		return;
	rsxkit_finish();
	release_targets();
	/* Las copias que quedaran en vuelo y la memoria de lo copiado eran de la
	 * máquina anterior (salsa64_rdp_settle ya las vació): nada de eso va a
	 * la RDRAM cargada. La numeración de las tareas y readback_done siguen. */
	memset(rdp.mirrors, 0, sizeof rdp.mirrors);
	memset(&rdp.frame, 0, sizeof rdp.frame);
	memset(rdp.big, 0, sizeof rdp.big);
	memset(rdp.rb_task, 0, sizeof rdp.rb_task);
	memset(rdp.rb_prev, 0, sizeof rdp.rb_prev);
	memset(rdp.rb_prev_rep, 0, sizeof rdp.rb_prev_rep);
	memset(rdp.rb_prev_n, 0, sizeof rdp.rb_prev_n);
	rdp.rb_prev_i = 0;
	memset(&rdp.vi_ring, 0, sizeof rdp.vi_ring);
	rdp.scratch_slot = -1;
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
	/* Lo que viene de un destino reciente se lee del RSX: que la HLE no lo
	 * decodifique (texture_from_target y, si al final no se puede, la
	 * decodificación en el PPE). */
	if (rdp.fbfx >= 1)
		job->flags |= GFX_JOB_FBTEX;
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

/* Región lógica (x, y, w, h) del destino de la ranura, en píxeles de su
 * superficie: con la escala de ese destino (surf_scale), no la objetivo. */
static rsxkit_rect surface_rect(int slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	struct rdp_rect r;
	rdp_surface_rect(rdp.surf_scale[slot], x, y, w, h, &r);
	return (rsxkit_rect){ (int32_t)r.x, (int32_t)r.y, r.w, r.h };
}

/* Escribe en la RDRAM, como RGBA5551, `rows` filas de una copia RGB565 de
 * `lw` píxeles por fila con paso `pitch`. */
static void to_rdram(const uint8_t *src, uint32_t pitch, uint32_t addr, uint32_t width, uint32_t lw, uint32_t rows,
		     uint32_t bpp)
{
	/* El RSX escribió el búfer a espaldas de la caché del PPE: soltar las
	 * líneas que pudieran quedar de la lectura anterior. */
	for (uint32_t off = 0; off < rows * pitch; off += 128)
		__asm__ volatile("dcbf 0,%0" ::"r"(src + off) : "memory");
	__asm__ volatile("sync" ::: "memory");
	for (uint32_t y = 0; y < rows; y++) {
		const uint16_t *row = (const uint16_t *)(src + y * pitch);
		if (bpp == 1) {
			/* Color image de 8 bits (la cobertura que Zelda copia para el
			 * filtro de Link): la intensidad es el rojo del RGB565. */
			uint8_t *dst = rdp.rdram + addr + y * width;
			for (uint32_t x = 0; x < lw; x++)
				dst[x] = (uint8_t)((row[x] >> 8 & 0xF8) | (row[x] >> 13));
		} else {
			rsxkit_pixels_565_to_5551((uint16_t *)(rdp.rdram + addr + y * width * 2), row, lw);
		}
	}
}

/* Copias espejo cuya barrera ya pasó: a la RDRAM y el búfer queda libre. */
static void readbacks_finish(bool wait)
{
	struct mirror *f = &rdp.frame;
	if (f->used && (wait ? (rsxkit_fence_wait(f->fence), true) : rsxkit_fence_done(f->fence))) {
		to_rdram(rdp.rb + FRAME_SLOT * RB_SLOT, RB_PITCH, f->addr, f->width, f->lw, f->rows, 2);
		f->used = false;
		rdp.stats.frames_mirrored++;
	}
	for (int k = 0; k < RB_SLOTS; k++) {
		struct mirror *m = &rdp.big[k];
		if (m->used && (wait ? (rsxkit_fence_wait(m->fence), true) : rsxkit_fence_done(m->fence))) {
			to_rdram(rdp.rb + k * RB_SLOT, RB_PITCH, m->addr, m->width, m->lw, m->rows, m->bpp ? m->bpp : 2);
			m->used = false;
			rdp.stats.mirrors++;
		}
	}
	for (int k = 0; k < MIRRORS; k++) {
		struct mirror *m = &rdp.mirrors[k];
		if (!m->used)
			continue;
		if (wait)
			rsxkit_fence_wait(m->fence);
		else if (!rsxkit_fence_done(m->fence))
			continue;
		uint32_t pitch = (m->lw * 2 + 63) & ~63u;
		to_rdram(rdp.rb + (RB_SLOTS + 2) * RB_SLOT + k * MIRROR_BYTES, pitch, m->addr, m->width, m->lw, m->rows,
			 m->bpp ? m->bpp : 2);
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

/* Suelta el scratch (se vuelve a crear cuando un dibujo lo pida). */
static void drop_scratch(void)
{
	if (!rdp.scratch)
		return;
	rsxkit_target_destroy(rdp.scratch);
	rdp.scratch = NULL;
	rdp.vram -= rdp.scratch_bytes;
	rdp.scratch_w = rdp.scratch_h = rdp.scratch_bytes = rdp.scratch_scale = 0;
	rdp.scratch_slot = -1;
}

/* Suelta la superficie de una ranura (destruye y descuenta). */
static void drop_surface(int i)
{
	if (rdp.bound_t == i)
		rdp.bound_t = -1;
	if (rdp.scratch_slot == i)
		rdp.scratch_slot = -1;  /* el scratch era de esta superficie */
	if (rdp.again.surf == rdp.surf[i])
		rdp.again.surf = NULL;
	rsxkit_target_destroy(rdp.surf[i]);
	rdp.surf[i] = NULL;
	rdp.surf_dirty[i] = false;
	rdp.vram -= rdp.surf_bytes[i];
	rdp.surf_bytes[i] = 0;
	rdp.surf_scale[i] = 0;
}

/* Hace sitio para `bytes` más bajo el tope RDP_VRAM_BUDGET: primero suelta
 * el scratch si no es de `keep` y después recicla el destino más viejo
 * (menos campos desde que se dibujó), sin tocar `keep`, el presentado en el
 * último campo ni uno dibujado hace RDP_TARGETS_FRESH tareas o menos (puede
 * ser la textura del dibujo que se está armando). false: ni así cabe. Con
 * el peor caso de rdp_vram_worst dentro del tope (rdp_scale_pick lo
 * asegura) nunca llega a reciclar: es la red si cambian el tope o el
 * máximo de superficies. */
static bool make_room(uint32_t bytes, int keep)
{
	if (rdp.vram + rdp.zeta_bytes + bytes > RDP_VRAM_BUDGET && rdp.scratch && rdp.scratch_slot != keep)
		drop_scratch();
	while (rdp.vram + rdp.zeta_bytes + bytes > RDP_VRAM_BUDGET) {
		int victim = -1;
		for (int j = 0; j < RDP_TARGETS_MAX; j++) {
			if (!rdp.surf[j] || j == keep || j == rdp.tg.shown ||
			    rdp.tg.task - rdp.tg.t[j].last_task <= RDP_TARGETS_FRESH)
				continue;
			if (victim < 0 || (int32_t)(rdp.tg.t[j].last_field - rdp.tg.t[victim].last_field) < 0)
				victim = j;
		}
		if (victim < 0)
			return false;
		drop_surface(victim);
		rdp.tg.t[victim].used = false;
		rdp.stats.targets_evicted++;
	}
	return true;
}

/* Ranura (con superficie) de una color image; -1 si el RSX no tiene
 * memoria para una nueva (esa imagen se queda en la copia de la RDRAM). La
 * superficie va a la escala del destino (rdp_target_scale), que queda en
 * surf_scale. */
static int get_target(uint32_t addr, uint32_t width, uint32_t fmt, uint32_t siz, uint32_t min_lh)
{
	bool fresh;
	rdp.last_get.slot = -1;
	/* Una color image imposible (la HLE la sacó de un comando mal
	 * interpretado: Zelda al abrir la pausa manda anchos de 1495 y 3770) no
	 * toca la tabla: reciclaría una ranura buena para nada. */
	if (!width || width > 640 || min_lh > 480) {
		rdp.stats.targets_absurd++;
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "imposible, ignorada");
		LOGN(10, "gfx: color image imposible %06x/%u (alto %u): se ignora", addr, width, min_lh);
		return -1;
	}
	struct { bool used; uint32_t addr, width; } before[RDP_TARGETS_MAX];
	for (int k = 0; k < RDP_TARGETS_MAX; k++) {
		before[k].used = rdp.tg.t[k].used;
		before[k].addr = rdp.tg.t[k].addr;
		before[k].width = rdp.tg.t[k].width;
	}
	int i = rdp_targets_get(&rdp.tg, addr, width, siz, min_lh, &fresh);
	rdp.last_get.slot = i;
	if (!fresh) {
		/* Sin dibujar desde hace tareas: lo que vale está en la RDRAM. */
		bool stale = rdp.tg.task - rdp.tg.t[i].last_task > RDP_TARGETS_FRESH;
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, stale ? "cebada" : "viva");
		if (stale)
			prime(i);
		return i;
	}
	if (before[i].used && (before[i].addr != addr || before[i].width != width))
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "nueva, recicla %06x/%u", before[i].addr,
			 before[i].width);
	else
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, before[i].used ? "crece" : "nueva");
	if (rdp.surf[i])
		drop_surface(i);
	const struct rdp_target *t = &rdp.tg.t[i];
	uint32_t sc = rdp_target_scale(rdp.scale, t->lw, t->lh);
	uint32_t bytes = rdp_surface_bytes(t->lw * sc, t->lh * sc);
	if (make_room(bytes, i))
		rdp.surf[i] = rsxkit_target_create(t->lw * sc, t->lh * sc);
	if (!rdp.surf[i]) {
		rdp.stats.targets_failed++;
		LOGN(20, "gfx: destino %06x/%u: el RSX no dio una superficie de %u × %u (%u×; %u KB vivos)", addr, width,
		     t->lw * sc, t->lh * sc, sc, rdp.vram >> 10);
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "SIN SUPERFICIE");
		rdp.tg.t[i].used = false;
		return -1;
	}
	rdp.surf_bytes[i] = bytes;
	rdp.surf_scale[i] = sc;
	rdp.vram += bytes;
	rdp.surf_dirty[i] = false;
	rdp.tg.t[i].fmt = (uint8_t)fmt;
	rdp.stats.targets_created++;
	prime(i);
	return i;
}

/* Profundidad de la z image `addr` para dibujar en el destino `slot`. Va
 * en píxeles de la superficie de ese destino (su escala: rsxkit_bind la
 * pide al menos tan grande como el destino). Si la z image ya tiene una
 * que no alcanza (la usó un destino de otra escala o de otro tamaño), se
 * rehace en la misma entrada con el mayor de los dos tamaños: así no quedan
 * dos con la misma dirección (zclear borraría la que no es) ni se destruye
 * y crea a cada cambio de destino. Lo más que crece es lo que pesa el mayor
 * destino posible (rdp_vram_worst). */
static struct depth *get_depth(uint32_t addr, int slot)
{
	const struct rdp_target *t = &rdp.tg.t[slot];
	uint32_t sc = rdp.surf_scale[slot] ? rdp.surf_scale[slot] : 1;
	uint32_t w = t->lw * sc, h = t->lh * sc;
	struct depth *same = NULL, *free_d = NULL;
	for (int i = 0; i < MAX_DEPTHS; i++) {
		struct depth *d = &rdp.depths[i];
		if (d->used && d->addr == addr) {
			if (d->w >= w && d->h >= h)
				return d;
			same = d;
		}
		if (!d->used && !free_d)
			free_d = d;
	}
	struct depth *d = same ? same : free_d ? free_d : &rdp.depths[addr & 1];
	if (same) {
		w = same->w > w ? same->w : w;
		h = same->h > h ? same->h : h;
	}
	if (d->used) {
		if (rdp.bound_d == d)
			rdp.bound_d = NULL;
		rsxkit_depth_destroy(d->d);
		d->used = false;
		rdp.vram -= d->bytes;
		d->bytes = 0;
	}
	uint32_t bytes = rdp_surface_bytes(w, h);
	d->d = make_room(bytes, slot) ? rsxkit_depth_create(w, h) : NULL;
	if (!d->d)
		return NULL;
	d->bytes = bytes;
	rdp.vram += bytes;
	d->used = true;
	d->addr = addr;
	d->w = w;
	d->h = h;
	d->clear_pending = false;
	return d;
}

/* Liga el destino de la ranura con la profundidad `d` (NULL: sin z image,
 * con la de la pantalla, que apply_scale dejó del tamaño del mayor destino).
 * false si rsxkit no lo ligó (ninguna zeta lo cubre): el dibujo se salta en
 * vez de ir con una zeta más chica que el color. */
static bool bind(int slot, struct depth *d)
{
	if (slot == rdp.bound_t && d == rdp.bound_d)
		return true;
	const struct rdp_target *t = &rdp.tg.t[slot];
	int rc = rsxkit_bind(rdp.surf[slot], d ? d->d : NULL, (float)t->lw, (float)t->lh);
	if (rc != RSXKIT_OK) {
		rdp.bound_t = -1;
		rdp.bound_d = NULL;
		rdp.stats.binds_failed++;
		LOGN(10, "gfx: destino %06x/%u no se ligó (%d): ninguna profundidad cubre su superficie", t->addr, t->width,
		     rc);
		return false;
	}
	rdp.bound_t = slot;
	rdp.bound_d = d;
	if (d && d->clear_pending) {
		rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, d->clear_value, d->clear_rect);
		d->clear_pending = false;
	}
	return true;
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
 * superficie (a la escala de ese destino, que puede no ser la del destino
 * en que se dibuja: u y v van normalizadas, así que solo las filas ligadas
 * dependen de la escala, rdp_target_tex_map) en vez de la copia
 * decodificada de la RDRAM (*bound). Devuelve true si hay que sincronizar
 * antes del dibujo (la superficie se dibujó desde la última vez). */
static bool texture_from_target(const struct gfx_tex *gt, int cur, rsxkit_texture *t, bool *bound)
{
	*bound = false;
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
	uint32_t sc = rdp.surf_scale[slot];
	bool copied = false;
	if (src == RDP_TEXSRC_SELF) {
		/* Lee lo que está dibujando: una copia de la superficie tal como
		 * está ahora (una por tarea; las franjas de Zelda leen cada una
		 * filas que aún no reescribió). El scratch toma el tamaño, y con él
		 * la escala, de la superficie copiada. */
		if (rdp.scratch_slot != slot) {
			uint32_t w, h;
			rsxkit_target_size(rdp.surf[slot], &w, &h);
			if (rdp.scratch && (rdp.scratch_w != w || rdp.scratch_h != h))
				drop_scratch();
			if (!rdp.scratch) {
				uint32_t bytes = rdp_surface_bytes(w, h);
				if (make_room(bytes, slot))
					rdp.scratch = rsxkit_target_create(w, h);
				if (rdp.scratch) {
					rdp.scratch_w = w;
					rdp.scratch_h = h;
					rdp.scratch_bytes = bytes;
					rdp.vram += bytes;
				}
			}
			if (!rdp.scratch || rsxkit_target_copy(rdp.scratch, rdp.surf[slot]) != RSXKIT_OK)
				return false;
			rdp.bound_t = -1;  /* rsxkit_target_copy dejó ligado el scratch */
			rdp.scratch_slot = slot;
			rdp.scratch_scale = sc;
			copied = true;
		}
		from = rdp.scratch;
		sc = rdp.scratch_scale;
	}
	struct rdp_tex_map map;
	rdp_target_tex_map(sc, tg->lw, x0, y0, gt->width, gt->height, gt->xf[0], gt->xf[2], &map);
	rsxkit_texture nt;
	rsxkit_target_texture(from, map.row0, map.rows, &nt);
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
	t->xf[0] = map.xf0;
	t->xf[2] = map.xf2;
	*bound = true;
	/* El primer dibujo que lee el scratch recién copiado también tiene que
	 * esperar al RSX e invalidar su caché de texturas (si no, un cuadro con
	 * basura: el haz verde al abrir la pausa de Zelda en la PS3). */
	return copied || (src == RDP_TEXSRC_RSX && rdp.surf_dirty[slot]);
}

/* G_RM_VISCVG: en vez de dibujar, deja en la RDRAM «cobertura plena» en
 * las filas del rectángulo (el CPU de Zelda salta los píxeles cubiertos
 * del todo al filtrar el fondo de la pausa). */
static void viscvg(int cur, const struct gfx_draw *g)
{
	const struct rdp_target *t = &rdp.tg.t[cur];
	if (!rdp.rdram || rdp.fbfx < 2 || t->bpp != 2)
		return;
	rdp.rb_task[cur] = rdp.tg.task;
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

/* Una textura que la HLE dejó para leer de un destino (GFX_TEX_TARGET) y
 * que no se pudo leer de él (para la tabla el destino es viejo, es del CPU
 * o no tiene superficie): se decodifica de la RDRAM en el arena de la
 * tarea, detrás de lo que escribió la HLE (el arena casi no se usa con la
 * caché de texturas). false si no cabe. */
static bool texture_from_rdram(const struct gfx_tex *gt, struct set *s, rsxkit_texture *t)
{
	uint32_t off = (rdp.ppe_tex_off + 127) & ~127u, size = (uint32_t)gt->pitch * gt->height;
	if (!rdp.rdram || off + size > TEX_BYTES) {
		rdp.stats.tex_ppe_lost++;
		LOGN(10, "gfx: textura %ux%u desde %06x/%u: no cabe en el arena para decodificarla en el PPE", gt->width,
		     gt->height, gt->src_addr, gt->src_pitch);
		return false;
	}
	rdp_decode_rgba16(rdp.rdram, rdp.rdram_size, gt->src_addr, gt->src_pitch, gt->width, gt->height, s->tex + off,
			  gt->pitch);
	rdp.ppe_tex_off = off + size;
	t->offset = s->tex_off + off;
	rdp.stats.tex_ppe++;
	return true;
}

/* false: el dibujo no se puede hacer (una textura que no hubo dónde
 * decodificar). */
static bool to_rsxkit(const struct gfx_draw *g, struct set *s, int cur, rsxkit_draw *d, bool *sync)
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
		bool from_target = false;
		if (texture_from_target(gt, cur, t, &from_target))
			*sync = true;
		if (gt->where == GFX_TEX_TARGET && !from_target && !texture_from_rdram(gt, s, t))
			return false;
	}
	memcpy(d->k, g->k, sizeof d->k);
	return true;
}

/* Color images auxiliares de 16 bits de la tarea (las que no son un
 * framebuffer reciente del VI): se copian a la RDRAM al traducirla. La
 * emulación espera aquí la copia de las que acaban de aparecer (el CPU las
 * suele leer en cuanto llega la interrupción del DP: el fondo de la pausa
 * de Zelda, o Link recién dibujado que el CPU copia a otra parte); las que
 * se repiten en RB_HISTORY tareas van sin esperar (iconos de cada cuadro,
 * la intro de Conker). Solo recorre las cabeceras de los registros. */
/* ¿Algún dibujo de la tarea lee como textura la color image (addr, width)?
 * Si ninguno la lee, el único lector posible es el CPU. Mira el origen de
 * las dos texturas de cada dibujo (gfx_tex.src_addr) contra la imagen con
 * el alto de siempre (240 filas). */
static bool rdp_reads_image(const struct set *s, const struct gfx_result *res, uint32_t addr, uint32_t width)
{
	uint32_t lo = addr, hi = addr + width * 2 * 240;
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW && c->size >= sizeof(struct gfx_draw)) {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			for (int i = 0; i < g->ntex && i < 2; i++)
				if (g->tex[i].src_addr >= lo && g->tex[i].src_addr < hi)
					return true;
		}
		off += c->size;
	}
	return false;
}

static void readbacks_wanted(struct set *s, const struct gfx_result *res)
{
	s->rb_n = s->rb_nsync = 0;
	s->trace = false;
	if (!rdp.rdram || rdp.fbfx < 2) {
		memset(rdp.rb_prev_n, 0, sizeof rdp.rb_prev_n);
		return;
	}
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		/* Auxiliar: de otro ancho que el VI (aunque esté en la dirección
		 * del framebuffer: Zelda dibuja a Link en sus primeros bytes con
		 * ancho 64) o, del ancho del VI, fuera de los framebuffers
		 * recientes. */
		uint32_t vi_w = rdp.vi_ring.n ? rdp.vi_ring.width[(rdp.vi_ring.n - 1) % RDP_VI_RING] : 0;
		uint32_t siz = c->c & 0xFF, fmt = (c->c >> 8) & 0xFF;
		/* De 16 bits, o de 8 de intensidad (la cobertura que Zelda guarda
		 * para su filtro; una CI8 no se puede reconstruir desde el RSX). */
		if (c->op == GFX_OP_TARGET && (siz == 2u || (siz == 1u && fmt == 4u)) && c->b && c->b <= 640 &&
		    rdp.vi_ring.n && (c->b != vi_w || !rdp_vi_ring_covers(&rdp.vi_ring, c->a, c->b))) {
			uint32_t k = 0;
			while (k < s->rb_n && s->rb_addr[k] != c->a)
				k++;
			if (k == s->rb_n && k < RB_PER_TASK) {
				uint32_t rep = 0;  /* tareas seguidas en que se venía pidiendo */
				for (uint32_t h = 0; h < RB_HISTORY; h++)
					for (uint32_t j = 0; j < rdp.rb_prev_n[h]; j++)
						if (rdp.rb_prev[h][j] == c->a)
							rep = rdp.rb_prev_rep[h][j] + 1;
				bool again = rep > 0;
				/* Se espera si acaba de aparecer y, si es chica y ningún
				 * dibujo la lee (Link redibujado cada tarea de la pausa de
				 * Zelda, que el CPU podría copiar), también la segunda vez;
				 * de ahí en adelante va sin esperar y el CPU recibe la de la
				 * tarea anterior (esperarla costaba ~22 ms por tarea). Las
				 * grandes repetidas nunca se esperan (la intro de Conker). */
				bool small = c->b * (siz == 1u ? 1 : 2) * 240 <= MIRROR_BYTES;  /* 64 de ancho sí; 320, no */
				bool sync = !again || (small && rep < 2 && !rdp_reads_image(s, res, c->a, c->b));
				s->rb_rep[k] = rep;
				s->rb_sync[k] = sync;
				s->rb_nsync += sync;
				s->rb_width[s->rb_n] = c->b;
				s->rb_addr[s->rb_n++] = c->a;
			}
		}
		off += c->size;
	}
	if (s->rb_n) {
		if (rdp.quiet_tasks >= 10 && rdp.trace_events < 3) {
			rdp.trace_left = 6;
			rdp.trace_events++;
		}
		rdp.quiet_tasks = 0;
	} else {
		rdp.quiet_tasks++;
	}
	s->trace = rdp.trace_left > 0;
	if (s->trace)
		rdp.trace_left--;
	rdp.rb_prev_i = (rdp.rb_prev_i + 1) % RB_HISTORY;
	memcpy(rdp.rb_prev[rdp.rb_prev_i], s->rb_addr, sizeof rdp.rb_prev[0]);
	memcpy(rdp.rb_prev_rep[rdp.rb_prev_i], s->rb_rep, sizeof rdp.rb_prev_rep[0]);
	rdp.rb_prev_n[rdp.rb_prev_i] = s->rb_n;
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
	if (!s->rb_nsync)
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

static void frame_mirror(int slot, uint32_t rows, bool wait);

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
	rdp.ppe_tex_off = res->tex_bytes;
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
			cur = get_target(c->a, c->b, (c->c >> 8) & 0xFF, c->c & 0xFF,
					 scissor_bottom(s, off + c->size, res->cmd_bytes));
			if (s->trace && rdp_log)
				rdp_log("gfx: fb tarea %u: color image %06x/%u siz %u → ranura %d (%s)", n, c->a, c->b,
					c->c & 0xFF, cur, rdp.last_get.how);
			break;
		case GFX_OP_ZCLEAR:
			zclear((const struct gfx_zclear *)c);
			break;
		case GFX_OP_DRAW: {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			if (cur < 0 || !g->h.b)
				break;
			bool sync;
			if (g->viscvg) {
				/* Cobertura plena en la RDRAM y, en la superficie, blanco
				 * (el RDP copia después esa «cobertura» a otra imagen y el
				 * CPU la lee: Link en la pausa de Zelda). */
				viscvg(cur, g);
				struct gfx_draw w = *g;
				w.ntex = 0;
				w.nterms = 0;
				w.blend = GFX_BLEND_OFF;
				w.atest = 0;
				w.fog = 0;
				w.depth = 0;
				memset(w.k, 0, sizeof w.k);
				for (int lane = 0; lane < 4; lane++)
					w.k[GFX_K_ONE][0][lane] = 1.0f;
				to_rsxkit(&w, s, cur, &rd, &sync);
			} else if (!to_rsxkit(g, s, cur, &rd, &sync)) {
				break;
			}
			if (sync) {
				/* Lee una superficie dibujada en este mismo tramo. */
				rsxkit_texture_sync();
				memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
			}
			struct depth *d = rdp.bound_t == cur ? rdp.bound_d : NULL;
			if (g->depth)
				d = get_depth(g->zimg, cur);
			if (!bind(cur, d))
				break;
			rdp_targets_drawn(&rdp.tg, cur);
			drawn[cur] = true;
			rdp.surf_dirty[cur] = true;
			/* Hasta qué fila llega el dibujo: lo más bajo de sus vértices,
			 * limitado por las tijeras (las copias a la RDRAM escriben solo
			 * eso: el alto del destino es una conjetura y pasarse pisaría el
			 * heap del juego, como detrás del búfer de Link en Zelda). */
			{
				const struct gfx_vertex *v = (const struct gfx_vertex *)(s->vtx) + g->h.a;
				float ymax = 0.0f;
				for (uint32_t k = 0; k < g->h.b; k++)
					if (v[k].w > 0.0f && v[k].y / v[k].w > ymax)
						ymax = v[k].y / v[k].w;
				uint32_t rows = (uint32_t)(ymax + 0.999f), bottom = (g->scissor[3] + 3) / 4;
				if (rows > bottom)
					rows = bottom;
				if (rows > drawn_rows[cur])
					drawn_rows[cur] = rows;
			}
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
	uint32_t big_used = 0;  /* búferes grandes tomados en esta tarea */
	for (int k = 0; k < RB_SLOTS; k++)
		big_used |= rdp.big[k].used << k;
	for (uint32_t k = 0; k < s->rb_n; k++) {
		bool handled = false;
		for (int i = 0; i < RDP_TARGETS_MAX; i++) {
			const struct rdp_target *t = &rdp.tg.t[i];
			uint32_t rows = drawn_rows[i] < t->lh ? drawn_rows[i] : t->lh;
			/* La misma dirección puede ser dos color images de anchos
			 * distintos (Link de Zelda en los primeros bytes del framebuffer):
			 * la pedida es la de ese ancho. */
			if (!t->used || t->addr != s->rb_addr[k] || t->width != s->rb_width[k])
				continue;
			if (!drawn[i] || !rows || (t->bpp != 2 && t->bpp != 1) || t->lw > RB_MAX_W || t->lh > RB_MAX_H ||
			    t->addr + t->width * t->bpp * rows > rdp.rdram_size) {
				if (s->trace && rdp_log)
					rdp_log("gfx: fb tarea %u: copia %06x/%u ranura %d descartada (dibujada %d, %u filas, %u bpp)",
						n, t->addr, t->width, i, drawn[i], rows, t->bpp);
				continue;
			}
			/* Repetida y chica: espejo chico sin esperar (abajo). */
			uint32_t pitch_small = (t->lw * 2 + 63) & ~63u;
			if (!s->rb_sync[k] && t->width != rdp.tg.vi_width && pitch_small * rows <= MIRROR_BYTES) {
				handled = true;
				break;
			}
			/* Un búfer grande libre. Si los dos están tomados por copias
			 * esperadas de esta misma tarea, su lectura aún no pasó por el
			 * RSX (la barrera va al final): se espera una barrera y se
			 * convierten antes de reutilizar el búfer (antes la tercera
			 * copia esperada de una tarea pisaba la primera). */
			int buf = -1;
			for (int b = 0; b < RB_SLOTS && buf < 0; b++)
				if (!(big_used & (1u << b)))
					buf = b;
			if (buf < 0) {
				if (nrb) {
					rsxkit_fence_wait(rsxkit_fence());
					for (uint32_t j = 0; j < nrb; j++) {
						const struct rdp_target *tj = &rdp.tg.t[rb[j].slot];
						to_rdram(rdp.rb + rb[j].buf * RB_SLOT, RB_PITCH, tj->addr, tj->width, tj->lw, rb[j].rows,
							 tj->bpp);
						rdp.stats.readbacks++;
					}
					nrb = 0;
				}
				readbacks_finish(true);
				big_used = 0;
				buf = 0;
			}
			if (!rdp_targets_readback(&rdp.tg, i, true))
				continue;
			/* El RSX reduce a 1× desde la escala de este destino. */
			rsxkit_rect src = surface_rect(i, 0, 0, t->lw, rows);
			if (rsxkit_target_read(rdp.surf[i], &src, rdp.rb_off + buf * RB_SLOT, RB_PITCH, t->lw, rows,
					       RSXKIT_RGB565) != RSXKIT_OK) {
				LOGN(10, "gfx: copia a la RDRAM de %06x/%u: rsxkit_target_read falló", t->addr, t->width);
				continue;
			}
			rdp.bound_t = -1;
			big_used |= 1u << buf;
			sync_rb[i] = true;
			rdp.rb_task[i] = rdp.tg.task;
			handled = true;
			if (s->trace && rdp_log)
				rdp_log("gfx: fb tarea %u: copia %06x/%u ranura %d %s, %u filas, búfer %d", n, t->addr, t->width,
					i, s->rb_sync[k] ? "esperada" : "sin esperar", rows, buf);
			if (s->rb_sync[k]) {
				rb[nrb].slot = i;
				rb[nrb].buf = buf;
				rb[nrb].rows = rows;
				nrb++;
			} else {
				struct mirror *m = &rdp.big[buf];
				m->used = true;
				m->addr = t->addr;
				m->width = t->width;
				m->lw = t->lw;
				m->rows = rows;
				m->bpp = t->bpp;
				m->fence = 0;  /* se pone con la barrera de la tarea */
			}
			break;
		}
		if (!handled) {
			rdp.stats.readbacks_lost++;
			if (s->trace && rdp_log)
				rdp_log("gfx: fb tarea %u: copia %06x/%u %s PERDIDA: sin ranura dibujada", n, s->rb_addr[k],
					s->rb_width[k], s->rb_sync[k] ? "esperada" : "sin esperar");
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
		if (!drawn[i] || sync_rb[i] || !rows || t->presented || (t->bpp != 2 && (t->bpp != 1 || t->fmt != 4)) ||
		    t->width == rdp.tg.vi_width || pitch * rows > MIRROR_BYTES ||
		    t->addr + t->width * t->bpp * rows > rdp.rdram_size)
			continue;
		int k = 0;
		while (k < MIRRORS && rdp.mirrors[k].used)
			k++;
		if (k == MIRRORS) {
			rdp.stats.mirrors_skipped++;
			continue;
		}
		rsxkit_rect src = surface_rect(i, 0, 0, t->lw, rows);
		if (rsxkit_target_read(rdp.surf[i], &src, rdp.rb_off + (RB_SLOTS + 2) * RB_SLOT + k * MIRROR_BYTES, pitch,
				       t->lw, rows, RSXKIT_RGB565) != RSXKIT_OK)
			continue;
		rdp.bound_t = -1;
		rdp.rb_task[i] = rdp.tg.task;
		struct mirror *m = &rdp.mirrors[k];
		m->used = true;
		m->addr = t->addr;
		m->width = t->width;
		m->lw = t->lw;
		m->rows = rows;
		m->bpp = t->bpp;
		mirror_k[nmirror++] = k;
	}
	if (s->trace && rdp_log) {
		char line[400];
		int len = snprintf(line, sizeof line, "gfx: fb tarea %u: espejos %u, tabla:", n, nmirror);
		for (int i = 0; i < RDP_TARGETS_MAX && len < (int)sizeof line; i++) {
			const struct rdp_target *t = &rdp.tg.t[i];
			if (t->used)
				len += snprintf(line + len, sizeof line - len, " r%d %06x/%u lh%u t-%u%s%s%s", i, t->addr, t->width,
						t->lh, rdp.tg.task - t->last_task, t->presented ? " vi" : "",
						t->cpu_owned ? " cpu" : "", rdp.surf[i] ? "" : " SIN-SUP");
		}
		rdp_log("%s", line);
	}
	s->fence = rsxkit_fence();
	lwsync();
	s->fence_task = n + 1;
	for (uint32_t k = 0; k < nmirror; k++)
		rdp.mirrors[mirror_k[k]].fence = s->fence;
	for (int k = 0; k < RB_SLOTS; k++)
		if (rdp.big[k].used && !rdp.big[k].fence)
			rdp.big[k].fence = s->fence;
	if (nrb) {
		uint64_t t1 = sysGetSystemTime();
		rsxkit_fence_wait(s->fence);
		for (uint32_t k = 0; k < nrb; k++) {
			const struct rdp_target *t = &rdp.tg.t[rb[k].slot];
			to_rdram(rdp.rb + rb[k].buf * RB_SLOT, RB_PITCH, t->addr, t->width, t->lw, rb[k].rows, t->bpp);
			rdp.stats.readbacks++;
		}
		readbacks_finish(false);  /* pasó la barrera: los espejos también */
		rdp.stats.us_readback += sysGetSystemTime() - t1;
	}
	/* Espejo de los framebuffers que dibujó la tarea (el VI ya los mostró
	 * alguna vez): ahora, como el RDP, y no al presentarlos. Va después de
	 * poner las barreras de las copias de arriba (readbacks_finish no debe
	 * ver una sin barrera). */
	for (int i = 0; rdp.rdram && rdp.fbfx >= 2 && i < RDP_TARGETS_MAX; i++) {
		uint32_t rows = drawn_rows[i] < rdp.tg.t[i].lh ? drawn_rows[i] : rdp.tg.t[i].lh;
		if (drawn[i] && !sync_rb[i] && rdp_targets_mirror_at_task_end(&rdp.tg, i))
			frame_mirror(i, rows, true);
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

/* Espejo de un framebuffer: el RSX lo reduce a un búfer y, pasada su
 * barrera (readbacks_finish, en una traducción o presentación posterior),
 * el PPE lo escribe en la RDRAM. Así lo que el CPU lea del framebuffer
 * (Mario Kart reduce el cuadro para la pantalla del túnel) es el cuadro,
 * no basura. Una sola vez por dibujo (targets.h): al terminar la tarea que
 * lo dibujó (`wait`: si el espejo anterior aún no pasó su barrera, se
 * espera) o, si entonces no se sabía que era un framebuffer, al
 * presentarlo (solo las filas que muestra el VI; sin esperar: si el espejo
 * anterior aún no pasó la barrera, este campo se salta). Copiar en cada
 * campo, o al presentar, un cuadro que el RDP dibujó antes pisaba lo que
 * el juego cargó por DMA después en esa memoria (SM64 al pasar del logo a
 * la cara de Mario se colgaba). */
static void frame_mirror(int slot, uint32_t rows, bool wait)
{
	const struct rdp_target *t = &rdp.tg.t[slot];
	readbacks_finish(false);
	if (wait && rdp.frame.used) {
		rsxkit_fence_wait(rdp.frame.fence);
		readbacks_finish(false);
	}
	if (rdp.frame.used || !rdp.rdram || rdp.fbfx < 2 || !rdp.surf[slot] || t->bpp != 2 || t->lw > RB_MAX_W ||
	    rows > RB_MAX_H || !rows || t->addr + t->width * 2 * rows > rdp.rdram_size ||
	    !rdp_targets_mirror_wanted(&rdp.tg, slot))
		return;
	if (rows > t->lh)
		rows = t->lh;
	/* No pisar lo que una copia reciente dejó en esa misma memoria (Link
	 * en los primeros bytes del framebuffer; la cobertura de VISCVG). */
	uint32_t lo = t->addr, hi = t->addr + t->width * 2 * rows;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *a = &rdp.tg.t[i];
		if (!a->used || rdp.tg.task - rdp.rb_task[i] > RB_KEEP_TASKS)
			continue;
		uint32_t alo = a->addr, ahi = a->addr + a->width * a->bpp * a->lh;
		if (alo < hi && ahi > lo)
			return;
	}
	rsxkit_rect src = surface_rect(slot, 0, 0, t->lw, rows);
	if (rsxkit_target_read(rdp.surf[slot], &src, rdp.rb_off + FRAME_SLOT * RB_SLOT, RB_PITCH, t->lw, rows,
			       RSXKIT_RGB565) != RSXKIT_OK)
		return;
	rdp.bound_t = -1;
	rdp_targets_mirrored(&rdp.tg, slot);
	rdp.frame.used = true;
	rdp.frame.addr = t->addr;
	rdp.frame.width = t->width;
	rdp.frame.lw = t->lw;
	rdp.frame.rows = rows;
	rdp.frame.fence = rsxkit_fence();
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
	rsxkit_rect src = surface_rect(p.slot, cl, p.y0 + ct, w, h);
	/* Si la región cabe en la pantalla, a 1:1 (modo entero con su tamaño
	 * en píxeles de la superficie: factor 1, píxeles cuadrados, centrada;
	 * en 480/576 el píxel no es cuadrado y el ancho se estira igual que
	 * antes). Ajustarla a la pantalla estiraba el recorte del overscan
	 * (912 × 684 a 960 × 720, ×1,053) y el filtro lineal repetía una fila
	 * cada ~19: las líneas en el logo de N64 de Zelda. Lo que sobra queda
	 * negro, que es como se borra el cuadro. Sin recorte no cambia nada:
	 * 960 × 720 cabía justo. Con la escala por destino la región puede
	 * quedar muy por debajo de la pantalla (640 × 480 a 2×, o un
	 * framebuffer de 640 × 480 a 1× con la escala-objetivo 3×, en 720p):
	 * entonces se ajusta como siempre (rdp_present_integer). */
	const rsxkit_screen *sc = rsxkit_screen_info();
	rdp.again.surf = rdp.surf[p.slot];
	rdp.again.src = src;
	if (view && view->scale == RSXKIT_SCALE_FIT && rdp_present_integer(src.w, src.h, sc->width, sc->height)) {
		rsxkit_view one = { RSXKIT_SCALE_INTEGER, view->filter, 0.0f };
		rdp.again.w = src.w;
		rdp.again.h = src.h;
		rdp.again.view = one;
	} else {
		rdp.again.w = w;
		rdp.again.h = h;
		rdp.again.view = view ? *view : (rsxkit_view){ RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 0.0f };
	}
	rsxkit_target_present(rdp.again.surf, &rdp.again.src, rdp.again.w, rdp.again.h, &rdp.again.view, out);
	frame_mirror(p.slot, p.y0 + p.h, false);
	return true;
}

bool salsa64_rdp_present_again(rsxkit_rect *out)
{
	if (!rdp.ready || !rdp.again.surf)
		return false;
	rsxkit_target_present(rdp.again.surf, &rdp.again.src, rdp.again.w, rdp.again.h, &rdp.again.view, out);
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
	const struct gfx_tex **tgt = malloc(max * sizeof *tgt);
	uint8_t *here = malloc(GFX_TEX_MAX);
	if (!cmd || !src || !tgt || !here) {
		free(cmd);
		free(src);
		free(tgt);
		free(here);
		return false;
	}
	memcpy(cmd, s->cmd, r->cmd_bytes);
	/* Las texturas de la caché se copian detrás del arena (una vez cada
	 * una) y los dibujos pasan a apuntar ahí; las que se leían de un destino
	 * (GFX_TEX_TARGET), decodificadas de la RDRAM de ahora. */
	uint32_t arena = (r->tex_bytes + 127) & ~127u, tex_bytes = arena, n = 0;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= r->cmd_bytes;) {
		struct gfx_cmd *c = (struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		struct gfx_draw *d = (struct gfx_draw *)c;
		for (int i = 0; c->op == GFX_OP_DRAW && i < d->ntex && i < 2; i++) {
			struct gfx_tex *t = &d->tex[i];
			if (t->where == GFX_TEX_ARENA)
				continue;
			bool target = t->where == GFX_TEX_TARGET;
			uint32_t k = 0, dst = arena;
			while (k < n && (target || tgt[k] || src[k] != t->offset))
				dst += (len[k++] + 127) & ~127u;
			if (k == n && n < max) {
				src[n] = t->offset;
				tgt[n] = target ? t : NULL;
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
		const uint8_t *p = rdp.cache + src[k];
		if (tgt[k]) {
			memset(here, 0, GFX_TEX_MAX);
			if (rdp.rdram && len[k] <= GFX_TEX_MAX)
				rdp_decode_rgba16(rdp.rdram, rdp.rdram_size, tgt[k]->src_addr, tgt[k]->src_pitch, tgt[k]->width,
						  tgt[k]->height, here, tgt[k]->pitch);
			p = here;
		}
		ok = len[k] <= (tgt[k] ? GFX_TEX_MAX : CACHE_BYTES) && fwrite(p, 1, len[k], f) == len[k] &&
		     fwrite(zero, 1, pad, f) == pad;
	}
	if (f)
		fclose(f);
	free(cmd);
	free(src);
	free(tgt);
	free(here);
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
