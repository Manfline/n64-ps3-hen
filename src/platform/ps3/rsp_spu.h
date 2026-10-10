/*
 * Salsa64 — RSP a bajo nivel en un SPE (PS3): backend salsa64_lle_backend
 * que manda el microcódigo real al programa src/spu/rsp como trabajos de
 * lib/spukit (rsp/lle/job.h) y aplica al recogerlos lo que el SPE no puede
 * hacer solo (tramos del RDP, tramos de la RDRAM escritos, interrupción
 * del SP). Fin de tarea por sondeo (`done`), como los demás backends; si
 * el trabajo agotó su cupo de instrucciones sin que el RSP se detuviera,
 * se publica otro que sigue donde quedó.
 */
#ifndef SALSA64_PLATFORM_PS3_RSP_SPU_H
#define SALSA64_PLATFORM_PS3_RSP_SPU_H

#include <spukit/spukit.h>

#include "core/bus.h"
#include "rsp/lle/job.h"
#include "rsp/lle/lle.h"

struct salsa64_rsp_spu {
	struct salsa64_lle_backend backend;
	spukit_worker *worker;
	struct salsa64_bus *bus;
	struct lle_log *log;
	struct lle_job job;
	struct lle_result res;
	uint32_t seq;
	bool pending;
	/* Estadísticas desde el arranque. */
	uint32_t tasks;          /* arranques (tareas del microcódigo) */
	uint32_t jobs;           /* trabajos publicados (continuaciones incluidas) */
	uint64_t instructions;
	uint64_t spu_ticks;      /* tiempo en el SPE (ticks del timebase) */
	uint64_t wait_us;        /* PPE esperando al SPE en finish */
	uint32_t waits, polls;
	uint64_t sleep_us;
	uint32_t irqs, dpc_entries, dpc_lost, failed;
	uint32_t task_max;       /* instrucciones de la tarea más larga */
	uint64_t task_instr;     /* instrucciones de la tarea en curso */
};

/* Arranca el trabajador (spukit_init ya llamado). Devuelve 0 o el error de
 * spukit. Después: salsa64_rsp_set_lle(bus, &s->backend, modo). */
int salsa64_rsp_spu_init(struct salsa64_rsp_spu *s, struct salsa64_bus *bus);
int salsa64_rsp_spu_shutdown(struct salsa64_rsp_spu *s);

#endif
