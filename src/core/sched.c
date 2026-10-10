/*
 * Retro64 — planificador de eventos. Ver sched.h.
 */
#include "core/sched.h"

static void sched_update_next(struct retro64_sched *s)
{
	uint64_t next = RETRO64_SCHED_NEVER;
	for (int i = 0; i < RETRO64_EV_MAX; i++)
		if (s->when[i] < next)
			next = s->when[i];
	s->next = next;
}

void retro64_sched_init(struct retro64_sched *s)
{
	for (int i = 0; i < RETRO64_EV_MAX; i++) {
		s->when[i] = RETRO64_SCHED_NEVER;
		s->fn[i] = NULL;
		s->ctx[i] = NULL;
	}
	s->next = RETRO64_SCHED_NEVER;
}

void retro64_sched_handler(struct retro64_sched *s, enum retro64_event ev,
			   retro64_event_fn fn, void *ctx)
{
	s->fn[ev] = fn;
	s->ctx[ev] = ctx;
}

void retro64_sched_set(struct retro64_sched *s, enum retro64_event ev, uint64_t when)
{
	s->when[ev] = when;
	sched_update_next(s);
}

void retro64_sched_cancel(struct retro64_sched *s, enum retro64_event ev)
{
	s->when[ev] = RETRO64_SCHED_NEVER;
	sched_update_next(s);
}

void retro64_sched_dispatch(struct retro64_sched *s, uint64_t now)
{
	while (s->next <= now) {
		int ev = 0;
		for (int i = 1; i < RETRO64_EV_MAX; i++)
			if (s->when[i] < s->when[ev])
				ev = i;
		uint64_t when = s->when[ev];
		/* El manejador puede reprogramar su propia ranura. */
		s->when[ev] = RETRO64_SCHED_NEVER;
		sched_update_next(s);
		if (s->fn[ev])
			s->fn[ev](s->ctx[ev], when);
	}
}
