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

/* Pulsa y suelta un botón. */
static enum pausectl_action tap(uint32_t b)
{
	enum pausectl_action a = poll1(b, false, true);
	poll1(0, false, true);
	return a;
}

/* Abre el menú de pausa (START + SELECT) y lo deja soltado. */
static void open_menu(void)
{
	poll1(B(SELECT), false, true);
	poll1(QUIT, false, true);
	poll1(0, false, true);
}

/* Abre el menú y va a la fila de salir y la acepta. */
static enum pausectl_action choose_exit(void)
{
	open_menu();
	tap(B(UP));  /* da la vuelta: Continuar → Salir */
	CHECK_EQ(pc.menu_sel, PM_EXIT);
	return tap(B(CROSS));
}

/* START + SELECT abre el menú (START no llega al juego) y pausa; ○ lo cierra
 * sin que el ○ llegue como B hasta soltarse. */
static void menu_opens_and_closes(void)
{
	pausectl_init(&pc, true);
	poll1(B(SELECT), false, true);
	CHECK_EQ(game[0], B(SELECT));
	CHECK_EQ(poll1(QUIT, false, true), PAUSECTL_NONE);
	CHECK(pc.menu && !pc.dialog);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_MENU);
	CHECK_EQ(pc.menu_sel, PM_RESUME);
	CHECK_EQ(game[0], 0);
	for (int i = 0; i < 5; i++)
		CHECK_EQ(poll1(QUIT, false, true), PAUSECTL_NONE);
	CHECK(pc.menu);
	poll1(0, false, true);
	poll1(B(RIGHT), false, true);
	CHECK_EQ(game[0], 0);  /* con el menú, nada llega al juego */
	poll1(0, false, true);
	CHECK_EQ(poll1(B(CIRCLE), false, true), PAUSECTL_NONE);
	CHECK(!pc.menu);
	CHECK_EQ(pausectl_reasons(&pc), 0);
	CHECK_EQ(game[0], 0);
	poll1(B(CIRCLE) | B(CROSS), false, true);
	CHECK_EQ(game[0], B(CROSS));
	poll1(0, false, true);
	poll1(B(CIRCLE), false, true);
	CHECK_EQ(game[0], B(CIRCLE));
}

/* El atajo otra vez, START o «Continuar» también cierran el menú. */
static void menu_other_closers(void)
{
	pausectl_init(&pc, true);
	open_menu();
	poll1(QUIT, false, true);
	CHECK(!pc.menu);
	CHECK_EQ(game[0], 0);  /* START tragado: no pausa el juego al volver */
	poll1(0, false, true);
	poll1(B(START), false, true);
	CHECK_EQ(game[0], B(START));
	poll1(0, false, true);
	open_menu();
	tap(B(START));
	CHECK(!pc.menu);
	open_menu();
	tap(B(CROSS));  /* Continuar */
	CHECK(!pc.menu);
	CHECK_EQ(pc.menu_do, -1);
}

/* ↑↓ dan la vuelta; guardar y cargar piden la acción y cierran; la ranura
 * cambia con ← → y × sin cerrar; FPS pide el cambio sin cerrar. */
static void menu_rows(void)
{
	pausectl_init(&pc, true);
	open_menu();
	tap(B(DOWN));
	CHECK_EQ(pc.menu_sel, PM_SAVE);
	tap(B(UP));
	tap(B(UP));
	CHECK_EQ(pc.menu_sel, PM_EXIT);
	tap(B(DOWN));
	CHECK_EQ(pc.menu_sel, PM_RESUME);
	tap(B(DOWN));
	tap(B(CROSS));
	CHECK(!pc.menu);
	CHECK_EQ(pc.menu_do, PM_SAVE);
	pc.menu_do = -1;
	open_menu();
	tap(B(DOWN));
	tap(B(DOWN));
	CHECK_EQ(pc.menu_sel, PM_LOAD);
	tap(B(CROSS));
	CHECK(!pc.menu);
	CHECK_EQ(pc.menu_do, PM_LOAD);
	pc.menu_do = -1;
	open_menu();
	for (int i = 0; i < PM_SLOT; i++)
		tap(B(DOWN));
	tap(B(RIGHT));
	CHECK(pc.menu);
	CHECK_EQ(pc.menu_do, PM_SLOT);
	CHECK_EQ(pc.menu_arg, 1);
	pc.menu_do = -1;
	tap(B(LEFT));
	CHECK_EQ(pc.menu_do, PM_SLOT);
	CHECK_EQ(pc.menu_arg, -1);
	pc.menu_do = -1;
	tap(B(CROSS));
	CHECK(pc.menu);
	CHECK_EQ(pc.menu_do, PM_SLOT);
	CHECK_EQ(pc.menu_arg, 1);
	pc.menu_do = -1;
	for (int i = PM_SLOT; i < PM_FPS; i++)
		tap(B(DOWN));
	CHECK_EQ(pc.menu_sel, PM_FPS);
	tap(B(CROSS));
	CHECK(pc.menu);
	CHECK_EQ(pc.menu_do, PM_FPS);
}

/* «Trucos» abre la lista si hay; si no, avisa sin cerrar el menú. */
static void menu_cheats_row(void)
{
	pausectl_init(&pc, true);
	open_menu();
	for (int i = 0; i < PM_CHEATS; i++)
		tap(B(DOWN));
	tap(B(CROSS));
	CHECK(pc.menu && !pc.panel);
	CHECK_EQ(pc.menu_do, PM_CHEATS);
	pc.menu_do = -1;
	pc.panel_avail = true;
	pc.panel_rows = 10;
	tap(B(CROSS));
	CHECK(!pc.menu && pc.panel);
	CHECK_EQ(pc.menu_do, -1);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_PANEL);
}

/* «Imagen»: su página; ←→ y × piden cambiar el valor, × en «Guardar» pide
 * guardar; ○ o «Volver» regresan al menú de pausa y START cierra todo, sin
 * que el START llegue al juego hasta soltarse. */
static void menu_image_page(void)
{
	pausectl_init(&pc, true);
	open_menu();
	for (int i = 0; i < PM_IMAGE; i++)
		tap(B(DOWN));
	CHECK_EQ(pc.menu_sel, PM_IMAGE);
	tap(B(CROSS));
	CHECK(pc.image && !pc.menu);
	CHECK_EQ(pc.image_sel, IM_FILTER);
	CHECK_EQ(pc.image_do, -1);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_MENU);
	poll1(B(RIGHT), false, true);
	CHECK_EQ(game[0], 0);
	poll1(0, false, true);
	CHECK_EQ(pc.image_do, IM_FILTER);
	CHECK_EQ(pc.image_arg, 1);
	pc.image_do = -1;
	/* Con la máscara hay fila «Densidad», con su valor. */
	pc.image_density = true;
	tap(B(DOWN));
	CHECK_EQ(pc.image_sel, IM_DENSITY);
	tap(B(RIGHT));
	CHECK_EQ(pc.image_do, IM_DENSITY);
	CHECK_EQ(pc.image_arg, 1);
	pc.image_do = -1;
	tap(B(CROSS));  /* × como → */
	CHECK_EQ(pc.image_do, IM_DENSITY);
	CHECK_EQ(pc.image_arg, 1);
	pc.image_do = -1;
	tap(B(LEFT));
	CHECK_EQ(pc.image_do, IM_DENSITY);
	CHECK_EQ(pc.image_arg, -1);
	pc.image_do = -1;
	tap(B(UP));
	CHECK_EQ(pc.image_sel, IM_FILTER);
	/* Sin ella se salta. */
	pc.image_density = false;
	tap(B(DOWN));
	CHECK_EQ(pc.image_sel, IM_ASPECT);
	tap(B(UP));
	CHECK_EQ(pc.image_sel, IM_FILTER);
	tap(B(DOWN));
	for (int i = IM_ASPECT; i < IM_SCALE; i++)
		tap(B(DOWN));
	CHECK_EQ(pc.image_sel, IM_SCALE);
	tap(B(LEFT));
	CHECK_EQ(pc.image_do, IM_SCALE);
	CHECK_EQ(pc.image_arg, -1);
	pc.image_do = -1;
	tap(B(CROSS));
	CHECK_EQ(pc.image_do, IM_SCALE);
	CHECK_EQ(pc.image_arg, 1);
	pc.image_do = -1;
	tap(B(DOWN));
	CHECK_EQ(pc.image_sel, IM_INTERP);
	tap(B(RIGHT));
	CHECK_EQ(pc.image_do, IM_INTERP);
	CHECK_EQ(pc.image_arg, 1);
	pc.image_do = -1;
	tap(B(DOWN));
	CHECK_EQ(pc.image_sel, IM_SAVE);
	tap(B(RIGHT));
	CHECK_EQ(pc.image_do, -1);  /* «Guardar» no tiene valor */
	tap(B(CROSS));
	CHECK_EQ(pc.image_do, IM_SAVE);
	CHECK(pc.image);
	pc.image_do = -1;
	tap(B(CIRCLE));
	CHECK(!pc.image && pc.menu);
	CHECK_EQ(pc.menu_sel, PM_IMAGE);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_MENU);
	tap(B(CROSS));
	CHECK(pc.image);
	tap(B(UP));
	CHECK_EQ(pc.image_sel, IM_BACK);  /* da la vuelta */
	tap(B(CROSS));
	CHECK(!pc.image && pc.menu);
	tap(B(CROSS));
	CHECK(pc.image);
	poll1(B(START), false, true);
	CHECK(!pc.image && !pc.menu);
	CHECK_EQ(pausectl_reasons(&pc), 0);
	CHECK_EQ(game[0], 0);
	poll1(0, false, true);
	poll1(B(START), false, true);
	CHECK_EQ(game[0], B(START));
	poll1(0, false, true);
	/* START + SELECT también cierra todo desde «Imagen». */
	open_menu();
	for (int i = 0; i < PM_IMAGE; i++)
		tap(B(DOWN));
	tap(B(CROSS));
	CHECK(pc.image);
	poll1(B(SELECT), false, true);
	poll1(QUIT, false, true);
	CHECK(!pc.image && !pc.menu);
	poll1(0, false, true);
	pausectl_game_end(&pc);
	CHECK(!pc.image);
	CHECK_EQ(pc.image_do, -1);
}

/* «Salir»: con confirmación pregunta; sin ella sale directo. */
static void menu_exit_row(void)
{
	pausectl_init(&pc, true);
	CHECK_EQ(choose_exit(), PAUSECTL_NONE);
	CHECK(pc.dialog && !pc.menu);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_CONFIRM);
	CHECK_EQ(game[0], 0);
	CHECK_EQ(tap(B(CIRCLE)), PAUSECTL_NONE);
	CHECK(!pc.dialog);
	CHECK_EQ(pausectl_reasons(&pc), 0);
	pausectl_init(&pc, false);
	CHECK_EQ(choose_exit(), PAUSECTL_EXIT);
}

/* Los dos «Reiniciar» preguntan siempre (también con confirmar = no), el
 * cuadro dice cuál es, × devuelve su acción y ○ vuelve al juego sin hacer nada. */
static enum pausectl_action choose_restart(int row)
{
	open_menu();
	for (int i = 0; i < row; i++)
		tap(B(DOWN));
	CHECK_EQ(pc.menu_sel, row);
	return tap(B(CROSS));
}

static void menu_restart_rows(void)
{
	CHECK_EQ(PM_RESET + 1, PM_RESTART);
	CHECK_EQ(PM_RESTART + 1, PM_EXIT);
	for (int confirm = 0; confirm < 2; confirm++) {
		pausectl_init(&pc, confirm);
		CHECK_EQ(choose_restart(PM_RESET), PAUSECTL_NONE);
		CHECK(pc.dialog && !pc.menu);
		CHECK_EQ(pc.dialog_for, PAUSECTL_RESET);
		CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_CONFIRM);
		CHECK_EQ(game[0], 0);
		CHECK_EQ(poll1(B(CROSS), false, true), PAUSECTL_RESET);
		CHECK(!pc.dialog);
		poll1(0, false, true);
		CHECK_EQ(pausectl_reasons(&pc), 0);

		pausectl_init(&pc, confirm);
		CHECK_EQ(choose_restart(PM_RESTART), PAUSECTL_NONE);
		CHECK(pc.dialog);
		CHECK_EQ(pc.dialog_for, PAUSECTL_RESTART);
		CHECK_EQ(poll1(B(CROSS), false, true), PAUSECTL_RESTART);
		poll1(0, false, true);

		/* ○ en el cuadro: el juego sigue y no se pide nada. */
		pausectl_init(&pc, confirm);
		choose_restart(PM_RESET);
		CHECK_EQ(tap(B(CIRCLE)), PAUSECTL_NONE);
		CHECK(!pc.dialog);
		CHECK_EQ(pausectl_reasons(&pc), 0);
	}
	/* Salir sigue pidiendo su cuadro y devolviendo PAUSECTL_EXIT. */
	pausectl_init(&pc, true);
	CHECK_EQ(choose_exit(), PAUSECTL_NONE);
	CHECK_EQ(pc.dialog_for, PAUSECTL_EXIT);
	CHECK_EQ(poll1(B(CROSS), false, true), PAUSECTL_EXIT);
}

/* × en el cuadro sale; lo apretado no llega a lo siguiente (el menú de
 * Salsa64) hasta soltarse. */
static void confirm_yes_exits(void)
{
	pausectl_init(&pc, true);
	choose_exit();
	CHECK(pc.dialog);
	CHECK_EQ(poll1(B(CROSS), false, true), PAUSECTL_EXIT);
	CHECK(!pc.dialog);
	pausectl_game_end(&pc);
	CHECK_EQ(poll1(B(CROSS), false, false), PAUSECTL_NONE);
	CHECK_EQ(game[0], 0);
	poll1(0, false, false);
	poll1(B(CROSS), false, false);
	CHECK_EQ(game[0], B(CROSS));
}

/* En el menú de Salsa64 el atajo no hace nada; START en un mando y SELECT en
 * otro no es el atajo; al terminar el juego el menú se va. */
static void combo_variants(void)
{
	pausectl_init(&pc, true);
	CHECK_EQ(poll1(QUIT, false, false), PAUSECTL_NONE);
	CHECK(!pc.menu && !pc.dialog);
	CHECK_EQ(game[0], QUIT);
	pausectl_init(&pc, true);
	memset(raw, 0, sizeof raw);
	raw[0] = B(START);
	raw[1] = B(SELECT);
	CHECK_EQ(pausectl_pads(&pc, raw, false, true, game), PAUSECTL_NONE);
	CHECK(!pc.menu);
	CHECK_EQ(game[0], B(START));
	CHECK_EQ(game[1], B(SELECT));
	open_menu();
	pausectl_game_end(&pc);
	CHECK(!pc.menu);
	CHECK_EQ(pc.menu_do, -1);
}

/* Con el menú de pausa abierto llega el menú del sistema: el × apretado allá
 * no cuenta; al volver, el × nuevo sí. */
static void menu_survives_system_menu(void)
{
	pausectl_init(&pc, true);
	open_menu();
	tap(B(UP));
	pausectl_system_menu(&pc, true);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_SYSTEM | PAUSECTL_MENU);
	CHECK_EQ(poll1(B(CROSS), true, true), PAUSECTL_NONE);
	CHECK(pc.menu);
	pausectl_system_menu(&pc, false);
	CHECK_EQ(poll1(0, false, true), PAUSECTL_NONE);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_MENU);
	poll1(B(CROSS), false, true);
	CHECK(pc.dialog);
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

/* SELECT + □ abre la lista de trucos con el juego en pausa: el juego no recibe
 * nada, ↑↓ eligen (y repiten), × pide el cambio y ○ cierra sin que el ○ llegue
 * al juego. */
static void cheat_panel(void)
{
	pausectl_init(&pc, true);
	/* Sin trucos: no abre, avisa y el □ no llega al juego. */
	poll1(B(SELECT), false, true);
	CHECK_EQ(poll1(B(SELECT) | B(SQUARE), false, true), PAUSECTL_NONE);
	CHECK(!pc.panel && pc.panel_denied);
	CHECK_EQ(game[0] & B(SQUARE), 0);
	pc.panel_denied = false;
	poll1(0, false, true);
	/* Con trucos. */
	pc.panel_avail = true;
	pc.panel_rows = 20;
	poll1(B(SELECT), false, true);
	poll1(B(SELECT) | B(SQUARE), false, true);
	CHECK(pc.panel);
	CHECK_EQ(pausectl_reasons(&pc), PAUSECTL_PANEL);
	CHECK_EQ(game[0], 0);
	poll1(0, false, true);
	CHECK_EQ(pc.panel_sel, 0);
	/* ↓ una vez, y mantenido repite pasado un rato. */
	poll1(B(DOWN), false, true);
	CHECK_EQ(pc.panel_sel, 1);
	for (int i = 0; i < 10; i++)
		poll1(B(DOWN), false, true);
	CHECK_EQ(pc.panel_sel, 1);  /* todavía no repite */
	for (int i = 0; i < 30; i++)
		poll1(B(DOWN), false, true);
	CHECK(pc.panel_sel > 5);
	poll1(0, false, true);
	int at = pc.panel_sel;
	poll1(B(UP), false, true);
	CHECK_EQ(pc.panel_sel, at - 1);
	poll1(0, false, true);
	/* Saltos con ← → y L1/R1, parados en los extremos. */
	poll1(B(RIGHT), false, true);
	CHECK_EQ(pc.panel_sel, at - 1 + PAUSECTL_PANEL_PAGE > 19 ? 19 : at - 1 + PAUSECTL_PANEL_PAGE);
	poll1(0, false, true);
	pc.panel_sel = 0;
	poll1(B(UP), false, true);
	CHECK_EQ(pc.panel_sel, 19);  /* un paso da la vuelta */
	poll1(0, false, true);
	poll1(B(L1), false, true);
	CHECK_EQ(pc.panel_sel, 11);
	poll1(0, false, true);
	/* × pide cambiar el elegido; el juego no ve el ×. */
	CHECK_EQ(pc.toggle, -1);
	poll1(B(CROSS), false, true);
	CHECK_EQ(pc.toggle, 11);
	CHECK_EQ(game[0], 0);
	pc.toggle = -1;
	poll1(0, false, true);
	/* ○ cierra; el ○ y lo que siga apretado no llegan al juego hasta soltarse. */
	poll1(B(CIRCLE), false, true);
	CHECK(!pc.panel);
	CHECK_EQ(pausectl_reasons(&pc), 0);
	CHECK_EQ(game[0], 0);
	poll1(B(CIRCLE) | B(CROSS), false, true);
	CHECK_EQ(game[0], B(CROSS));  /* el × nuevo sí; el ○ no */
	poll1(0, false, true);
	/* Se reabre con SELECT + □ en la misma fila, y el mismo atajo la cierra. */
	poll1(B(SELECT), false, true);
	poll1(B(SELECT) | B(SQUARE), false, true);
	CHECK(pc.panel && pc.panel_sel == 11);
	poll1(B(SELECT), false, true);
	poll1(B(SELECT) | B(SQUARE), false, true);
	CHECK(!pc.panel);
	poll1(0, false, true);
	/* START cierra; el menú del sistema encima no cierra ni cambia nada. */
	poll1(B(SELECT), false, true);
	poll1(B(SELECT) | B(SQUARE), false, true);
	poll1(0, false, true);
	pausectl_system_menu(&pc, true);
	poll1(B(CROSS) | B(CIRCLE), true, true);
	CHECK(pc.panel && pc.toggle == -1);
	pausectl_system_menu(&pc, false);
	poll1(0, false, true);
	poll1(0, false, true);
	poll1(B(START), false, true);
	CHECK(!pc.panel);
	/* Fuera del juego (menú de Salsa64) no hay lista; al terminar el juego se va. */
	poll1(0, false, true);
	poll1(B(SELECT), false, true);
	poll1(B(SELECT) | B(SQUARE), false, true);
	CHECK(pc.panel);
	pausectl_game_end(&pc);
	CHECK(!pc.panel && !pc.panel_avail);
	/* La ventana de filas sigue al elegido. */
	CHECK_EQ(pausectl_panel_top(0, 20), 0);
	CHECK_EQ(pausectl_panel_top(5, 20), 0);
	CHECK_EQ(pausectl_panel_top(10, 20), 5);
	CHECK_EQ(pausectl_panel_top(19, 20), 9);
	CHECK_EQ(pausectl_panel_top(3, 5), 0);
}

void t_suite_pausectl(void)
{
	RUN(system_menu_pauses_and_releases);
	RUN(cheat_panel);
	RUN(intercept_alone_pauses);
	RUN(menu_opens_and_closes);
	RUN(menu_other_closers);
	RUN(menu_rows);
	RUN(menu_cheats_row);
	RUN(menu_image_page);
	RUN(menu_exit_row);
	RUN(menu_restart_rows);
	RUN(confirm_yes_exits);
	RUN(combo_variants);
	RUN(menu_survives_system_menu);
	RUN(dialog_layout_fits);
	RUN(pause_gap_keeps_rhythm);
}
