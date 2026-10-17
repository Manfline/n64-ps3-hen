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

#include "rdp/gfxdump.h"
#include "rdp/targets.h"
#include "rdp/vmatch.h"
#include "rsp/gfx/gfx.h"
#include "rsp/gfx/hle.h"

#define SETS        3
#define VTX_BYTES   (2u << 20)
/* gfx_draw son 480 bytes: 1600 dibujos por tarea. El arena de texturas
 * casi no se usa con la caché (docs/memory-budget.md): le cede 256 KB y
 * CMD_INTERP. */
#define CMD_BYTES   (768u << 10)
/* Con la interpolación la HLE pone también sus registros GFX_OP_VGROUP, en
 * hasta un tercio del búfer (gfx.h): ese tercio va de más (384 KB), y los
 * dibujos tienen el mismo lugar que sin ella (con 768 KB para los dos, una
 * tarea pesada se cortaba y el juego recibía antes la interrupción del DP). */
#define CMD_INTERP  (CMD_BYTES / (GFX_VGROUP_SHARE - 1))
#define CMD_SET     (CMD_BYTES + CMD_INTERP)
#define TEX_BYTES   ((4u << 20) - (256u << 10) - CMD_INTERP)
#define SET_BYTES   (VTX_BYTES + CMD_SET + TEX_BYTES)
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
 * esperar: MIRROR_BYTES es lo que cuenta como imagen chica (hasta 128 × 256 a
 * 16 bits) y MIRROR_SLOT el tamaño de cada búfer, que además guarda las
 * imágenes de una ranura que se recicla (el aviso de 538 × 100 de «Game Pak
 * Check» de Pokémon Stadium: 106 KB con el paso a 64 bytes). */
#define MIRROR_BYTES 65536u
#define MIRROR_SLOT  131072u
#define MIRRORS      4
/* Espejo del framebuffer presentado, cada campo, sin esperar (Mario Kart
 * reduce el cuadro con el CPU para la pantalla del túnel): un búfer grande
 * más, después de los RB_SLOTS y el de cebado. */
#define FRAME_SLOT  (RB_SLOTS + 1)
#define FB_BYTES    (((RB_SLOTS + 2) * RB_SLOT + MIRRORS * MIRROR_SLOT + 0xFFFFFu) & ~0xFFFFFu)
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
/* Interpolación por geometría (salsa64_rdp_interp, docs/interpolacion.md):
 * detrás de los vértices de una tarea con GFX_JOB_INTERP, desde VTX_INTERP
 * de su juego de búferes, van sus posiciones en la tarea anterior (un
 * float[4] por vértice, en el mismo orden). La HLE tiene todo el búfer, como
 * sin la interpolación: una tarea con más vértices que VTX_INTERP no se
 * interpola (antes se cortaba). Las tablas del emparejamiento
 * (vmatch.c) son dos, de VM_GROUPS grupos y VM_CLIP vértices (Zelda OoT usa
 * ~360 y ~2900 por cuadro; GoldenEye ~250 y ~3400); un cuadro que no cabe no
 * se interpola. */
#define VTX_INTERP  ((VTX_BYTES / 3 * 2) & ~31u)
#define VM_GROUPS   2048
#define VM_CLIP     32768
#define INTERP_DISP 120.0f  /* corte (vmatch_pair): píxeles de la N64 por cuadro del juego */
_Static_assert((VTX_BYTES - VTX_INTERP) / 16 >= VTX_INTERP / sizeof(struct gfx_vertex),
	       "una posición anterior por vértice");

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
		       GFX_DEPTH_DECAL == RSXKIT_DEPTH_OFFSET && GFX_DEPTH_PRIM == RSXKIT_DEPTH_CONST && GFX_DEPTH_LESS == RSXKIT_DEPTH_LESS,
	       "profundidad");
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
	uint32_t set_wait_us;  /* perfilado: lo que esperó acquire este juego para la tarea */
	bool interp; /* se envió con GFX_JOB_INTERP */
	/* Interpolación, al traducirla: sus posiciones de antes valen (hubo
	 * anterior y no fue un corte), su número en la tabla de destinos
	 * (rdp_targets.task: el last_task de lo que dibujó) y hasta dónde usó
	 * su arena el PPE (un redibujo decodifica detrás). */
	bool interp_ok;
	uint32_t tg_task;
	uint32_t ppe_tex_end;
};

/* Perfilado (salsa64_rdp_set_perf_hook): lo de la tarea que se traduce. */
static void (*perf_hook)(const struct salsa64_rdp_task_info *info);
static struct salsa64_rdp_task_info perf_info;

void salsa64_rdp_set_perf_hook(void (*hook)(const struct salsa64_rdp_task_info *info))
{
	perf_hook = hook;
}

/* Huella de lo que hace un combiner aproximado: ciclo, texturas, niebla,
 * términos y factores (FNV-1a). */
static void perf_approx(const struct gfx_draw *d)
{
	uint32_t h = 2166136261u;
	const uint8_t head[4] = { d->cycle, d->ntex, d->fog, d->nterms };
	for (unsigned k = 0; k < 4; k++)
		h = (h ^ head[k]) * 16777619u;
	const uint8_t *f = &d->factors[0][0];
	for (unsigned k = 0; k < sizeof d->factors; k++)
		h = (h ^ f[k]) * 16777619u;
	if (!h)
		h = 1;
	perf_info.approx_draws++;
	for (uint32_t k = 0; k < perf_info.nsig; k++)
		if (perf_info.sig[k] == h)
			return;
	if (perf_info.nsig < SALSA64_RDP_TASK_SIGS)
		perf_info.sig[perf_info.nsig++] = h;
}

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }
static inline void hwsync(void) { __asm__ volatile("sync" ::: "memory"); }

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

/* Tablas del emparejamiento: memalign la primera vez que se enciende la
 * interpolación (fuera del bucle) y quedan; apagada no se reservan. */
struct vm_frame {
	struct vmatch_table t;
};
static struct vm_frame *vm;

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
	/* Cambia cada vez que algo escribe en la superficie de la ranura (un
	 * dibujo, la carga desde la RDRAM, crearla): para no rehacer lo que
	 * se calcula de una imagen que no cambió (salsa64_rdp_present_source). */
	uint32_t surf_gen[RDP_TARGETS_MAX];
	uint32_t gen_seq;
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
	struct rdp_box scratch_dirty;  /* lo dibujado en esa ranura después de copiarla */
	uint32_t ppe_tex_off;        /* arena de la tarea en traducción: hasta dónde está ocupado */
	struct depth depths[MAX_DEPTHS];
	int bound_t;                 /* ranura ligada, o -1 */
	struct depth *bound_d;
	unsigned last_set;           /* juego de búferes de la última tarea dibujada */
	struct gfx_result last_res;
	struct salsa64_rdp_stats stats;
	struct salsa64_rdp_tb tb;
	/* Efectos de framebuffer (docs/framebuffer.md). */
	int fbfx;                    /* 0 apagado, 1 render a textura, 2 también copias */
	bool aa_edges;               /* salsa64_rdp_aa_edges */
	uint8_t *rdram;              /* la RDRAM emulada (NULL: sin copias ni cebado) */
	uint32_t rdram_size;
	void (*wake)(void);          /* despierta al hilo de presentación */
	struct rdp_vi_ring vi_ring;  /* orígenes recientes del VI (hilo de emulación) */
	uint8_t *rb;                 /* búferes de copia (RB_SLOTS) y de cebado, mapeados */
	uint32_t rb_off;
	volatile uint32_t readback_done;  /* última tarea con sus copias ya en la RDRAM */
	/* Tarea (n + 1) cuya copia a la RDRAM espera ahora done, o 0:
	 * salsa64_rdp_flush la traduce aunque quien llama todavía no la cuente.
	 * La presentación traduce solo las tareas que el RDP pudo dibujar, y eso
	 * lo dice el hilo de emulación al terminar cada campo; esperando aquí no
	 * lo termina, así que cada copia esperada se cumplía por el tope de
	 * RB_WAIT_US (200 ms por tarea en la intro de Perfect Dark: 5 campos/s). */
	volatile uint32_t rb_waiting;
	/* Lo mismo para acquire: tarea (n − SETS + 1) que tiene que estar
	 * traducida para que el juego de búferes n % SETS quede libre, o 0. Sin
	 * esto, un juego que manda más de SETS tareas gráficas dentro de un
	 * campo (Paper Mario al abrir el telón, Mario Party tras los logos) se
	 * quedaba esperando en acquire sin cerrar el campo, la presentación no
	 * recibía las tareas visibles y nunca traducía esa: interbloqueo. */
	volatile uint32_t set_waiting;
	bool rb_late_logged;
	/* Copias espejo en vuelo: el RSX escribe el búfer; cuando pasa la
	 * barrera se convierte a la RDRAM (readbacks_finish). */
	struct mirror {
		bool used;
		uint32_t addr, width, lw, rows, fence, bpp;
		struct rdp_box box;  /* lo que se escribe en la RDRAM (rows = box.y1) */
		uint32_t task;  /* la tarea que dibujó lo copiado (rdp_dma_since) */
	} mirrors[MIRRORS];
	/* DMA a la RDRAM desde el hilo de emulación (targets.h): una copia de
	 * la tarea T no pisa lo que un DMA escribió desde que T empezó. */
	struct rdp_dma_ring dma;
	struct mirror frame;         /* el espejo del framebuffer presentado */
	/* Última tarea en que cada ranura se copió a la RDRAM (o se rellenó
	 * su cobertura): el espejo del framebuffer no pisa esa zona mientras
	 * el CPU pueda estar leyéndola. */
	uint32_t rb_task[RDP_TARGETS_MAX];
	struct rdp_present last_p;   /* lo que eligió la última presentación (traza por campo) */
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
	struct { int slot; bool fresh; char how[48]; } last_get;  /* qué hizo get_target (traza); fresh: superficie nueva */
	/* Lo último presentado, para repetirlo en una pausa
	 * (salsa64_rdp_present_again); surf NULL: nada que repetir. slot, y0,
	 * pw y ph son la región sin recortar, para repetirla con otra vista o
	 * con otro recorte (el menú de pausa). */
	struct again {
		rsxkit_target *surf;
		int slot;
		uint32_t y0, pw, ph;
		rsxkit_rect src;
		uint32_t w, h;
		rsxkit_view view;
		uint32_t scale;  /* píxeles de la superficie por píxel de la N64 */
		bool interp;     /* lo presentado es un campo intermedio (rdp.interp.surf) */
	} again, again_last; /* again_last: lo del campo anterior (un intermedio que no se pudo lo repite) */
	/* salsa64_rdp_present dibuja (sin filtro) o solo elige la fuente. */
	bool no_draw;
	/* Interpolación por geometría (salsa64_rdp_interp). */
	struct {
		int mode;        /* enum salsa64_interp: con uno distinto de OFF las tareas llevan GFX_JOB_INTERP */
		float t;         /* SALSA64_INTERP_FIXED: cada tarea se dibuja en esa fracción */
		uint32_t cur;    /* tabla de la última tarea traducida (vm[cur]) */
		bool have_prev;  /* vm[cur] es de la tarea anterior a la que se traduce */
		/* Campos intermedios (SALSA64_INTERP_FIELDS, presentación): el
		 * cuadro que se muestra (rdp_targets.task de su dibujo), el campo
		 * en que apareció, los campos que duró el anterior (k) y la
		 * superficie y profundidad de los intermedios (del tamaño del
		 * destino presentado; cuentan en rdp.vram). */
		uint32_t shown_task, frame_field, k;
		uint32_t last_gap;  /* campos que duró el último cuadro (1: el juego va a 60; no se empareja) */
		rsxkit_target *surf;
		rsxkit_depth *depth;
		uint32_t w, h, bytes;
		uint32_t gen;    /* cambia con cada intermedio (salsa64_rdp_present_source) */
		/* Con el hilo de emulación (acquire, interp_hold): la tarea que
		 * acquire está tomando (n + 1), la que se redibuja ahora (su juego
		 * de búferes no se reusa), la más vieja con un redibujo que el RSX
		 * aún no terminó (keep_fence; 0 = en curso: la HLE no suelta de la
		 * caché de texturas lo que usa) y el done_task más alto que recibió
		 * la HLE. */
		volatile uint32_t acq, busy, keep_task, keep_fence, done_given;
		volatile uint32_t done_pending;  /* el done_task que acquire está por recortar (0: ninguno) */
		/* La tarea del cuadro que se muestra: la HLE no suelta de la caché
		 * nada de ella ni de las siguientes (sus intermedios aún se
		 * dibujan); 0 nada. */
		volatile uint32_t protect;
		/* La última tarea con GFX_JOB_INTERP que no cupo (tablas, vértices
		 * o registros: interp_task); 0 ninguna. acquire no pide la bandera
		 * en las INTERP_BACKOFF tareas que siguen (interp_wanted). */
		volatile uint32_t full_task;
	} interp;
	/* Lo que se traduce ahora (salsa64_rdp_translating; lo lee el latido
	 * desde otro hilo): la tarea (n + 1, 0 fuera de translate) y la etapa. */
	volatile uint32_t in_task;
	const char *volatile at;
	/* Caja negra: se avisa de cada tarea al empezar a traducirla. */
	salsa64_rdp_task_hook task_hook;
	void *task_ctx;
	struct salsa64_rdp_field_info finfo;  /* el último campo presentado (salsa64_rdp_field_info) */
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
		s->tex = s->cmd + CMD_SET;
		s->vtx_off = rdp.pool_off + base;
		s->tex_off = rdp.pool_off + base + VTX_BYTES + CMD_SET;
	}
	rdp.cache = rdp.pool + SETS_BYTES;
	rdp.cache_off = rdp.pool_off + SETS_BYTES;
	rdp.rb = rdp.cache + CACHE_BYTES;
	rdp.rb_off = rdp.cache_off + CACHE_BYTES;
	rdp.fbfx = 2;
	rdp.aa_edges = false;  /* experimental: engordaba los modelos en la PS3 */
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

int salsa64_rdp_interp(int mode, float t)
{
	bool on = mode == SALSA64_INTERP_FIELDS || mode == SALSA64_INTERP_FIXED;
	if (on && !vm) {
		size_t bytes = vmatch_table_bytes(VM_GROUPS, VM_CLIP);
		uint8_t *mem = memalign(128, 2 * bytes);
		vm = mem ? malloc(2 * sizeof *vm) : NULL;
		if (!vm) {
			free(mem);
			return RSXKIT_ENOMEM;
		}
		for (int i = 0; i < 2; i++)
			vmatch_table_init(&vm[i].t, mem + i * bytes, VM_GROUPS, VM_CLIP);
	}
	rdp.interp.t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	rdp.interp.mode = on ? mode : SALSA64_INTERP_OFF;
	if (rdp.interp.mode != SALSA64_INTERP_FIELDS)
		rdp.interp.protect = 0;
	return RSXKIT_OK;
}

void salsa64_rdp_aa_edges(bool on)
{
	rdp.aa_edges = on;
}

void salsa64_rdp_last_present(int *slot, uint32_t *age, uint32_t *addr)
{
	*slot = rdp.last_p.slot;
	*age = rdp.last_p.age;
	*addr = rdp.last_p.slot >= 0 ? rdp.tg.t[rdp.last_p.slot].addr : 0;
}

void salsa64_rdp_rdram_written(uint32_t pa, uint32_t len, bool gfx_in_flight)
{
	rdp_dma_note(&rdp.dma, pa, len, rdp.finished + (gfx_in_flight ? 1u : 0u));
}

/* Un DMA escribió desde la tarea `task` en lo que se iba a copiar: la
 * copia se descarta (Star Fox 64 carga datos encima del framebuffer que
 * acaba de limpiar). */
static bool dma_newer(uint32_t addr, uint32_t bytes, uint32_t task)
{
	if (!rdp_dma_since(&rdp.dma, addr, addr + bytes, task))
		return false;
	rdp.stats.dma_skipped++;
	return true;
}

static void interp_release(void);

static void release_targets(void)
{
	rdp.again.surf = NULL;
	rdp.again.interp = false;
	interp_release();
	/* La tabla de destinos vuelve a contar desde 0: nada de lo anterior se
	 * redibuja. */
	rdp.interp.shown_task = rdp.interp.k = rdp.interp.last_gap = 0;
	for (unsigned i = 0; i < SETS; i++)
		rdp.sets[i].tg_task = 0;
	memset(&rdp.again_last, 0, sizeof rdp.again_last);
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
	/* La tabla de destinos, vacía. Sus contadores de texturas siguen: el
	 * log los resta entre informes y, al cargar un estado, salían negativos. */
	uint32_t tex_rsx = rdp.tg.tex_rsx, tex_rdram = rdp.tg.tex_rdram, tex_self = rdp.tg.tex_self;
	rdp_targets_init(&rdp.tg);
	rdp.tg.tex_rsx = tex_rsx;
	rdp.tg.tex_rdram = tex_rdram;
	rdp.tg.tex_self = tex_self;
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
	memset(&rdp.dma, 0, sizeof rdp.dma);
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
	rdp.interp.have_prev = false;
	rdp.interp.acq = rdp.interp.busy = rdp.interp.keep_task = rdp.interp.keep_fence = rdp.interp.done_given = 0;
	rdp.interp.done_pending = 0;
	rdp.interp.protect = 0;
	rdp.interp.full_task = 0;
	memset(&rdp.again_last, 0, sizeof rdp.again_last);
	memset(&rdp.stats, 0, sizeof rdp.stats);
	memset(&rdp.tb, 0, sizeof rdp.tb);
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

/* ¿Pedir a la HLE la identidad de los vértices (GFX_JOB_INTERP) para la
 * tarea n + 1? Le cuesta al SPE 0 y la emulación lo espera (en Super Odyssey
 * 64, que no cabía en las tablas, el SPE tardaba en cada tarea un 60 % más y
 * la emulación pasó de 60 a 55 campos por segundo, con el audio estirado), así
 * que solo cuando puede servir: no con el juego a 60 cuadros (no hay campos
 * intermedios) ni en las INTERP_BACKOFF tareas que siguen a una que no cupo;
 * después se vuelve a probar (la primera con la bandera es la anterior de la
 * que sigue). La prueba fija (SALSA64_INTERP_FIXED) la pide siempre. */
#define INTERP_BACKOFF 90u
static bool interp_wanted(uint32_t n)
{
	if (rdp.interp.mode == SALSA64_INTERP_OFF || !vm)
		return false;
	if (rdp.interp.mode != SALSA64_INTERP_FIELDS)
		return true;
	if (rdp.interp.last_gap == 1)
		return false;
	uint32_t f = rdp.interp.full_task;
	return !f || (int32_t)(n + 1 - f) >= (int32_t)INTERP_BACKOFF;
}

static void acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
	uint32_t n = rdp.acquired;
	struct set *s = &rdp.sets[n % SETS];
	/* Campos intermedios: el juego que se va a pisar no puede estar
	 * redibujándose (interp_hold, del otro lado). */
	bool fields = rdp.interp.mode == SALSA64_INTERP_FIELDS || rdp.interp.busy || rdp.interp.keep_task;
	if (fields) {
		rdp.interp.acq = n + 1;
		hwsync();
	}
	/* Libre cuando su tarea anterior (n − SETS) ya se tradujo y el RSX pasó
	 * la barrera que la siguió. */
	if (n >= SETS) {
		uint64_t t0 = 0;
		for (;;) {
			bool translated = (int32_t)(rdp.translated - (n - SETS + 1)) >= 0;
			lwsync();
			if (translated && rsxkit_fence_done(s->fence) && (!fields || rdp.interp.busy != n - SETS + 1))
				break;
			if (!t0) {
				t0 = sysGetSystemTime();
				rdp.stats.set_waits++;
				/* Que la presentación traduzca esa tarea aunque todavía no
				 * sea visible (el campo no se cierra mientras se espera). */
				rdp.set_waiting = n - SETS + 1;
				lwsync();
				if (rdp.wake)
					rdp.wake();
			}
			sysUsleep(50);
		}
		s->set_wait_us = 0;
		if (t0) {
			rdp.set_waiting = 0;
			uint64_t dt = sysGetSystemTime() - t0;
			rdp.stats.us_set_wait += dt;
			s->set_wait_us = (uint32_t)(dt > 0xFFFFFFFFu ? 0xFFFFFFFFu : dt);
		}
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
	if (fields) {
		/* Lo que usa una tarea que se redibuja (o cuyo redibujo el RSX no
		 * terminó) no sale de la caché de texturas. Mientras se recorta, el
		 * valor sin recortar queda a la vista de interp_hold (si llega justo
		 * ahora, desiste); después, done_given es lo que de verdad recibe la
		 * HLE. Guardar ahí el valor sin recortar hacía que, con la
		 * traducción un cuadro por delante de lo que se ve, ningún
		 * intermedio llegara a tiempo (Mario 64: todos «repetidos»). */
		rdp.interp.done_pending = done;
		hwsync();
		uint32_t kt = rdp.interp.keep_task;
		lwsync();
		uint32_t kf = rdp.interp.keep_fence;
		if (kt && (!kf || !rsxkit_fence_done(kf)) && (int32_t)(done - kt) >= 0)
			done = kt - 1;
		/* La del cuadro en pantalla y las siguientes (se van a mostrar), salvo
		 * las que ya no se pueden redibujar: si la emulación va más adelante
		 * que eso (Mario 64 dibuja cuadros antes de mostrarlos), desde la más
		 * vieja cuyo juego de búferes sigue vivo (este acquire reusa el de la
		 * tarea n + 1 − SETS). Apagarla entera en ese caso dejaba sin
		 * proteger también las siguientes: ningún intermedio a tiempo. */
		uint32_t pr = rdp.interp.protect;
		if (pr) {
			uint32_t alive = n + 2 - SETS;  /* la más vieja con su juego vivo tras este acquire */
			uint32_t lo = (int32_t)(alive - pr) > 0 ? alive : pr;
			if ((int32_t)(done - lo) >= 0)
				done = lo - 1;
		}
		if ((int32_t)(done - rdp.interp.done_given) > 0)
			rdp.interp.done_given = done;
		lwsync();
		rdp.interp.done_pending = 0;
	}
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
	s->interp = interp_wanted(n);
	if (s->interp) {
		job->flags |= GFX_JOB_INTERP;
		job->cmd_cap = CMD_SET;
	} else if (rdp.interp.mode != SALSA64_INTERP_OFF) {
		rdp.stats.interp_skipped++;
	}
	rdp.acquired = n + 1;
}

/* Hasta qué línea llegan las tijeras de los dibujos que siguen a un
 * TARGET (hasta el siguiente): el alto mínimo de su superficie. */
/* ¿Los dibujos hasta el próximo TARGET son todos rellenos (FILLRECT)? Una
 * color image alta con solo rellenos limpia varios framebuffers de una vez
 * (Star Fox 64, DK64) y se reparte; con dibujos de verdad es una imagen
 * (la transición del cierre de DK64, de 320 × 459, que el CPU lee después)
 * y se crea tal cual. */
static bool fills_only(const struct set *s, uint32_t off, uint32_t cmd_bytes)
{
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->op == GFX_OP_TARGET || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW && ((const struct gfx_draw *)c)->cycle != G_CYC_FILL)
			return false;
		off += c->size;
	}
	return true;
}

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

/* Reloj del hilo que traduce (timebase, 79,8 MHz): rdp.tb. */
static inline uint64_t tb_now(void)
{
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

/* Espera una barrera del RSX (duerme, rsxkit_fence_wait) y lo anota. */
static void fence_wait(uint32_t fence)
{
	uint64_t t0 = tb_now();
	rsxkit_fence_wait(fence);
	rdp.tb.fence += tb_now() - t0;
}

/* Escribe en la RDRAM, como RGBA5551, la caja `b` (rdp_box: lo que el RDP
 * pudo escribir, nunca filas enteras) de una copia RGB565 de `lw` píxeles
 * por fila con paso `pitch` que empieza en la fila 0 del destino. */
static void to_rdram(const uint8_t *src, uint32_t pitch, uint32_t addr, uint32_t width, uint32_t lw,
		     const struct rdp_box *b, uint32_t bpp)
{
	uint64_t t0 = tb_now();
	uint32_t x1 = b->x1 < lw ? b->x1 : lw;
	if (rdp_box_empty(b) || x1 <= b->x0)
		return;
	/* El RSX escribió el búfer a espaldas de la caché del PPE: soltar las
	 * líneas que pudieran quedar de la lectura anterior. */
	for (uint32_t off = b->y0 * pitch & ~127u; off < b->y1 * pitch; off += 128)
		__asm__ volatile("dcbf 0,%0" ::"r"(src + off) : "memory");
	__asm__ volatile("sync" ::: "memory");
	for (uint32_t y = b->y0; y < b->y1; y++) {
		const uint16_t *row = (const uint16_t *)(src + y * pitch);
		if (bpp == 1) {
			/* Color image de 8 bits (la cobertura que Zelda copia para el
			 * filtro de Link): la intensidad es el rojo del RGB565. */
			uint8_t *dst = rdp.rdram + addr + y * width;
			for (uint32_t x = b->x0; x < x1; x++)
				dst[x] = (uint8_t)((row[x] >> 8 & 0xF8) | (row[x] >> 13));
		} else {
			rsxkit_pixels_565_to_5551((uint16_t *)(rdp.rdram + addr + y * width * 2) + b->x0, row + b->x0,
						  x1 - b->x0);
		}
	}
	rdp.tb.copy += tb_now() - t0;
}

/* Copias espejo cuya barrera ya pasó: a la RDRAM y el búfer queda libre. */
static void readbacks_finish(bool wait)
{
	struct mirror *f = &rdp.frame;
	if (f->used && (wait ? (fence_wait(f->fence), true) : rsxkit_fence_done(f->fence))) {
		if (!dma_newer(f->addr, f->width * 2 * f->rows, f->task)) {
			to_rdram(rdp.rb + FRAME_SLOT * RB_SLOT, RB_PITCH, f->addr, f->width, f->lw, &f->box, 2);
			rdp.stats.frames_mirrored++;
		}
		f->used = false;
	}
	for (int k = 0; k < RB_SLOTS; k++) {
		struct mirror *m = &rdp.big[k];
		if (m->used && (wait ? (fence_wait(m->fence), true) : rsxkit_fence_done(m->fence))) {
			uint32_t bpp = m->bpp ? m->bpp : 2;
			if (!dma_newer(m->addr, m->width * bpp * m->rows, m->task)) {
				to_rdram(rdp.rb + k * RB_SLOT, RB_PITCH, m->addr, m->width, m->lw, &m->box, bpp);
				rdp.stats.mirrors++;
			}
			m->used = false;
		}
	}
	for (int k = 0; k < MIRRORS; k++) {
		struct mirror *m = &rdp.mirrors[k];
		if (!m->used)
			continue;
		if (wait)
			fence_wait(m->fence);
		else if (!rsxkit_fence_done(m->fence))
			continue;
		uint32_t pitch = (m->lw * 2 + 63) & ~63u, bpp = m->bpp ? m->bpp : 2;
		if (!dma_newer(m->addr, m->width * bpp * m->rows, m->task)) {
			to_rdram(rdp.rb + (RB_SLOTS + 2) * RB_SLOT + k * MIRROR_SLOT, pitch, m->addr, m->width, m->lw, &m->box,
				 bpp);
			rdp.stats.mirrors++;
		}
		m->used = false;
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
	uint64_t t0 = tb_now();
	for (uint32_t y = 0; y < t->lh; y++)
		memcpy(dst + y * pitch, rdp.rdram + t->addr + y * line, t->lw * 2);
	rdp.tb.copy += tb_now() - t0;
	if (rsxkit_target_fill(rdp.surf[i], rdp.rb_off + RB_SLOTS * RB_SLOT, pitch, t->lw, t->lh, RSXKIT_RGBA5551) ==
	    RSXKIT_OK) {
		rdp.bound_t = -1;  /* rsxkit_target_fill ligó la superficie sin profundidad */
		rdp.stats.primes++;
		rdp.surf_gen[i] = ++rdp.gen_seq;
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
	rdp.last_get.fresh = false;
	/* Una color image imposible (la HLE la sacó de un comando mal
	 * interpretado: Zelda al abrir la pausa manda anchos de 1495 y 3770) no
	 * toca la tabla: reciclaría una ranura buena para nada. */
	if (!width || width > 640 || min_lh > 480) {
		rdp.stats.targets_absurd++;
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "imposible, ignorada");
		LOGN(10, "gfx: color image imposible %06x/%u (alto %u): se ignora", addr, width, min_lh);
		return -1;
	}
	struct { bool used; uint32_t addr, width, bpp; } before[RDP_TARGETS_MAX];
	for (int k = 0; k < RDP_TARGETS_MAX; k++) {
		before[k].used = rdp.tg.t[k].used;
		before[k].addr = rdp.tg.t[k].addr;
		before[k].width = rdp.tg.t[k].width;
		before[k].bpp = rdp.tg.t[k].bpp;
	}
	int i = rdp_targets_get(&rdp.tg, addr, width, siz, min_lh, &fresh);
	rdp.last_get.slot = i;
	rdp.last_get.fresh = fresh;
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
	else if (before[i].used && before[i].bpp != rdp.tg.t[i].bpp)
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "nueva, era de %u bits", before[i].bpp * 8);
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
	rdp.surf_gen[i] = ++rdp.gen_seq;
	rdp.vram += bytes;
	LOGN(40, "gfx: destino %06x/%u: %u × %u a %u× (%s; alto pedido %u; %u KB vivos)", addr, width, t->lw, t->lh, sc,
	     rdp.last_get.how, min_lh, rdp.vram >> 10);
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
	/* De 16 bits, o una I8 de una color image de 8 bits que la HLE dejó
	 * para leer de ahí (las otras de 8 bits no se leen igual del RSX). */
	uint32_t bpp = gt->src_siz == 2u ? 2 : gt->src_siz == 1u && gt->where == GFX_TEX_TARGET ? 1 : 0;
	if (rdp.fbfx < 1 || !gt->src_addr || !bpp)
		return false;
	int slot;
	uint32_t x0, y0;
	enum rdp_texsrc src = rdp_targets_texture(&rdp.tg, gt->src_addr, gt->src_pitch, bpp, cur, &slot, &x0, &y0);
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
		/* Otra vez si desde la copia se dibujó donde lee la textura: Conker
		 * usa la misma imagen de 8 bits para la sombra de cada personaje y
		 * desenfoca cada silueta leyendo lo que acaba de dibujar. Las
		 * franjas de Zelda leen filas que aún no reescribieron: una copia. */
		if (rdp.scratch_slot != slot || rdp_box_overlaps(&rdp.scratch_dirty, x0, y0, x0 + gt->width, y0 + gt->height)) {
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
			memset(&rdp.scratch_dirty, 0, sizeof rdp.scratch_dirty);
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
	/* Una I8: la intensidad que el RDP escribió en la imagen es el rojo. */
	t->remap = bpp == 1 ? RSXKIT_REMAP_RRRR : RSXKIT_REMAP_NONE;
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
	uint64_t t0 = tb_now();
	for (uint32_t y = y0; y < y1 && x1 > x0; y++) {
		uint32_t a = t->addr + (y * t->width + x0) * 2;
		if (a + (x1 - x0) * 2 > rdp.rdram_size)
			break;
		memset(rdp.rdram + a, 0xFF, (x1 - x0) * 2);
	}
	rdp.tb.copy += tb_now() - t0;
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
	uint64_t t0 = tb_now();
	if (gt->src_siz == 1u)
		rdp_decode_i8(rdp.rdram, rdp.rdram_size, gt->src_addr, gt->src_pitch, gt->width, gt->height, s->tex + off,
			      gt->pitch);
	else
		rdp_decode_rgba16(rdp.rdram, rdp.rdram_size, gt->src_addr, gt->src_pitch, gt->width, gt->height,
				  s->tex + off, gt->pitch);
	rdp.tb.tex += tb_now() - t0;
	rdp.ppe_tex_off = off + size;
	t->offset = s->tex_off + off;
	rdp.stats.tex_ppe++;
	return true;
}

/* Algún dibujo traducido leyó una textura de un destino (o de la RDRAM por
 * venir de uno): esa tarea no se interpola (redibujada más tarde, leería lo
 * que el destino tenga entonces). Lo baja translate. */
static bool to_rsxkit_fb;

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
		[GFX_WRAP_MIRROR] = RSXKIT_WRAP_MIRROR, [GFX_WRAP_MIRROR_ONCE] = RSXKIT_WRAP_MIRROR_ONCE,
	};
	memset(d, 0, offsetof(rsxkit_draw, k));
	d->blend = g->blend < sizeof blend ? blend[g->blend] : RSXKIT_BLEND_OFF;
	d->cull = g->cull;
	d->depth = g->depth;
	if (g->depth & GFX_DEPTH_PRIM)
		d->z = gfx_prim_z((const struct gfx_vertex *)(s->vtx) + g->h.a, g->h.b);
	d->alpha_ref = g->atest ? (g->aref ? g->aref : 1) : 0;
	d->ntex = g->ntex > 2 ? 2 : g->ntex;
	d->fog = g->fog;
	/* Con AA_EN el RDP escribe los píxeles que el triángulo cubre en parte
	 * y así tapa las grietas de las uniones en T (el piso del túnel de Dam
	 * en GoldenEye: el borrado azul se veía por ellas); el RSX solo mira el
	 * centro. Las aristas como líneas antes de los triángulos las tapan a
	 * cualquier escala (docs/rsx-notes.md). Sin calcomanías: sus líneas no
	 * llevarían el desplazamiento de polígono de la calcomanía. */
	if (rdp.aa_edges && g->aa && !(g->depth & GFX_DEPTH_DECAL)) {
		/* 1,5 píxeles de la N64 a la escala de la superficie, en octavos:
		 * las grietas de los dos volcados de ares del túnel de Dam se tapan
		 * desde 1,5 (con 1 quedaban tiras de ~1 px entre aristas casi
		 * paralelas); medido con el rasterizador del host, raster.c. */
		uint32_t sc = cur >= 0 && rdp.surf_scale[cur] ? rdp.surf_scale[cur] : 1;
		d->edges = (uint8_t)(12u * sc > 255u ? 255u : 12u * sc);
	}
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
		if (from_target || gt->where == GFX_TEX_TARGET)
			to_rsxkit_fb = true;
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
		uint32_t vi_w = rdp.vi_ring.n ? rdp.vi_ring.cur_width : 0;
		uint32_t siz = c->c & 0xFF;
		/* De 16 bits, o de 8 (la cobertura que Zelda guarda para su filtro,
		 * las máscaras de sombra de Conker): en una de 8 bits el RDP escribe
		 * el rojo, sea cual sea el formato; Conker la declara CI y la lee
		 * como I8. */
		if (c->op == GFX_OP_TARGET && (siz == 2u || siz == 1u) && c->b && c->b <= 640 &&
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
	rdp.rb_waiting = n + 1;
	lwsync();  /* antes de despertar a quien traduce */
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
	rdp.rb_waiting = 0;
	rdp.stats.us_readback_wait += sysGetSystemTime() - t0;
}

static void frame_mirror(int slot, uint32_t rows, bool wait);
static void again_set(int slot, uint32_t y0, uint32_t pw, uint32_t ph, const rsxkit_view *view,
		      const struct salsa64_overscan *crop);

/* Interpolación: empareja los vértices de la tarea `task` con los de la
 * anterior (vmatch.c) y deja sus posiciones de allá detrás de los vértices.
 * Sin pareja (o en un corte), la de ahora. Si la tarea no cupo (tablas,
 * vértices más allá de VTX_INTERP o registros que faltan) no se interpola,
 * no sirve de anterior y acquire deja de pedir la bandera un rato. */
static void interp_task(struct set *s, const struct gfx_result *res, uint32_t task)
{
	uint64_t t0 = sysGetSystemTime();
	uint32_t c = rdp.interp.cur ^ 1;
	struct vmatch_table *cur = &vm[c].t, *prev = rdp.interp.have_prev ? &vm[c ^ 1].t : NULL;
	bool room = res->vtx_bytes <= VTX_INTERP && !(res->flags & GFX_RES_VGROUP_FULL);
	vmatch_build(cur, s->cmd, res->cmd_bytes);
	if (!room)
		cur->full = true;
	if (cur->full)
		rdp.interp.full_task = task;
	uint64_t t1 = sysGetSystemTime();
	struct vmatch_stats st;
	uint32_t nv = res->vtx_bytes / sizeof(struct gfx_vertex);
	vmatch_pair(prev, cur, INTERP_DISP, &st);
	vmatch_atomic(cur, (const struct gfx_vertex *)s->vtx, nv, &st);
	uint64_t t2 = sysGetSystemTime();
	uint32_t paired = room ? vmatch_prev(prev, cur, &st, (const struct gfx_vertex *)s->vtx, nv,
					     (float(*)[4])(s->vtx + VTX_INTERP))
			       : 0;
	uint64_t t3 = sysGetSystemTime();
	rdp.stats.us_interp_build += t1 - t0;
	rdp.stats.us_interp_pair += t2 - t1;
	rdp.stats.us_interp_prev += t3 - t2;
	rdp.interp.cur = c;
	rdp.interp.have_prev = true;
	s->interp_ok = prev && !st.cut && !cur->full && paired;
	if (prev) {
		rdp.stats.interp_tasks++;
		rdp.stats.interp_cuts += st.cut;
		rdp.stats.interp_cuts_camera += st.camera;
		rdp.stats.interp_full += cur->full;
		rdp.stats.interp_vertices += nv;
		rdp.stats.interp_paired += paired;
		rdp.stats.interp_content += st.by_content;
		rdp.stats.interp_rejected += st.jumped + st.deformed + st.collapsed + st.near;
		rdp.stats.interp_reordered += st.reordered;
		rdp.stats.interp_atomic += st.atomic;
		rdp.stats.interp_kept += st.kept;
	}
	if (rdp.interp.mode == SALSA64_INTERP_FIXED && rdp.interp.t < 1.0f && room)
		rsxkit_vertices_prev(s->vtx_off + VTX_INTERP, RSXKIT_MEM_MAIN, rdp.interp.t);
	rdp.stats.us_interp += sysGetSystemTime() - t0;
}

/* Traduce la salida de la tarea n a comandos del RSX. */
/* Borra con `color` el rectángulo (xa, ya)–(xb, yb) (píxeles lógicos) del
 * destino `i`, como un FILLRECT del RDP, y lo cuenta como dibujado en la
 * tarea. false si no hay nada que borrar o no se pudo ligar. */
static bool tall_fill_target(int i, float xa, float ya, float xb, float yb, uint32_t color, bool drawn[RDP_TARGETS_MAX],
			     struct rdp_box drawn_box[RDP_TARGETS_MAX])
{
	const struct rdp_target *t = &rdp.tg.t[i];
	if (yb <= ya || xb <= xa || !rdp.surf[i] || !bind(i, NULL))
		return false;
	float rect[4] = { xa, ya, xb, yb };
	rsxkit_clear(RSXKIT_CLEAR_COLOR, color, 0.0f, rect);
	rdp_targets_drawn(&rdp.tg, i);
	drawn[i] = true;
	rdp.surf_gen[i] = ++rdp.gen_seq;
	rdp.surf_dirty[i] = true;
	struct rdp_box r = { 0, 0, 0, 0 };
	const uint16_t all[4] = { 0, 0, (uint16_t)(t->lw * 4), (uint16_t)(t->lh * 4) };
	rdp_box_add(&r, xa, ya, xb, yb, true, all, t->lw, t->lh);
	rdp_box_union(&drawn_box[i], &r);
	rdp_box_union(&rdp.tg.t[i].box, &r);
	rdp.stats.tall_fills++;
	return true;
}

/* Copia espejo de una imagen auxiliar chica dibujada en la tarea `n` (nunca
 * presentada, de otro ancho que el VI): el RSX reduce la ranura `i` a un
 * búfer de espejos y la RDRAM la tendrá cuando un dibujo la lea ya vieja
 * (Link en la pantalla de equipo de Zelda, 395 tareas después). No se
 * espera: se convierte en una traducción posterior, cuando el RSX pasó la
 * barrera. Devuelve el espejo usado, o -1 si la ranura no cumple o no hay
 * espejo libre. `box` es lo dibujado en ella en la tarea. */
static int mirror_small(int i, const struct rdp_box *box, uint32_t n, uint32_t limit)
{
	const struct rdp_target *t = &rdp.tg.t[i];
	uint32_t rows = box->y1 < t->lh ? box->y1 : t->lh;
	uint32_t pitch = (t->lw * 2 + 63) & ~63u;
	if (!rdp.rdram || rdp.fbfx < 2 || !rows || t->presented || (t->bpp != 2 && t->bpp != 1) ||
	    t->width == rdp.tg.vi_width || pitch * rows > limit || t->addr + t->width * t->bpp * rows > rdp.rdram_size)
		return -1;
	int k = 0;
	while (k < MIRRORS && rdp.mirrors[k].used)
		k++;
	if (k == MIRRORS) {
		rdp.stats.mirrors_skipped++;
		return -1;
	}
	rsxkit_rect src = surface_rect(i, 0, 0, t->lw, rows);
	if (rsxkit_target_read(rdp.surf[i], &src, rdp.rb_off + (RB_SLOTS + 2) * RB_SLOT + k * MIRROR_SLOT, pitch, t->lw,
			       rows, RSXKIT_RGB565) != RSXKIT_OK)
		return -1;
	rdp.bound_t = -1;
	rdp.rb_task[i] = rdp.tg.task;
	struct mirror *m = &rdp.mirrors[k];
	m->used = true;
	m->addr = t->addr;
	m->width = t->width;
	m->lw = t->lw;
	m->rows = rows;
	m->box = *box;
	m->box.y1 = (uint16_t)rows;
	m->bpp = t->bpp;
	m->task = n + 1;
	return k;
}

static void translate(uint32_t n)
{
	uint64_t t0 = sysGetSystemTime(), tb0 = tb_now();
	struct set *s = &rdp.sets[n % SETS];
	const struct gfx_result *res = &s->res;
	rdp.at = "inicio";
	rdp.in_task = n + 1;
	if (rdp.task_hook) {
		struct gfx_dump_src src = { s->cmd, res->cmd_bytes, s->vtx, res->vtx_bytes, s->tex, res->tex_bytes,
					    rdp.cache, CACHE_BYTES, rdp.rdram, rdp.rdram_size };
		rdp.task_hook(rdp.task_ctx, n, &src, res);
	}
	rsxkit_vertices(s->vtx_off, RSXKIT_MEM_MAIN);
	s->interp_ok = false;
	to_rsxkit_fb = false;
	/* Un juego a 60 cuadros no tiene campos intermedios: no se empareja. */
	if (s->interp && !(rdp.interp.mode == SALSA64_INTERP_FIELDS && rdp.interp.last_gap == 1))
		interp_task(s, res, n + 1);
	else
		rdp.interp.have_prev = false;
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
	rdp_targets_task(&rdp.tg);
	s->tg_task = rdp.tg.task;
	rdp.scratch_slot = -1;
	rdp.ppe_tex_off = res->tex_bytes;
	readbacks_finish(false);
	rsxkit_fp_stats fp0 = { 0 };
	if (perf_hook) {
		memset(&perf_info, 0, sizeof perf_info);
		rsxkit_fp_get_stats(&fp0);
	}
	int cur = -1;
	bool drawn[RDP_TARGETS_MAX] = { false };
	rdp.at = "dibujos";
	struct rdp_box drawn_box[RDP_TARGETS_MAX];  /* lo que tocaron los dibujos de la tarea */
	memset(drawn_box, 0, sizeof drawn_box);
	int mirror_k[RDP_TARGETS_MAX + MIRRORS];  /* espejos de la tarea (los de ranuras recicladas y los del final) */
	uint32_t nmirror = 0;

	uint32_t off = 0;
	rsxkit_draw rd;
	struct { uint32_t addr, width, bpp, rows; } tall = { 0, 0, 0, 0 };  /* color image más alta que 480 */
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		switch (c->op) {
		case GFX_OP_TARGET: {
			uint32_t lh = scissor_bottom(s, off + c->size, res->cmd_bytes), siz = c->c & 0xFF;
			bool is_tall = c->b && c->b <= 640 && rdp_targets_tall(c->b, lh) &&
				       fills_only(s, off + c->size, res->cmd_bytes);
			rdp.at = "destinos";
			if (!is_tall && c->b && c->b <= 640 && lh <= 480) {
				/* La ranura va a ser de otra imagen (la recicla este destino o
				 * crece): lo que el RDP dibujó en ella hasta ahora, si es una
				 * imagen auxiliar chica, se copia con SU dirección y SU alto
				 * antes de que cambien; después el juego la usa de textura
				 * (Pokémon Stadium: nueve destinos en una tarea). */
				int v = rdp_targets_recycled(&rdp.tg, c->a, c->b, siz, lh);
				if (v >= 0 && drawn[v] && rdp.surf[v]) {
					int k = mirror_small(v, &drawn_box[v], n, MIRROR_SLOT);
					if (k >= 0)
						mirror_k[nmirror++] = k;
					LOGN(20, "gfx: la ranura %d (%06x/%u) se recicla con lo dibujado hasta la fila %u: %s", v,
					     rdp.tg.t[v].addr, rdp.tg.t[v].width, drawn_box[v].y1, k >= 0 ? "copiada" : "perdida");
				}
			}
			cur = is_tall ? -1 : get_target(c->a, c->b, (c->c >> 8) & 0xFF, siz, lh);
			rdp.at = "dibujos";
			if (cur >= 0 && rdp.last_get.fresh) {
				/* Ranura de otra imagen: lo dibujado y su caja eran del destino
				 * anterior (ver raster.c). */
				drawn[cur] = false;
				memset(&drawn_box[cur], 0, sizeof drawn_box[cur]);
				/* Un relleno de imagen alta anterior (rdp_targets_tall_note) que
				 * este destino no vio: se cebó con la RDRAM, que no lo tiene. */
				struct rdp_tall_rect pend[RDP_TALL_FILLS];
				int np = rdp_targets_tall_pending(&rdp.tg, cur, &rdp.dma, pend);
				for (int k = 0; k < np; k++)
					if (tall_fill_target(cur, pend[k].x0, pend[k].y0, pend[k].x1, pend[k].y1, pend[k].color, drawn,
							     drawn_box))
						LOGN(20, "gfx: destino nuevo %06x/%u: relleno de imagen alta anterior (%.0f,%.0f)–(%.0f,%.0f)",
						     c->a, c->b, pend[k].x0, pend[k].y0, pend[k].x1, pend[k].y1);
			}
			if (is_tall)
				snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "alta, rellenos repartidos");
			tall.rows = is_tall ? lh : 0;
			tall.addr = c->a;
			tall.width = c->b;
			tall.bpp = siz == 3 ? 4 : siz == 2 ? 2 : 1;
			if (s->trace && rdp_log)
				rdp_log("gfx: fb tarea %u: color image %06x/%u siz %u → ranura %d (%s)", n, c->a, c->b,
					c->c & 0xFF, cur, rdp.last_get.how);
			break;
		}
		case GFX_OP_ZCLEAR:
			zclear((const struct gfx_zclear *)c);
			break;
		case GFX_OP_DRAW: {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			if (g->approx && perf_hook)
				perf_approx(g);
			if (cur < 0 && tall.rows && g->cycle == G_CYC_FILL && g->h.b >= 3) {
				/* Relleno de la imagen alta (rdp_targets_inside): se borra el
				 * rectángulo en cada destino que cae dentro. */
				int slots[RDP_TARGETS_MAX];
				uint32_t first[RDP_TARGETS_MAX];
				const struct gfx_vertex *v = (const struct gfx_vertex *)(s->vtx) + g->h.a;
				float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
				for (uint32_t k = 0; k < g->h.b; k++) {
					if (!(v[k].w > 0.0f))
						continue;
					float x = v[k].x / v[k].w, y = v[k].y / v[k].w;
					x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
					y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
				}
				uint32_t color = (uint32_t)(g->k[GFX_K_ONE][0][0] * 255.0f + 0.5f) << 16 |
						 (uint32_t)(g->k[GFX_K_ONE][0][1] * 255.0f + 0.5f) << 8 |
						 (uint32_t)(g->k[GFX_K_ONE][0][2] * 255.0f + 0.5f);
				int nin = rdp_targets_inside(&rdp.tg, tall.addr, tall.width, tall.bpp, tall.rows, slots, first);
				for (int k = 0; k < nin && x1 > x0 && y1 > y0; k++) {
					int i = slots[k];
					const struct rdp_target *t = &rdp.tg.t[i];
					float ya = y0 - (float)first[k], yb = y1 - (float)first[k];
					ya = ya < 0.0f ? 0.0f : ya;
					yb = yb > (float)t->lh ? (float)t->lh : yb;
					float xa = x0 < 0.0f ? 0.0f : x0, xb = x1 > (float)t->lw ? (float)t->lw : x1;
					tall_fill_target(i, xa, ya, xb, yb, color, drawn, drawn_box);
				}
				/* Y para los destinos que se creen después. */
				rdp_targets_tall_note(&rdp.tg, tall.addr, tall.width, tall.bpp, tall.rows, x0, y0, x1, y1, color, slots,
						      nin);
				break;
			}
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
			if (g->depth) {
				rdp.at = "profundidad";
				d = get_depth(g->zimg, cur);
				rdp.at = "dibujos";
			}
			if (!bind(cur, d))
				break;
			rdp_targets_drawn(&rdp.tg, cur);
			drawn[cur] = true;
			rdp.surf_gen[cur] = ++rdp.gen_seq;
			rdp.surf_dirty[cur] = true;
			/* Qué puede tocar el dibujo: la caja de sus vértices dentro de
			 * las tijeras (las copias a la RDRAM escriben solo eso: el alto
			 * del destino es una conjetura y pasarse pisaría el heap del
			 * juego, como detrás del búfer de Link en Zelda; y fuera de las
			 * tijeras hay datos, como en los márgenes del z-buffer de
			 * Smash; targets.h). */
			{
				const struct gfx_vertex *v = (const struct gfx_vertex *)(s->vtx) + g->h.a;
				float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
				bool front = true;
				for (uint32_t k = 0; k < g->h.b; k++) {
					if (!(v[k].w > 0.0f)) {
						front = false;
						continue;
					}
					float x = v[k].x / v[k].w, y = v[k].y / v[k].w;
					x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
					y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
				}
				const struct rdp_target *t = &rdp.tg.t[cur];
				struct rdp_box r = { 0, 0, 0, 0 };
				rdp_box_add(&r, x0, y0, x1, y1, front, g->scissor, t->lw, t->lh);
				rdp_box_union(&drawn_box[cur], &r);
				rdp_box_union(&rdp.tg.t[cur].box, &r);
				if (cur == rdp.scratch_slot)
					rdp_box_union(&rdp.scratch_dirty, &r);
			}
			rsxkit_draw_triangles(&rd, g->h.a, g->h.b);
			rdp.stats.draws++;
			if (rd.edges)
				rdp.stats.draws_edges++;
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
	if (to_rsxkit_fb && s->interp_ok) {
		s->interp_ok = false;
		rdp.stats.interp_fbtex++;
	}
	rdp.at = "copias a la RDRAM";
	/* Copias a la RDRAM que pidió done: el RSX reduce cada destino a su
	 * búfer y, pasada la barrera, el PPE lo convierte y lo escribe. */
	struct { int slot; uint32_t buf, rows; struct rdp_box box; } rb[RB_SLOTS];
	uint32_t nrb = 0;
	bool sync_rb[RDP_TARGETS_MAX] = { false };
	uint32_t big_used = 0;  /* búferes grandes tomados en esta tarea */
	for (int k = 0; k < RB_SLOTS; k++)
		big_used |= rdp.big[k].used << k;
	for (uint32_t k = 0; k < s->rb_n; k++) {
		bool handled = false;
		for (int i = 0; i < RDP_TARGETS_MAX; i++) {
			const struct rdp_target *t = &rdp.tg.t[i];
			uint32_t rows = drawn_box[i].y1 < t->lh ? drawn_box[i].y1 : t->lh;
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
					fence_wait(rsxkit_fence());
					for (uint32_t j = 0; j < nrb; j++) {
						const struct rdp_target *tj = &rdp.tg.t[rb[j].slot];
						if (dma_newer(tj->addr, tj->width * tj->bpp * rb[j].rows, n + 1))
							continue;
						to_rdram(rdp.rb + rb[j].buf * RB_SLOT, RB_PITCH, tj->addr, tj->width, tj->lw, &rb[j].box,
							 tj->bpp);
						rdp.stats.readbacks++;
					}
					nrb = 0;
				}
				readbacks_finish(true);
				big_used = 0;
				buf = 0;
			}
			/* La esperada pasa al CPU (la va a leer y quizá cambiar: el
			 * fondo de la pausa de Zelda); la que se repite sin esperar no:
			 * el RDP la sigue leyendo del RSX (targets.h). */
			if (!rdp_targets_readback(&rdp.tg, i, true, s->rb_sync[k]))
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
				rdp_log("gfx: fb tarea %u: copia %06x/%u ranura %d %s, %u..%u × %u..%u, búfer %d", n, t->addr,
					t->width, i, s->rb_sync[k] ? "esperada" : "sin esperar", drawn_box[i].x0, drawn_box[i].x1,
					drawn_box[i].y0, rows, buf);
			if (s->rb_sync[k]) {
				rb[nrb].slot = i;
				rb[nrb].buf = buf;
				rb[nrb].rows = rows;
				rb[nrb].box = drawn_box[i];
				rb[nrb].box.y1 = (uint16_t)rows;
				nrb++;
			} else {
				struct mirror *m = &rdp.big[buf];
				m->used = true;
				m->addr = t->addr;
				m->width = t->width;
				m->lw = t->lw;
				m->rows = rows;
				m->box = drawn_box[i];
				m->box.y1 = (uint16_t)rows;
				m->bpp = t->bpp;
				m->task = n + 1;
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
	for (int i = 0; rdp.rdram && rdp.fbfx >= 2 && i < RDP_TARGETS_MAX; i++) {
		if (!drawn[i] || sync_rb[i])
			continue;
		int k = mirror_small(i, &drawn_box[i], n, MIRROR_BYTES);
		if (k >= 0)
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
		rdp.at = "barrera de las copias";
		uint64_t t1 = sysGetSystemTime();
		fence_wait(s->fence);
		for (uint32_t k = 0; k < nrb; k++) {
			const struct rdp_target *t = &rdp.tg.t[rb[k].slot];
			if (dma_newer(t->addr, t->width * t->bpp * rb[k].rows, n + 1))
				continue;
			to_rdram(rdp.rb + rb[k].buf * RB_SLOT, RB_PITCH, t->addr, t->width, t->lw, &rb[k].box, t->bpp);
			rdp.stats.readbacks++;
		}
		readbacks_finish(false);  /* pasó la barrera: los espejos también */
		rdp.stats.us_readback += sysGetSystemTime() - t1;
	}
	rdp.at = "espejo del framebuffer";
	/* Espejo de los framebuffers que dibujó la tarea (el VI ya los mostró
	 * alguna vez): ahora, como el RDP, y no al presentarlos. Va después de
	 * poner las barreras de las copias de arriba (readbacks_finish no debe
	 * ver una sin barrera). */
	for (int i = 0; rdp.rdram && rdp.fbfx >= 2 && i < RDP_TARGETS_MAX; i++) {
		uint32_t rows = drawn_box[i].y1 < rdp.tg.t[i].lh ? drawn_box[i].y1 : rdp.tg.t[i].lh;
		if (drawn[i] && !sync_rb[i] && rdp_targets_mirror_at_task_end(&rdp.tg, i))
			frame_mirror(i, rows, true);
	}
	lwsync();  /* la RDRAM, antes de decir que está */
	rdp.readback_done = n + 1;
	rdp.at = "fin";
	s->ppe_tex_end = rdp.ppe_tex_off;
	rdp.last_res = *res;
	rdp.last_set = (unsigned)(s - rdp.sets);
	rdp.stats.tasks++;
	uint64_t us = sysGetSystemTime() - t0;
	rdp.stats.us_translate += us;
	rdp.tb.translate += tb_now() - tb0;
	if (perf_hook) {
		rsxkit_fp_stats fp1;
		rsxkit_fp_get_stats(&fp1);
		perf_info.task = n + 1;
		perf_info.res = res;
		perf_info.us_translate = (uint32_t)us;
		perf_info.us_set_wait = s->set_wait_us;
		perf_info.fp_loads = fp1.loads - fp0.loads;
		perf_info.readbacks = nrb;
		perf_hook(&perf_info);
	}
	rdp.in_task = 0;
	rdp.at = NULL;
}

uint32_t salsa64_rdp_flush(uint32_t upto)
{
	uint32_t n = rdp.translated, done_n = rdp.finished;
	lwsync();  /* los resultados, después de ver `finished` */
	if (upto == ~0u)
		upto = done_n;
	uint32_t w = rdp.rb_waiting;  /* la que espera la emulación, siempre */
	if (w && (int32_t)(w - upto) > 0)
		upto = w;
	w = rdp.set_waiting;  /* y la que libera el juego de búferes que espera acquire */
	if (w && (int32_t)(w - upto) > 0)
		upto = w;
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
		fence_wait(rdp.frame.fence);
		readbacks_finish(false);
	}
	if (rdp.frame.used || !rdp.rdram || rdp.fbfx < 2 || !rdp.surf[slot] || t->bpp != 2 || t->lw > RB_MAX_W ||
	    rows > RB_MAX_H || !rows || t->addr + t->width * 2 * rows > rdp.rdram_size ||
	    !rdp_targets_mirror_wanted(&rdp.tg, slot))
		return;
	if (rows > t->lh)
		rows = t->lh;
	/* Lo dibujado desde el último espejo (targets.h), hasta `rows`. */
	struct rdp_box box = t->box;
	if (rows > box.y1)
		rows = box.y1;
	box.y1 = (uint16_t)rows;
	if (rdp_box_empty(&box))
		return;
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
	rdp.frame.box = box;
	rdp.frame.task = t->last_task;
	rdp.frame.fence = rsxkit_fence();
}

/* ---- Campos intermedios (SALSA64_INTERP_FIELDS, docs/interpolacion.md) ----
 *
 * En los campos en que el VI repite un cuadro del juego que tardó k campos
 * (2–4: 30 a 15 cuadros), la tarea que lo dibujó se vuelve a traducir con
 * sus vértices entre el cuadro anterior y ese (t = (j + 1)/k en el campo j
 * desde que apareció; el cuadro de verdad llega k − 1 campos después: 33 ms
 * a 20 cuadros) en una superficie aparte, y se presenta esa. Solo si su juego
 * de búferes sigue con ella (interp_hold) y sus posiciones de antes valen. */

static void interp_release(void)
{
	if (rdp.interp.surf)
		rsxkit_target_destroy(rdp.interp.surf);
	if (rdp.interp.depth)
		rsxkit_depth_destroy(rdp.interp.depth);
	rdp.interp.surf = NULL;
	rdp.interp.depth = NULL;
	rdp.vram -= rdp.interp.bytes < rdp.vram ? rdp.interp.bytes : rdp.vram;
	rdp.interp.w = rdp.interp.h = rdp.interp.bytes = 0;
}

/* Superficie y profundidad de los intermedios del tamaño de la del destino
 * `slot` (cuentan en el tope de memoria como un destino más). */
static bool interp_surface(int slot)
{
	uint32_t w, h;
	rsxkit_target_size(rdp.surf[slot], &w, &h);
	if (rdp.interp.surf && rdp.interp.w == w && rdp.interp.h == h)
		return true;
	interp_release();
	uint32_t bytes = 2 * rdp_surface_bytes(w, h);
	if (!make_room(bytes, slot))
		return false;
	rdp.interp.surf = rsxkit_target_create(w, h);
	rdp.interp.depth = rdp.interp.surf ? rsxkit_depth_create(w, h) : NULL;
	if (!rdp.interp.depth) {
		interp_release();
		LOGN(5, "gfx: interpolación: sin memoria del RSX para la superficie de %ux%u", w, h);
		return false;
	}
	rdp.interp.w = w, rdp.interp.h = h, rdp.interp.bytes = bytes;
	rdp.vram += bytes;
	return true;
}

/* La tarea `task` (fence_task) se va a redibujar: que acquire no reuse su
 * juego y que la HLE no suelte de la caché lo que usa. false si ya es
 * tarde (su juego se está reusando, o una tarea en la HLE pudo soltarlo). */
static bool interp_hold(uint32_t task)
{
	rdp.interp.busy = task;
	uint32_t kf = rdp.interp.keep_fence;
	if (!rdp.interp.keep_task || (kf && rsxkit_fence_done(kf)))
		rdp.interp.keep_task = task;
	rdp.interp.keep_fence = 0;
	hwsync();
	uint32_t pend = rdp.interp.done_pending;
	bool reused = (int32_t)(rdp.interp.acq - (task + SETS)) >= 0;
	bool cache = (int32_t)(rdp.interp.done_given - task) >= 0 || (pend && (int32_t)(pend - task) >= 0);
	if (reused || cache) {
		rdp.stats.interp_rep_set += reused;
		rdp.stats.interp_rep_cache += !reused;
		rdp.interp.keep_fence = rsxkit_fence();
		lwsync();
		rdp.interp.busy = 0;
		return false;
	}
	return true;
}

/* Redibuja la tarea de `s` (la que dibujó el destino `slot`) en
 * rdp.interp.surf con las posiciones en la fracción t: primero una copia
 * del destino (lo que la tarea no redibuja queda igual) y después solo sus
 * dibujos en ese destino, con la profundidad aparte. No toca la tabla de
 * destinos, las copias a la RDRAM ni el estado de lo que se dibuja de
 * verdad. */
static void interp_draw(struct set *s, int slot, float t)
{
	const struct rdp_target *tg = &rdp.tg.t[slot];
	const struct gfx_result *res = &s->res;
	rsxkit_target_copy(rdp.interp.surf, rdp.surf[slot]);
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
	bool bound = false;
	rsxkit_vertices(s->vtx_off, RSXKIT_MEM_MAIN);
	rsxkit_vertices_prev(s->vtx_off + VTX_INTERP, RSXKIT_MEM_MAIN, t);
	rdp.scratch_slot = -1;
	rdp.ppe_tex_off = s->ppe_tex_end;
	bool here = false, zpend = false;
	uint32_t zaddr = 0;
	float zval = 1.0f, zrect[4];
	rsxkit_draw rd;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= res->cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		off += c->size;
		if (c->op == GFX_OP_TARGET) {
			here = c->a == tg->addr && c->b == tg->width;
		} else if (c->op == GFX_OP_ZCLEAR) {
			const struct gfx_zclear *z = (const struct gfx_zclear *)c;
			zaddr = z->h.a;
			zval = (float)(z->h.b >> 2) / 16383.0f;
			for (int i = 0; i < 4; i++)
				zrect[i] = z->rect[i];
			zpend = true;
		} else if (c->op == GFX_OP_DRAW && here && c->size >= sizeof(struct gfx_draw)) {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			bool sync;
			if (!g->h.b || g->viscvg || !to_rsxkit(g, s, slot, &rd, &sync))
				continue;
			if (sync)
				rsxkit_texture_sync();
			/* Una copia al scratch (to_rsxkit) liga otro destino. */
			if (!bound || rdp.bound_t != -2) {
				if (rsxkit_bind(rdp.interp.surf, rdp.interp.depth, (float)tg->lw, (float)tg->lh) != RSXKIT_OK)
					break;
				rdp.bound_t = -2;
				if (!bound)  /* la profundidad aparte, lejos al empezar */
					rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, 1.0f, NULL);
				bound = true;
			}
			/* Y como la borró la tarea, al primer dibujo que la usa. */
			if (zpend && g->depth && g->zimg == zaddr) {
				rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, zval, zrect);
				zpend = false;
			}
			rsxkit_draw_triangles(&rd, g->h.a, g->h.b);
		}
	}
	s->ppe_tex_end = rdp.ppe_tex_off;
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
}

/* Si el campo es intermedio, lo dibuja y lo deja para presentar
 * (rdp.again.interp). */
static void interp_field(const struct rdp_present *p, uint32_t field)
{
	uint32_t task = rdp.tg.t[p->slot].last_task;
	struct set *s = NULL;
	for (unsigned i = 0; i < SETS; i++)
		if (rdp.sets[i].tg_task == task && rdp.sets[i].fence_task)
			s = &rdp.sets[i];
	if (task != rdp.interp.shown_task) {
		uint32_t gap = field - rdp.interp.frame_field;
		rdp.interp.k = rdp.interp.shown_task && gap >= 2 && gap <= 4 ? gap : 0;
		rdp.interp.last_gap = rdp.interp.shown_task ? gap : 0;
		rdp.interp.shown_task = task;
		rdp.interp.frame_field = field;
		/* Desde ahora la HLE no suelta lo que usa esta tarea (acquire). */
		rdp.interp.protect = s ? s->fence_task : 0;
	}
	uint32_t j = field - rdp.interp.frame_field;
	if (rdp.interp.k < 2 || j + 1 >= rdp.interp.k)
		return;  /* el cuadro de verdad */
	if (!s || !s->interp || !s->interp_ok) {
		rdp.stats.interp_plain++;  /* este cuadro no se interpola: va entero */
		return;
	}
	bool surf = interp_surface(p->slot);
	if (!surf)
		rdp.stats.interp_rep_surf++;
	if (!surf || !interp_hold(s->fence_task)) {
		/* Ya es tarde para redibujarlo: se repite lo del campo anterior (el
		 * cuadro final adelantaría y el intermedio siguiente volvería atrás:
		 * eso se ve como un parpadeo). */
		const struct again *l = &rdp.again_last;
		if (l->surf && (l->interp ? l->surf == rdp.interp.surf : rdp.surf[l->slot] == l->surf))
			rdp.again = *l;
		rdp.stats.interp_repeated++;
		rdp.finfo = (struct salsa64_rdp_field_info){ SALSA64_FIELD_REPEAT, (float)(j + 1) / (float)rdp.interp.k,
							     rdp.interp.k, s->fence_task - 1, s->fence_task - 2 };
		return;
	}
	uint64_t t0 = sysGetSystemTime();
	interp_draw(s, p->slot, (float)(j + 1) / (float)rdp.interp.k);
	rdp.finfo = (struct salsa64_rdp_field_info){ SALSA64_FIELD_INTERP, (float)(j + 1) / (float)rdp.interp.k,
						     rdp.interp.k, s->fence_task - 1, s->fence_task - 2 };
	uint32_t f = rsxkit_fence();
	s->fence = f;
	rdp.interp.keep_fence = f;
	lwsync();
	rdp.interp.busy = 0;
	rdp.again.surf = rdp.interp.surf;
	rdp.again.interp = true;
	rdp.interp.gen = ++rdp.gen_seq;
	rdp.stats.interp_fields++;
	rdp.stats.us_interp_draw += sysGetSystemTime() - t0;
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
	rdp.last_p = p;
	rdp.stats.age_sum += p.age;
	if (p.age > rdp.stats.age_max)
		rdp.stats.age_max = p.age;
	rdp.again.interp = false;
	rdp.finfo.kind = SALSA64_FIELD_TRUE;
	again_set(p.slot, p.y0, p.w, p.h, view, crop);
	if (rdp.interp.mode == SALSA64_INTERP_FIELDS)
		interp_field(&p, field);
	else if (rdp.interp.surf)
		interp_release();
	if (rdp.interp.mode != SALSA64_INTERP_FIELDS)
		rdp.interp.protect = 0;
	rdp.again_last = rdp.again;
	if (!rdp.no_draw)
		rsxkit_target_present(rdp.again.surf, &rdp.again.src, rdp.again.w, rdp.again.h, &rdp.again.view, out);
	else if (out)
		*out = rsxkit_layout(rdp.again.w, rdp.again.h, &rdp.again.view);
	frame_mirror(p.slot, p.y0 + p.h, false);
	return true;
}

/* Lo que se presenta de la región (slot, y0, pw × ph): recorte, tamaño y
 * vista, en rdp.again (salsa64_rdp_present y su repetición). */
static void again_set(int slot, uint32_t y0, uint32_t pw, uint32_t ph, const rsxkit_view *view,
		      const struct salsa64_overscan *crop)
{
	/* Recorte del overscan: la región se achica por los cuatro lados y se
	 * presenta como una imagen más chica (misma proporción de pantalla). */
	uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
	if (crop) {
		cl = crop->left, cr = crop->right, ct = crop->top, cb = crop->bottom;
		if (cl + cr >= pw)
			cl = cr = 0;
		if (ct + cb >= ph)
			ct = cb = 0;
	}
	uint32_t w = pw - cl - cr, h = ph - ct - cb;
	rsxkit_rect src = surface_rect(slot, cl, y0 + ct, w, h);
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
	 * entonces se ajusta como siempre (rdp_present_integer). La decisión y
	 * el tamaño en pantalla salen del tamaño lógico por la escala objetivo,
	 * no de la superficie: dos framebuffers del mismo juego pueden tener
	 * superficies de escala distinta (Donkey Kong 64 crea uno de 320 × 240
	 * mientras viven los de 640 × 480 de sus logos) y con el tamaño de la
	 * superficie cada uno salía de un tamaño: un «zoom» cada 2 campos. */
	const rsxkit_screen *sc = rsxkit_screen_info();
	uint32_t sw = w * rdp.scale, sh = h * rdp.scale;
	rdp.again.surf = rdp.again.interp ? rdp.interp.surf : rdp.surf[slot];
	rdp.again.scale = rdp.surf_scale[slot];
	rdp.again.slot = slot;
	rdp.again.y0 = y0;
	rdp.again.pw = pw;
	rdp.again.ph = ph;
	rdp.again.src = src;
	/* Con una proporción panorámica pedida (aspecto 16:9 del menú) no vale la
	 * presentación a 1:1: esa dibuja la región en su proporción de 4:3 y el
	 * aspecto no se aplicaría. Se ajusta a la pantalla con esa proporción. */
	if (view && view->scale == RSXKIT_SCALE_FIT && view->aspect <= 1.5f && rdp_present_integer(sw, sh, sc->width, sc->height)) {
		rsxkit_view one = { RSXKIT_SCALE_INTEGER, view->filter, 0.0f };
		rdp.again.w = sw;
		rdp.again.h = sh;
		rdp.again.view = one;
	} else {
		rdp.again.w = w;
		rdp.again.h = h;
		rdp.again.view = view ? *view : (rsxkit_view){ RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 0.0f };
	}
}

void salsa64_rdp_present_draw(bool draw)
{
	rdp.no_draw = !draw;
}

/* Lo que eligió la última presentación sigue vivo: el destino, o el campo
 * intermedio. */
static bool again_valid(void)
{
	return rdp.again.surf &&
	       (rdp.again.interp ? rdp.again.surf == rdp.interp.surf : rdp.surf[rdp.again.slot] == rdp.again.surf);
}

bool salsa64_rdp_present_source(const rsxkit_view *view, const struct salsa64_overscan *crop, rsxkit_texture *tex,
				rsxkit_rect *region, uint32_t *scale, uint32_t *gen)
{
	if (!rdp.ready || !again_valid())
		return false;
	if (view || crop)
		again_set(rdp.again.slot, rdp.again.y0, rdp.again.pw, rdp.again.ph, view, crop);
	rsxkit_target_texture(rdp.again.surf, 0, 0, tex);
	*region = rdp.again.src;
	*scale = rdp.again.scale ? rdp.again.scale : 1;
	if (gen)
		*gen = rdp.again.interp ? rdp.interp.gen : rdp.surf_gen[rdp.again.slot];
	return true;
}

bool salsa64_rdp_present_again(const rsxkit_view *view, const struct salsa64_overscan *crop, rsxkit_rect *out)
{
	if (!rdp.ready || !again_valid())
		return false;
	if (view || crop)
		again_set(rdp.again.slot, rdp.again.y0, rdp.again.pw, rdp.again.ph, view, crop);
	rsxkit_target_present(rdp.again.surf, &rdp.again.src, rdp.again.w, rdp.again.h, &rdp.again.view, out);
	return true;
}

void salsa64_rdp_field_info(struct salsa64_rdp_field_info *out)
{
	*out = rdp.finfo;
}

void salsa64_rdp_set_task_hook(salsa64_rdp_task_hook fn, void *ctx)
{
	rdp.task_ctx = ctx;
	rdp.task_hook = fn;
}

uint32_t salsa64_rdp_translated(void)
{
	return rdp.translated;
}

uint32_t salsa64_rdp_translating(const char **stage)
{
	uint32_t t = rdp.in_task;
	if (stage) {
		const char *at = rdp.at;
		*stage = t && at ? at : "";
	}
	return t;
}

bool salsa64_rdp_dump(const char *path)
{
	if (!rdp.ready || !rdp.last_res.cmd_bytes)
		return false;
	const struct set *s = &rdp.sets[rdp.last_set];
	const struct gfx_result *r = &rdp.last_res;
	struct gfx_dump_src src = { s->cmd, r->cmd_bytes, s->vtx, r->vtx_bytes, s->tex, r->tex_bytes,
				    rdp.cache, CACHE_BYTES, rdp.rdram, rdp.rdram_size };
	/* El armado (gfxdump.c) es el mismo de la caja negra; aquí, con
	 * memoria de la que se pide y se suelta (es una orden de depuración). */
	uint32_t max = GFX_DUMP_MAX_TEX(r->cmd_bytes);
	uint32_t *wsrc = malloc(max * 2 * sizeof *wsrc);
	const struct gfx_tex **wtgt = malloc(max * sizeof *wtgt);
	bool ok = false;
	if (wsrc && wtgt) {
		struct gfx_dump_work w = { wsrc, wsrc + max, wtgt, max };
		uint32_t need = gfx_dump_measure(&src, &w);
		uint8_t *buf = need ? malloc(need) : NULL;
		if (buf && gfx_dump_build(&src, &w, buf, need) == need) {
			FILE *f = fopen(path, "wb");
			ok = f && fwrite(buf, 1, need, f) == need;
			if (f)
				fclose(f);
		}
		free(buf);
	}
	free(wsrc);
	free(wtgt);
	return ok;
}

void salsa64_rdp_get_waits(uint64_t *set_wait_us, uint64_t *readback_wait_us)
{
	*set_wait_us = rdp.stats.us_set_wait;
	*readback_wait_us = rdp.stats.us_readback_wait;
}

void salsa64_rdp_get_tb(struct salsa64_rdp_tb *out)
{
	*out = rdp.tb;
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
