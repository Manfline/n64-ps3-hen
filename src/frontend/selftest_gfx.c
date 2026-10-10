/*
 * Salsa64 — verificación del frontend con gfxtest. Ver selftest_gfx.h.
 */
#include "selftest_gfx.h"

#include <stdio.h>
#include <string.h>

#include "../../tests/roms/src/gfxtest/expect.h"
#include "platform/ps3/log.h"

static const struct gfxtest_point points[] = { GFXTEST_POINTS };
#define NPOINTS (sizeof points / sizeof points[0])
#define PPM_PATH SALSA64_DATA_DIR "/gfxtest.ppm"

static struct {
	int isv;       /* 1 OK, -1 FALLO */
	int screen;    /* 1 bien, -1 mal */
	int waited;    /* campos presentados desde el OK */
} st;

void gfxtest_start(void)
{
	memset(&st, 0, sizeof st);
}

void gfxtest_isv(const uint8_t *text, uint32_t len)
{
	static const char ok[] = GFXTEST_OK_TEXT, bad[] = GFXTEST_FAIL_TEXT;
	if (len >= sizeof ok - 1 && !memcmp(text, ok, sizeof ok - 1))
		st.isv = 1;
	else if (len >= sizeof bad - 1 && !memcmp(text, bad, sizeof bad - 1))
		st.isv = -1;
}

bool gfxtest_want_screen(void)
{
	/* Dos campos después del OK: la imagen ya es de una tarea completa. */
	return st.isv > 0 && !st.screen && ++st.waited >= 2;
}

static uint32_t screen_px(const uint32_t *px, uint32_t pitch, const struct salsa64_vi_frame *f,
			  const rsxkit_rect *r, float x, float y)
{
	uint32_t sx = r->x + (uint32_t)(x * r->w / f->width);
	uint32_t sy = r->y + (uint32_t)(y * r->h / f->height);
	return px[sy * (pitch / 4) + sx] & 0xFFFFFF;
}

/* La pantalla muestreada en el centro de cada píxel de la N64. */
static void dump(const uint32_t *px, uint32_t pitch, const struct salsa64_vi_frame *f, const rsxkit_rect *r)
{
	FILE *out = fopen(PPM_PATH, "wb");
	if (!out)
		return;
	fprintf(out, "P6\n%u %u\n255\n", f->width, f->height);
	static uint8_t line[3 * 640];
	for (uint32_t y = 0; y < f->height; y++) {
		for (uint32_t x = 0; x < f->width && x < 640; x++) {
			uint32_t c = screen_px(px, pitch, f, r, x + 0.5f, y + 0.5f);
			line[3 * x] = (uint8_t)(c >> 16);
			line[3 * x + 1] = (uint8_t)(c >> 8);
			line[3 * x + 2] = (uint8_t)c;
		}
		fwrite(line, 3, f->width, out);
	}
	fclose(out);
	salsa64_log("gfxtest: imagen en %s", PPM_PATH);
}

void gfxtest_screen(const struct salsa64_vi_frame *f, const rsxkit_rect *r)
{
	uint32_t pitch;
	const uint32_t *px = rsxkit_back_buffer(&pitch);
	unsigned bad = 0;
	for (unsigned i = 0; i < NPOINTS; i++) {
		uint32_t got = screen_px(px, pitch, f, r, points[i].x + 0.5f, points[i].y + 0.5f);
		uint32_t want = points[i].rgb;
		int ok = 1;
		for (int k = 0; k < 24; k += 8) {
			int d = (int)((got >> k) & 0xFF) - (int)((want >> k) & 0xFF);
			if (d > GFXTEST_TOL || d < -GFXTEST_TOL)
				ok = 0;
		}
		if (!ok) {
			bad++;
			salsa64_log("gfxtest: (%u, %u) %s: %06x, se esperaba %06x", points[i].x, points[i].y,
				    points[i].what, got, want);
		}
	}
	st.screen = bad ? -1 : 1;
	salsa64_log("gfxtest: pantalla %s (%u de %u puntos bien; rectángulo %ux%u en %d,%d)", bad ? "MAL" : "bien",
		    (unsigned)NPOINTS - bad, (unsigned)NPOINTS, r->w, r->h, r->x, r->y);
	dump(px, pitch, f, r);
}

enum selftest_state gfxtest_state(uint32_t fields)
{
	if (st.isv < 0 || st.screen < 0)
		return SELFTEST_FAILED;
	if (st.isv > 0 && st.screen > 0)
		return SELFTEST_OK;
	return fields >= SELFTEST_MAX_FIELDS ? SELFTEST_FAILED : SELFTEST_RUNNING;
}
