/*
 * Salsa64 — pruebas del aviso de juego colgado (src/frontend/cuelgue.c): solo
 * avisa de un juego que antes leía los controles o dibujaba, tras
 * CUELGUE_US sin hacer ninguna de las dos cosas con el VI avanzando, una vez
 * por episodio, y el texto trae lo que hace falta para diagnosticarlo.
 */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "frontend/cuelgue.h"

static char said[16][400];
static int nsaid;

static void fake_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (nsaid < 16)
		vsnprintf(said[nsaid++], sizeof said[0], fmt, ap);
	va_end(ap);
}

static struct cuelgue cg;
static uint64_t clock_us;

/* Medio segundo de reloj con 30 campos más en el VI y, si se pide, un
 * control leído y una tarea gráfica más. */
static bool tick(bool active, bool expect, bool vi_runs)
{
	struct salsa64_bus *bus = &t_n64.bus;
	if (vi_runs)
		bus->vi.fields += 30;
	if (active) {
		bus->pif.reads[0]++;
		bus->rsp.stats.gfx++;
	}
	clock_us += 500000;
	return cuelgue_watch(&cg, &t_n64, clock_us, expect, NULL, fake_log);
}

static void cuelgue_avisa_una_vez(void)
{
	nsaid = 0;
	clock_us = 1000000;
	cuelgue_reset(&cg);
	for (int i = 0; i < 12; i++)
		CHECK(!tick(true, true, true));  /* vivo: nada */
	/* Sin actividad: nada hasta CUELGUE_US, y entonces avisa una sola vez. */
	int fired = 0;
	for (int i = 0; i < 40; i++)
		fired += tick(false, true, true);
	CHECK_EQ(fired, 1);
	CHECK(nsaid >= 8);
	CHECK(strstr(said[0], "no lee los controles"));
	/* Vuelve a vivir: lo dice y puede avisar de nuevo en otro episodio. */
	CHECK(!tick(true, true, true));
	CHECK(strstr(said[nsaid - 1], "volvió"));
	for (int i = 0; i < 12; i++)
		tick(true, true, true);
	fired = 0;
	for (int i = 0; i < 40; i++)
		fired += tick(false, true, true);
	CHECK_EQ(fired, 1);
}

static void cuelgue_no_avisa_sin_motivo(void)
{
	/* Un juego que nunca dio señales de vida: no está colgado. */
	nsaid = 0;
	clock_us = 1000000;
	cuelgue_reset(&cg);
	for (int i = 0; i < 60; i++)
		CHECK(!tick(false, true, true));
	CHECK_EQ(nsaid, 0);
	/* En pausa (o sin juego) tampoco, aunque no haya actividad. */
	cuelgue_reset(&cg);
	for (int i = 0; i < 12; i++)
		tick(true, true, true);
	for (int i = 0; i < 60; i++)
		CHECK(!tick(false, false, true));
	CHECK_EQ(nsaid, 0);
	/* Ni con el VI quieto (de eso se ocupa el latido de los hilos). */
	cuelgue_reset(&cg);
	for (int i = 0; i < 12; i++)
		tick(true, true, true);
	for (int i = 0; i < 60; i++)
		CHECK(!tick(false, true, false));
	CHECK_EQ(nsaid, 0);
}

static void cuelgue_texto(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	bus->mi.intr = 0x15;
	bus->mi.mask = 0x3F;
	bus->rsp.stats.dp_sched = 7;
	bus->rsp.stats.dp_fired = 6;
	bus->rsp.stats.gfx_nosync = 2;
	salsa64_sched_set(&t_n64.sched, SALSA64_EV_DP_SYNC, CPU->cycles + 5000);
	char text[2048];
	unsigned n = cuelgue_state_text(&t_n64, text, sizeof text);
	CHECK(n > 100 && n < sizeof text);
	CHECK(strstr(text, "MI: INTR 15 máscara 3f"));
	CHECK(strstr(text, "DP programadas 7, levantadas 6"));
	CHECK(strstr(text, "sin SYNC_FULL ni corte 2"));
	CHECK(strstr(text, "DP +5000"));
	CHECK(strstr(text, "tarea -"));
	/* Un búfer chico se corta sin pasarse. */
	char small[40];
	n = cuelgue_state_text(&t_n64, small, sizeof small);
	CHECK(n < sizeof small);
	salsa64_sched_cancel(&t_n64.sched, SALSA64_EV_DP_SYNC);
}

void t_suite_cuelgue(void)
{
	RUN(cuelgue_avisa_una_vez);
	RUN(cuelgue_no_avisa_sin_motivo);
	RUN(cuelgue_texto);
}
