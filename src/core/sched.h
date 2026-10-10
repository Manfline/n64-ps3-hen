/*
 * Retro64 — planificador de eventos.
 *
 * El tiempo se mide en ciclos del VR4300 (PClock, 93,75 MHz) desde el
 * arranque, en 64 bits: no da la vuelta. Cada tipo de evento tiene una
 * ranura fija (no hay asignaciones); `next` es el mínimo de las ranuras
 * activas y es lo único que mira el bucle del CPU después de cada
 * instrucción.
 */
#ifndef RETRO64_CORE_SCHED_H
#define RETRO64_CORE_SCHED_H

#include "core/types.h"

#define RETRO64_SCHED_NEVER UINT64_MAX

enum retro64_event {
	RETRO64_EV_STOP,     /* fin del lapso pedido a retro64_n64_run */
	RETRO64_EV_COMPARE,  /* Count == Compare (COP0) */
	RETRO64_EV_MAX
};

typedef void (*retro64_event_fn)(void *ctx, uint64_t when);

struct retro64_sched {
	uint64_t next;
	uint64_t when[RETRO64_EV_MAX];
	retro64_event_fn fn[RETRO64_EV_MAX];
	void *ctx[RETRO64_EV_MAX];
};

void retro64_sched_init(struct retro64_sched *s);
void retro64_sched_handler(struct retro64_sched *s, enum retro64_event ev,
			   retro64_event_fn fn, void *ctx);
void retro64_sched_set(struct retro64_sched *s, enum retro64_event ev, uint64_t when);
void retro64_sched_cancel(struct retro64_sched *s, enum retro64_event ev);
/* Ejecuta, en orden de tiempo, los eventos con when <= now. */
void retro64_sched_dispatch(struct retro64_sched *s, uint64_t now);

#endif
