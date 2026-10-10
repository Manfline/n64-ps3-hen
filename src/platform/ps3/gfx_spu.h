/*
 * Salsa64 — backend gráfico en un SPE (PS3): cada tarea gráfica del RSP
 * se publica como un trabajo de lib/spukit al programa src/spu/gfx. El
 * núcleo pregunta si terminó (`done`) cada poll_cycles y entre medias
 * sigue emulando el CPU; si el CPU está en un bucle de espera, aquí se
 * duerme POLL_SLEEP_US antes de contestar, para no girar en vacío (el PPE
 * es SMT: una espera activa le quita ciclos al otro hilo) y para que el
 * reloj emulado no corra más rápido que el real. `finish` recoge el
 * resultado (ya casi nunca espera).
 */
#ifndef SALSA64_PLATFORM_PS3_GFX_SPU_H
#define SALSA64_PLATFORM_PS3_GFX_SPU_H

#include <spukit/spukit.h>

#include "rsp/gfx/backend.h"

struct salsa64_gfx_spu {
	struct salsa64_gfx_backend backend;
	spukit_worker *worker;
	const struct salsa64_bus *bus;
	struct salsa64_gfx_sink sink;
	struct gfx_job job;
	struct gfx_result res;
	uint32_t seq;
	bool pending;
	bool nocache;        /* el hash del SPE no coincide: sin caché de texturas */
	/* Estadísticas desde el arranque. */
	uint32_t tasks;
	uint64_t spu_ticks;  /* tiempo de la HLE en el SPE (ticks del timebase) */
	uint64_t wait_us;    /* PPE esperando al SPE al terminar una tarea (finish) */
	uint32_t waits;
	uint32_t polls;      /* sondeos con el SPE todavía ocupado */
	uint64_t sleep_us;   /* dormido en los sondeos con el CPU en bucle de espera */
	/* Etapas de la HLE (ticks del SPE, ver gfx_result). */
	uint64_t t_vtx, t_state, t_tex, t_load;
	/* Texturas decodificadas y reusadas, cargas a la TMEM y sus lecturas
	 * de la RDRAM (gfx_result). */
	uint64_t textures, tex_reused, loads, load_reads;
	/* Caché de texturas (gfx_result). */
	uint64_t tex_old, tex_arena, tex_evicted, t_hash;
};

/* Arranca el trabajador (spukit_init ya llamado). Devuelve 0 o el error de
 * spukit. */
int salsa64_gfx_spu_init(struct salsa64_gfx_spu *s, const struct salsa64_bus *bus,
			 const struct salsa64_gfx_sink *sink);
void salsa64_gfx_spu_shutdown(struct salsa64_gfx_spu *s);

#endif
