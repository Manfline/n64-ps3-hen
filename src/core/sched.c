/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — planificador de eventos. Ver sched.h.
 */
#include "core/sched.h"

static void sched_update_next(struct salsa64_sched *s)
{
	uint64_t next = SALSA64_SCHED_NEVER;
	for (int i = 0; i < SALSA64_EV_MAX; i++)
		if (s->when[i] < next)
			next = s->when[i];
	s->next = next;
}

void salsa64_sched_init(struct salsa64_sched *s)
{
	for (int i = 0; i < SALSA64_EV_MAX; i++) {
		s->when[i] = SALSA64_SCHED_NEVER;
		s->fn[i] = NULL;
		s->ctx[i] = NULL;
	}
	s->next = SALSA64_SCHED_NEVER;
}

void salsa64_sched_handler(struct salsa64_sched *s, enum salsa64_event ev,
			   salsa64_event_fn fn, void *ctx)
{
	s->fn[ev] = fn;
	s->ctx[ev] = ctx;
}

void salsa64_sched_set(struct salsa64_sched *s, enum salsa64_event ev, uint64_t when)
{
	s->when[ev] = when;
	sched_update_next(s);
}

void salsa64_sched_cancel(struct salsa64_sched *s, enum salsa64_event ev)
{
	s->when[ev] = SALSA64_SCHED_NEVER;
	sched_update_next(s);
}

void salsa64_sched_dispatch(struct salsa64_sched *s, uint64_t now)
{
	while (s->next <= now) {
		int ev = 0;
		for (int i = 1; i < SALSA64_EV_MAX; i++)
			if (s->when[i] < s->when[ev])
				ev = i;
		uint64_t when = s->when[ev];
		/* El manejador puede reprogramar su propia ranura. */
		s->when[ev] = SALSA64_SCHED_NEVER;
		sched_update_next(s);
		if (s->fn[ev])
			s->fn[ev](s->ctx[ev], when);
	}
}
