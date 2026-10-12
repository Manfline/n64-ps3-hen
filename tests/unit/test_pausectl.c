/*
 * Salsa64 — pruebas de la pausa del juego (src/frontend/pausectl.c): el
 * menú del sistema y el cuadro de START + SELECT detienen el juego, lo que
 * se aprieta mientras tanto no le llega y nada queda pegado al volver. Al
 * final, el hueco de tiempo de una pausa contra el ritmo (pacer.c): al
 * volver no hay ráfaga de campos para «recuperar» ni esperas largas.
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include <rsxkit/text.h>

#include "frontend/padmap.h"
#include "frontend/pacer.h"
#include "frontend/pausectl.h"

#define B(x)   (1u << SALSA64_DS3_##x)
#define QUIT   (B(START) | B(SELECT))

static struct pausectl pc;
static uint32_t raw[PAUSECTL_PORTS], game[PAUSECTL_PORTS];

/* Una vuelta con el puerto 1 en `b` (los otros sueltos). */
static enum pausectl_action poll1(uint32_t b, bool intercepted, bool in_game)
{
	memset(raw, 0, sizeof raw);
	raw[0] = b;
	return pausectl_pads(&pc, raw, intercepted, in_game, game);
}

/* Botón PS: con el menú del sistema abierto, el juego no recibe nada y queda
 * en pausa; al cerrarlo sigue y una pulsación nueva llega normal. */
static void system_menu_pauses_and_releases(void)
{
	pausectl_init(&pc, true);
	CHECK_EQ(poll1(B(CROSS), false, true), PAUSECTL_NONE);
	CHECK_EQ(game[0], B(CROSS));
	CHECK_EQ(pausectl_reasons(&pc), 0);
	pausectl_system_menu(&pc, true);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_SYSTEM);
	/* Lo que llegue mientras tanto (aunque la biblioteca lo diera) no cuenta. */
	CHECK_EQ(poll1(B(CROSS) | B(CIRCLE) | QUIT, true, true), PAUSECTL_NONE);
	CHECK_EQ(game[0], 0);
	CHECK(!pc.dialog);
	pausectl_system_menu(&pc, false);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_SYSTEM);  /* los mandos todavía son del sistema */
	CHECK_EQ(poll1(0, false, true), PAUSECTL_NONE);
	CHECK_EQ(pausectl_reasons(&pc), 0);
	CHECK_EQ(game[0], 0);
	CHECK_EQ(poll1(B(CROSS), false, true), PAUSECTL_NONE);
	CHECK_EQ(game[0], B(CROSS));
}

/* Los mandos tomados por el sistema sin aviso de menú también pausan. */
static void intercept_alone_pauses(void)
{
	pausectl_init(&pc, true);
	poll1(B(UP), true, true);
	CHECK_EQ(game[0], 0);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_SYSTEM);
	poll1(0, false, true);
	CHECK_EQ(pausectl_reasons(&pc), 0);
}

/* START + SELECT abre el cuadro (START no llega al juego); ○ lo cierra y el
 * ○ no llega como B hasta soltarse y volver a apretarse. */
static void confirm_no_resumes(void)
{
	pausectl_init(&pc, true);
	poll1(B(SELECT), false, true);
	CHECK_EQ(game[0], B(SELECT));
	CHECK_EQ(poll1(QUIT, false, true), PAUSECTL_NONE);
	CHECK(pc.dialog);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_CONFIRM);
	CHECK_EQ(game[0], 0);
	/* Mantener el atajo no lo cierra ni lo vuelve a abrir. */
	for (int i = 0; i < 5; i++)
		CHECK_EQ(poll1(QUIT, false, true), PAUSECTL_NONE);
	CHECK(pc.dialog);
	poll1(0, false, true);
	poll1(B(UP), false, true);
	CHECK_EQ(game[0], 0);  /* con el cuadro, nada llega al juego */
	CHECK(pc.dialog);
	CHECK_EQ(poll1(B(CIRCLE), false, true), PAUSECTL_NONE);
	CHECK(!pc.dialog);
	CHECK_EQ(pausectl_reasons(&pc), 0);
	CHECK_EQ(game[0], 0);
	poll1(B(CIRCLE) | B(CROSS), false, true);
	CHECK_EQ(game[0], B(CROSS));  /* lo nuevo sí; el ○ del «No», no */
	poll1(0, false, true);
	poll1(B(CIRCLE), false, true);
	CHECK_EQ(game[0], B(CIRCLE));
}

/* × sale; lo apretado no llega a lo siguiente (el menú de Salsa64) hasta
 * soltarse. */
static void confirm_yes_exits(void)
{
	pausectl_init(&pc, true);
	poll1(QUIT, false, true);
	CHECK(pc.dialog);
	poll1(0, false, true);
	CHECK_EQ(poll1(B(CROSS), false, true), PAUSECTL_EXIT);
	CHECK(!pc.dialog);
	pausectl_game_end(&pc);
	CHECK_EQ(poll1(B(CROSS), false, false), PAUSECTL_NONE);
	CHECK_EQ(game[0], 0);
	poll1(0, false, false);
	poll1(B(CROSS), false, false);
	CHECK_EQ(game[0], B(CROSS));
}

/* El atajo otra vez (soltado y apretado de nuevo) cierra el cuadro. */
static void combo_again_closes(void)
{
	pausectl_init(&pc, true);
	poll1(QUIT, false, true);
	CHECK(pc.dialog);
	poll1(B(START), false, true);
	CHECK(pc.dialog);
	CHECK_EQ(poll1(QUIT, false, true), PAUSECTL_NONE);
	CHECK(!pc.dialog);
	CHECK_EQ(game[0], 0);  /* START tragado: no pausa el juego al volver */
	poll1(B(START), false, true);
	CHECK_EQ(game[0], 0);
	CHECK(!pc.dialog);
	poll1(0, false, true);
	poll1(B(START), false, true);
	CHECK_EQ(game[0], B(START));
}

/* Con «confirmar = no», el atajo sale directo; en el menú de Salsa64 no
 * hace nada; START en un mando y SELECT en otro no es el atajo. */
static void combo_variants(void)
{
	pausectl_init(&pc, false);
	CHECK_EQ(poll1(QUIT, false, true), PAUSECTL_EXIT);
	CHECK(!pc.dialog);
	pausectl_init(&pc, true);
	CHECK_EQ(poll1(QUIT, false, false), PAUSECTL_NONE);
	CHECK(!pc.dialog);
	CHECK_EQ(game[0], QUIT);
	pausectl_init(&pc, true);
	memset(raw, 0, sizeof raw);
	raw[0] = B(START);
	raw[1] = B(SELECT);
	CHECK_EQ(pausectl_pads(&pc, raw, false, true, game), PAUSECTL_NONE);
	CHECK(!pc.dialog);
	CHECK_EQ(game[0], B(START));
	CHECK_EQ(game[1], B(SELECT));
}

/* Con el cuadro abierto llega el menú del sistema: el × apretado allá no
 * cuenta; al volver, el × nuevo sí. */
static void dialog_survives_system_menu(void)
{
	pausectl_init(&pc, true);
	poll1(QUIT, false, true);
	poll1(0, false, true);
	pausectl_system_menu(&pc, true);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_SYSTEM | PAUSECTL_CONFIRM);
	CHECK_EQ(poll1(B(CROSS), true, true), PAUSECTL_NONE);
	CHECK(pc.dialog);
	pausectl_system_menu(&pc, false);
	CHECK_EQ(poll1(0, false, true), PAUSECTL_NONE);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_CONFIRM);
	CHECK_EQ(poll1(B(CROSS), false, true), PAUSECTL_EXIT);
}

/* El cuadro cabe en 4:3 y en 16:9, centrado, con el texto adentro. */
static void dialog_layout_fits(void)
{
	static const char *const q[] = { "¿Volver al menú?", "¿Salir al XMB?" };
	const char *a = "×: Sí     ○: No";
	static const float widths[] = { 960.0f, 1280.0f };
	for (unsigned i = 0; i < 2; i++) {
		for (unsigned k = 0; k < 2; k++) {
			struct pausectl_box b;
			float lw = widths[k], lh = 720.0f;
			pausectl_layout(q[i], a, lw, lh, &b);
			CHECK(b.x > 0.0f && b.y > 0.0f && b.x + b.w < lw && b.y + b.h < lh);
			CHECK(b.w < lw / 2);  /* un cuadro chico */
			CHECK(fabsf(b.x + b.w / 2 - lw / 2) < 0.01f);
			/* La pregunta (16 o 14 caracteres) y las respuestas (15). */
			float qw = (i ? 14 : 16) * RSXKIT_FONT_W * b.qscale, aw = 15 * RSXKIT_FONT_W * b.ascale;
			CHECK(b.qx >= b.x && b.qx + qw <= b.x + b.w);
			CHECK(b.ax >= b.x && b.ax + aw <= b.x + b.w);
			CHECK(b.qy >= b.y && b.ay + RSXKIT_FONT_H * b.ascale <= b.y + b.h);
			CHECK(b.ay >= b.qy + RSXKIT_FONT_H * b.qscale);
		}
	}
}

/* El hueco de tiempo de una pausa contra el ritmo, como en emu_thread: cada
 * campo cuesta 5 ms; a los 2 s se abre el menú del sistema 6 s (emulación
 * quieta en el límite del campo, dormida de a 2 ms) y al cerrarlo el ritmo
 * empieza de nuevo. En el segundo siguiente salen los campos de un segundo
 * (ni ráfaga para recuperar los 6 s ni esperas de más de un campo). */
#define NTSC_FIELD 1567041ull  /* ciclos de un campo NTSC (vi.c) */

static uint64_t sim_us, max_sleep_us;

static void sim_sleep(void *ctx, uint64_t us)
{
	(void)ctx;
	sim_us += us;
	if (us > max_sleep_us)
		max_sleep_us = us;
}

static void pause_gap_keeps_rhythm(void)
{
	struct pacer p;
	pausectl_init(&pc, true);
	pacer_init(&p, 1, sim_sleep, NULL);
	sim_us = 1000;
	uint32_t fields = 0, after = 0, paused_fields = 0;
	bool was_paused = false;
	uint64_t resume_us = 0;
	while (sim_us < 9000000) {
		/* Presentación: el menú del sistema de 2 s a 8 s. */
		pausectl_system_menu(&pc, sim_us >= 2000000 && sim_us < 8000000);
		poll1(0, pc.system, true);
		if (pausectl_reasons(&pc)) {
			sim_us += 2000;  /* emu_pause */
			was_paused = true;
			continue;
		}
		if (was_paused) {
			pacer_init(&p, 1, sim_sleep, NULL);
			was_paused = false;
			resume_us = sim_us;
			max_sleep_us = 0;
		}
		pacer_wait(&p, sim_us);
		sim_us += 5000;  /* el campo */
		pacer_advance(&p, NTSC_FIELD);
		fields++;
		if (sim_us >= 2000000 && sim_us < 8000000)
			paused_fields++;
		if (resume_us && sim_us - resume_us <= 1000000)
			after++;
	}
	CHECK(resume_us >= 8000000 && resume_us < 8003000);
	CHECK(paused_fields <= 1);  /* a lo sumo el que estaba en curso */
	CHECK(after >= 58 && after <= 61);
	CHECK(max_sleep_us <= 17000);
	CHECK_EQ(p.resets, 0);
	CHECK(fields >= 3 * 59 && fields <= 3 * 61);  /* 2 s antes y 1 s después */
}

void t_suite_pausectl(void)
{
	RUN(system_menu_pauses_and_releases);
	RUN(intercept_alone_pauses);
	RUN(confirm_no_resumes);
	RUN(confirm_yes_exits);
	RUN(combo_again_closes);
	RUN(combo_variants);
	RUN(dialog_survives_system_menu);
	RUN(dialog_layout_fits);
	RUN(pause_gap_keeps_rhythm);
}
