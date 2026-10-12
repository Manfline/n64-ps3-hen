/*
 * Salsa64 — pruebas del contador de velocidad (src/frontend/osd_fps.c).
 */
#include "test.h"

#include <string.h>

#include "frontend/osd.h"

/* NTSC a velocidad completa: 59,83 campos/s, 1 566 833 ciclos por campo;
 * el juego dibuja a 30 y la pantalla presenta a 60. */
static void fps_full_speed(void)
{
	struct osd_fps f;
	memset(&f, 0, sizeof f);
	f.on = true;
	char text[96];
	osd_fps_text(&f, text, sizeof text);
	CHECK(strstr(text, "…") != NULL);  /* todavía sin medir */
	uint64_t cyc = 0;
	uint32_t fields = 0, frames = 0, shown = 0;
	for (uint64_t t = 1000; t <= 1000 + 1000000; t += 1000000 / 60) {
		osd_fps_sample(&f, t, fields, cyc, frames, shown);
		fields++;
		cyc += 1566833;
		frames += fields & 1;
		shown++;
	}
	CHECK(f.valid);
	CHECK(f.vi_rate > 59.0 && f.vi_rate < 61.0);
	CHECK(f.speed > 0.99 && f.speed < 1.01);
	CHECK(f.game_rate > 29.0 && f.game_rate < 31.0);
	CHECK(f.screen_rate > 59.0 && f.screen_rate < 61.0);
	osd_fps_text(&f, text, sizeof text);
	CHECK(strstr(text, "VI ") == text);
	CHECK(strstr(text, "(100 %)") != NULL);
	CHECK(strstr(text, " · juego ") != NULL);
	CHECK(strchr(text, '.') == NULL);  /* coma decimal */
}

/* Un estado cargado hace retroceder el campo: la ventana empieza de nuevo
 * en vez de dar números negativos. */
static void fps_field_goes_back(void)
{
	struct osd_fps f;
	memset(&f, 0, sizeof f);
	f.on = true;
	osd_fps_sample(&f, 1000, 5000, 5000ull * 1566833, 0, 0);
	osd_fps_sample(&f, 600000, 100, 100ull * 1566833, 10, 30);
	CHECK(!f.valid);
	osd_fps_sample(&f, 1200000, 130, 130ull * 1566833, 25, 66);
	CHECK(f.valid);
	CHECK(f.vi_rate > 49.0 && f.vi_rate < 51.0);
	osd_fps_reset(&f);
	CHECK(f.on);
	CHECK(!f.valid);
}

void t_suite_osd(void)
{
	RUN(fps_full_speed);
	RUN(fps_field_goes_back);
}
