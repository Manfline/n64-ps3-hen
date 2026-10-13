/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tarea JPEG del RSP en un SPE (PS3): cada tarea se publica como
 * un trabajo de lib/spukit al programa src/spu/jpeg y el núcleo pregunta si
 * terminó (`done`), como con las gráficas; entre medias el CPU sigue
 * emulando. Si el SPE no puede con la tarea (macrobloques sin alinear a 16
 * bytes), la hace el PPE al recogerla.
 */
#ifndef SALSA64_PLATFORM_PS3_JPEG_SPU_H
#define SALSA64_PLATFORM_PS3_JPEG_SPU_H

#include <spukit/spukit.h>

#include "core/bus.h"
#include "rsp/jpeg/job.h"

struct salsa64_jpeg_spu {
	struct salsa64_jpeg_backend backend;
	spukit_worker *worker;
	struct salsa64_bus *bus;
	struct jpeg_job job;
	uint32_t seq;
	bool pending;
	/* Estadísticas desde el arranque. */
	uint32_t tasks, macroblocks;
	uint32_t on_ppe;     /* devueltas por el SPE (alineación) y hechas en el PPE */
	uint64_t spu_ticks;  /* tiempo en el SPE (ticks del timebase) */
	uint64_t spu_max;    /* la tarea más larga */
	uint64_t wait_us;    /* PPE esperando al SPE al recogerla */
	uint32_t waits;
	uint32_t polls;      /* sondeos con la tarea sin terminar */
	uint64_t sleep_us;   /* dormido en sondeos con el CPU en bucle de espera */
};

/* Arranca el trabajador (spukit_init ya llamado) y compara las tareas de
 * prueba (rsp/jpeg/selftest.c) entre el SPE y el PPE. Devuelve 0 o el error
 * de spukit; *same queda en false si difieren (entonces el SPE no se usa). */
int salsa64_jpeg_spu_init(struct salsa64_jpeg_spu *s, struct salsa64_bus *bus, bool *same);
/* Detiene el trabajador (con la tarea en vuelo ya recogida); devuelve el
 * estado de salida del programa SPU. */
int salsa64_jpeg_spu_shutdown(struct salsa64_jpeg_spu *s);

#endif
