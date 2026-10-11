/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — planificador de eventos.
 *
 * El tiempo se mide en ciclos del VR4300 (PClock, 93,75 MHz) desde el
 * arranque, en 64 bits: no da la vuelta. Cada tipo de evento tiene una
 * ranura fija (no hay asignaciones); `next` es el mínimo de las ranuras
 * activas y es lo único que mira el bucle del CPU después de cada
 * instrucción.
 */
#ifndef SALSA64_CORE_SCHED_H
#define SALSA64_CORE_SCHED_H

#include "core/types.h"

#define SALSA64_SCHED_NEVER UINT64_MAX

enum salsa64_event {
	SALSA64_EV_STOP,     /* fin del lapso pedido a salsa64_n64_run */
	SALSA64_EV_COMPARE,  /* Count == Compare (COP0) */
	SALSA64_EV_VI_INTR,  /* el VI llega a la línea de VI_V_INTR */
	SALSA64_EV_VI_FIELD, /* fin de un campo de video */
	SALSA64_EV_SP_TASK,  /* termina la tarea del RSP en curso */
	SALSA64_EV_DP_SYNC,  /* el RDP termina (G_RDPFULLSYNC): interrupción DP */
	SALSA64_EV_AI_DMA,   /* termina el DMA del AI en curso */
	SALSA64_EV_RSP_LLE,  /* siguiente rebanada del RSP a bajo nivel (host) */
	SALSA64_EV_MAX
};

typedef void (*salsa64_event_fn)(void *ctx, uint64_t when);

struct salsa64_sched {
	uint64_t next;
	uint64_t when[SALSA64_EV_MAX];
	salsa64_event_fn fn[SALSA64_EV_MAX];
	void *ctx[SALSA64_EV_MAX];
};

void salsa64_sched_init(struct salsa64_sched *s);
void salsa64_sched_handler(struct salsa64_sched *s, enum salsa64_event ev,
			   salsa64_event_fn fn, void *ctx);
void salsa64_sched_set(struct salsa64_sched *s, enum salsa64_event ev, uint64_t when);
void salsa64_sched_cancel(struct salsa64_sched *s, enum salsa64_event ev);
/* Ejecuta, en orden de tiempo, los eventos con when <= now. */
void salsa64_sched_dispatch(struct salsa64_sched *s, uint64_t now);

#endif
