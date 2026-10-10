/*
 * Salsa64 — backends de la HLE gráfica para el núcleo (salsa64_gfx_backend).
 *
 * Cada tarea gráfica pasa por tres manos:
 *   acquire  quien consume la salida da los búferes (vértices, comandos,
 *            texturas) y su capacidad en el trabajo;
 *   HLE      gfx_run, aquí mismo (backend síncrono) o en un SPE (el de la
 *            PS3, en src/platform/ps3/gfx_spu.c);
 *   done     al terminar la tarea (cuando el núcleo la da por terminada),
 *            quien consume recibe el trabajo y su resultado.
 */
#ifndef SALSA64_RSP_GFX_BACKEND_H
#define SALSA64_RSP_GFX_BACKEND_H

#include "core/bus.h"
#include "rsp/gfx/gfx.h"

struct salsa64_gfx_sink {
	void *ctx;
	/* Pone en job los búferes de salida (vtx_ea, cmd_ea, tex_ea y sus
	 * capacidades). Puede esperar a que el consumidor libere uno. */
	void (*acquire)(void *ctx, struct gfx_job *job);
	/* La tarea terminó: la salida está completa. */
	void (*done)(void *ctx, const struct gfx_job *job, const struct gfx_result *res);
};

/* Trabajo para una tarea (sin los búferes de salida). */
void salsa64_gfx_job(struct gfx_job *job, const struct salsa64_bus *bus, const struct salsa64_task *task,
		     enum salsa64_gbi gbi);

/* Backend síncrono: la HLE corre en el hilo del CPU al arrancar la tarea. */
struct gfx_state;
struct salsa64_gfx_sync {
	struct salsa64_gfx_backend backend;
	/* RDP a bajo nivel: cada tramo DPC_START..DPC_END es un trabajo
	 * GFX_JOB_DPC sobre el mismo estado (salsa64_rsp_set_dpc). */
	struct salsa64_dpc_backend dpc;
	struct gfx_state *state;
	const struct salsa64_bus *bus;
	struct salsa64_gfx_sink sink;
	struct gfx_job job;
	struct gfx_result res;
	bool pending;
	bool reset;
	uint32_t dpc_runs;
	struct gfx_result dpc_sum;
};

/* `state` lo pone quien llama (~20 KB) y queda inicializado. */
void salsa64_gfx_sync_init(struct salsa64_gfx_sync *s, struct gfx_state *state, const struct salsa64_bus *bus,
			   const struct salsa64_gfx_sink *sink);

#endif
