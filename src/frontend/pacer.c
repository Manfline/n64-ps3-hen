/*
 * Salsa64 — ritmo de la emulación. Ver pacer.h.
 */
#include "pacer.h"

#include <string.h>

void pacer_init(struct pacer *p, int enabled, pacer_sleep_fn sleep, void *ctx)
{
	memset(p, 0, sizeof *p);
	p->enabled = enabled;
	p->sleep = sleep;
	p->ctx = ctx;
}

uint64_t pacer_wait(struct pacer *p, uint64_t now_us)
{
	uint64_t now_ns = now_us * 1000;
	if (!p->enabled)
		return 0;
	if (!p->started) {
		p->started = 1;
		p->next_ns = now_ns;
		return 0;
	}
	if (now_ns < p->next_ns) {
		if (p->next_ns - now_ns > PACER_MAX_AHEAD_NS) {
			p->next_ns = now_ns;  /* nadie espera minutos: referencia nueva */
			p->resets++;
			return 0;
		}
		uint64_t us = (p->next_ns - now_ns + 999) / 1000;
		p->waits++;
		p->us_requested += us;
		if (p->sleep)
			p->sleep(p->ctx, us);
		return us;
	}
	if (now_ns - p->next_ns > PACER_MAX_BEHIND_NS) {
		p->next_ns = now_ns;  /* sin deuda */
		p->resets++;
	}
	return 0;
}

void pacer_advance(struct pacer *p, uint64_t cycles)
{
	if (!p->enabled || !p->started)
		return;
	/* 1 ciclo del VR4300 = 1e9 / 93 750 000 ns = 32/3 ns, sin perder el resto. */
	uint64_t t = cycles * 32 + p->rem;
	p->next_ns += t / 3;
	p->rem = (uint32_t)(t % 3);
}

void pacer_nudge(struct pacer *p, int64_t ns)
{
	if (!p->enabled || !p->started)
		return;
	if (ns < 0 && (uint64_t)-ns > p->next_ns)
		p->next_ns = 0;
	else
		p->next_ns += (uint64_t)ns;
}
