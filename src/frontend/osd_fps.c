/*
 * Salsa64 — cuentas del contador de velocidad (portable, con pruebas en
 * tests/unit/test_osd.c). Ver osd.h.
 */
#include "osd.h"

#include <stdio.h>
#include <string.h>

#define VR4300_HZ 93750000.0

void osd_fps_reset(struct osd_fps *f)
{
	bool on = f->on;
	memset(f, 0, sizeof *f);
	f->on = on;
}

void osd_fps_sample(struct osd_fps *f, uint64_t now_us, uint32_t fields, uint64_t cycles, uint32_t game_frames,
		    uint32_t presented)
{
	if (!f->t0 || fields < f->fields0 || cycles < f->cycles0) {
		/* Primera muestra, o el campo retrocedió (estado cargado). */
		f->t0 = now_us ? now_us : 1;
		f->cycles0 = cycles;
		f->fields0 = fields;
		f->frames0 = game_frames;
		f->presented0 = presented;
		return;
	}
	uint64_t dt = now_us - f->t0;
	if (dt < OSD_FPS_US)
		return;
	double s = dt / 1e6;
	f->vi_rate = (fields - f->fields0) / s;
	f->speed = (cycles - f->cycles0) / VR4300_HZ / s;
	f->game_rate = (game_frames - f->frames0) / s;
	f->screen_rate = (presented - f->presented0) / s;
	f->valid = true;
	f->t0 = now_us;
	f->cycles0 = cycles;
	f->fields0 = fields;
	f->frames0 = game_frames;
	f->presented0 = presented;
}

/* Con coma decimal, como el resto de los textos. */
static void comma(char *s)
{
	for (; *s; s++)
		if (*s == '.')
			*s = ',';
}

void osd_fps_text(const struct osd_fps *f, char *buf, unsigned size)
{
	if (!f->valid) {
		snprintf(buf, size, "VI … · juego … · pantalla …");
		return;
	}
	snprintf(buf, size, "VI %.1f (%.0f %%) · juego %.0f · pantalla %.0f", f->vi_rate, f->speed * 100.0,
		 f->game_rate, f->screen_rate);
	comma(buf);
}
