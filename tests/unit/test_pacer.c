/*
 * Salsa64 — pruebas del ritmo de la emulación (src/frontend/pacer.c): espera
 * cuando va adelantado, no espera cuando va atrasado y olvida la deuda si
 * el atraso es grande; el ritmo medio sale exacto de los ciclos del campo.
 */
#include "test.h"

#include <stddef.h>

#include "frontend/pacer.h"

#define NTSC_FIELD 1567041ull  /* ciclos de un campo NTSC (vi.c) */
#define NTSC_NS    (NTSC_FIELD * 32 / 3)  /* 16 715 104 ns */

static uint64_t slept_us, sleeps;

static void fake_sleep(void *ctx, uint64_t us)
{
	(void)ctx;
	slept_us += us;
	sleeps++;
}

static struct pacer p;

static void begin(int enabled)
{
	slept_us = sleeps = 0;
	pacer_init(&p, enabled, fake_sleep, NULL);
}

static void pacer_ahead_sleeps(void)
{
	begin(1);
	CHECK_EQ(pacer_wait(&p, 1000), 0);  /* primera vez: solo toma la referencia */
	pacer_advance(&p, NTSC_FIELD);
	/* El campo tardó 5 ms: duerme hasta completar los 16,7. */
	uint64_t us = pacer_wait(&p, 6000);
	CHECK_EQ(us, (NTSC_NS - 5000000 + 999) / 1000);
	CHECK_EQ(slept_us, us);
	CHECK_EQ(p.waits, 1);
	CHECK_EQ(p.resets, 0);
}

static void pacer_behind_no_wait_no_debt(void)
{
	begin(1);
	pacer_wait(&p, 0);
	pacer_advance(&p, NTSC_FIELD);
	/* Un campo lento (20 ms): no espera y la referencia no se mueve. */
	CHECK_EQ(pacer_wait(&p, 20000), 0);
	CHECK_EQ(p.resets, 0);
	pacer_advance(&p, NTSC_FIELD);
	/* El siguiente campo rápido recupera solo lo que falta para 2 campos. */
	uint64_t us = pacer_wait(&p, 30000);
	CHECK_EQ(us, (2 * NTSC_NS - 30000000 + 999) / 1000);
	CHECK_EQ(sleeps, 1);
}

static void pacer_far_behind_resets(void)
{
	begin(1);
	pacer_wait(&p, 0);
	pacer_advance(&p, NTSC_FIELD);
	/* Un tirón de 1 s: se olvida la deuda. */
	CHECK_EQ(pacer_wait(&p, 1000000), 0);
	CHECK_EQ(p.resets, 1);
	CHECK_EQ(p.next_ns, 1000000000ull);
	pacer_advance(&p, NTSC_FIELD);
	CHECK_EQ(pacer_wait(&p, 1000000 + 1000), (NTSC_NS - 1000000 + 999) / 1000);
}

static void pacer_exact_rate(void)
{
	begin(1);
	pacer_wait(&p, 0);
	for (int i = 0; i < 300; i++)
		pacer_advance(&p, NTSC_FIELD);
	/* 300 campos: 300 × 1 567 041 × 32 / 3 ns exactos, sin acumular resto. */
	CHECK_EQ(p.next_ns, 300 * NTSC_FIELD * 32 / 3);
	CHECK_EQ(p.rem, 0);
	pacer_advance(&p, 1);
	CHECK_EQ(p.rem, 2);
	pacer_advance(&p, 1);
	CHECK_EQ(p.rem, 1);
	pacer_advance(&p, 1);
	CHECK_EQ(p.rem, 0);
	CHECK_EQ(p.next_ns, 300 * NTSC_FIELD * 32 / 3 + 32);
}

static void pacer_disabled_and_nudge(void)
{
	begin(0);
	CHECK_EQ(pacer_wait(&p, 0), 0);
	pacer_advance(&p, NTSC_FIELD);
	CHECK_EQ(pacer_wait(&p, 1), 0);
	CHECK_EQ(sleeps, 0);
	CHECK_EQ(p.started, 0);

	begin(1);
	pacer_wait(&p, 0);
	pacer_advance(&p, NTSC_FIELD);
	pacer_nudge(&p, 1000000);  /* el audio pide ir 1 ms más despacio */
	CHECK_EQ(pacer_wait(&p, 1000), (NTSC_NS + 1000000 - 1000000 + 999) / 1000);
	pacer_nudge(&p, -(int64_t)(NTSC_NS + 5000000));
	CHECK_EQ(p.next_ns, 0);
}

void t_suite_pacer(void)
{
	RUN(pacer_ahead_sleeps);
	RUN(pacer_behind_no_wait_no_debt);
	RUN(pacer_far_behind_resets);
	RUN(pacer_exact_rate);
	RUN(pacer_disabled_and_nudge);
}
