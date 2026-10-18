/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — backend gráfico en un SPE (PS3): cada tarea gráfica del RSP
 * se publica como un trabajo de lib/spukit al programa src/spu/gfx. El
 * núcleo pregunta si terminó (`done`) cada poll_cycles y entre medias
 * sigue emulando el CPU; si el CPU está en un bucle de espera, aquí se
 * duerme POLL_SLEEP_US antes de contestar, para no girar en vacío (el PPE
 * es SMT: una espera activa le quita ciclos al otro hilo) y para que el
 * reloj emulado no corra más rápido que el real. `finish` recoge el
 * resultado (ya casi nunca espera).
 *
 * El programa del SPE viene en varias imágenes, una por grupo de familias de
 * microcódigos (rsp/gfx/spe0.h): se carga la de la familia de cada tarea y,
 * si cambia, se pasa el estado del RDP a la nueva (gfx_spu.c,
 * docs/spe0-familias.md).
 */
#ifndef SALSA64_PLATFORM_PS3_GFX_SPU_H
#define SALSA64_PLATFORM_PS3_GFX_SPU_H

#include <spukit/spukit.h>

#include "rsp/gfx/backend.h"

struct salsa64_gfx_spu {
	struct salsa64_gfx_backend backend;
	/* Las listas del RDP que escribe el CPU (DPC_START/DPC_END) las corre este
	 * mismo SPE, con el mismo estado del RDP y la misma caché de texturas que
	 * las tareas: un segundo estado sobre la región de la caché pisaba las
	 * texturas del primero. */
	struct salsa64_dpc_backend dpc;
	/* Las listas que están en la DMEM (xbus: el SPE no las lee) van a este RDP del
	 * PPE, si el frontend lo da. */
	const struct salsa64_dpc_backend *xbus;
	uint32_t dpc_jobs;   /* listas del CPU hechas en el SPE */
	uint32_t errors;     /* trabajos que el SPE devolvió con error (terminan como cortados) */
	uint64_t dpc_us;     /* lo que esperó el PPE a cada una */
	spukit_worker *worker;
	const struct salsa64_bus *bus;
	struct salsa64_gfx_sink sink;
	struct gfx_job job;
	struct gfx_result res;
	uint32_t seq;
	bool pending;
	bool nocache;        /* el hash del SPE no coincide: sin caché de texturas */
	bool no_worker;      /* la tarea en curso no tuvo SPE: termina cortada */
	/* Imagen del SPE 0 cargada (rsp/gfx/spe0.h; -1: ninguna) y sus cambios. */
	int image;
	uint32_t image_tasks0;   /* `tasks` al cargarla */
	uint32_t last_task;      /* número (job->task) de la última tarea o lista mandada */
	uint32_t switches;       /* cambios de imagen con el juego andando */
	uint64_t switch_us;      /* lo que costaron, en el PPE */
	uint32_t selftest_bad;   /* imágenes cuyas tareas de prueba difirieron del PPE (bit por imagen) */
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
	uint64_t skipped;    /* comandos de otro microcódigo saltados (G_LOAD_UCODE) */
	uint64_t bg;         /* fondos de S2DEX dibujados (G_BG_COPY, G_BG_1CYC) */
	uint64_t tex_target; /* texturas sin decodificar: se leen de un destino (GFX_TEX_TARGET) */
};

/* Arranca el trabajador (spukit_init ya llamado). Devuelve 0 o el error de
 * spukit. */
int salsa64_gfx_spu_init(struct salsa64_gfx_spu *s, const struct salsa64_bus *bus,
			 const struct salsa64_gfx_sink *sink);
void salsa64_gfx_spu_shutdown(struct salsa64_gfx_spu *s);

#endif
