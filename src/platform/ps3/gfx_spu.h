/*
 * Salsa64 — backend gráfico en un SPE (PS3): cada tarea gráfica del RSP
 * se publica como un trabajo de lib/spukit al programa src/spu/gfx y se
 * espera cuando el núcleo da la tarea por terminada. Entre medias el PPE
 * sigue emulando el CPU.
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
	/* Estadísticas desde el arranque. */
	uint32_t tasks;
	uint64_t spu_ticks;  /* tiempo de la HLE en el SPE (ticks del timebase) */
	uint64_t wait_us;    /* PPE esperando al SPE al terminar una tarea */
	uint32_t waits;
};

/* Arranca el trabajador (spukit_init ya llamado). Devuelve 0 o el error de
 * spukit. */
int salsa64_gfx_spu_init(struct salsa64_gfx_spu *s, const struct salsa64_bus *bus,
			 const struct salsa64_gfx_sink *sink);
void salsa64_gfx_spu_shutdown(struct salsa64_gfx_spu *s);

#endif
