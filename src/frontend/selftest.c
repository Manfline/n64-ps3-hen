/*
 * Salsa64 — verificación del frontend con la ROM vitest. Ver selftest.h.
 *
 * Puntos del patrón de vitest (main.c de la ROM): centro de cada barra en
 * y = 80, extremos de la rampa en y = 180 y el fondo en y = 234 (el cuadro
 * naranja nunca baja de y = 229).
 */
#include "selftest.h"

#include <string.h>

#include "core/endian.h"
#include "platform/ps3/log.h"

#define TOL 6  /* diferencia admitida por canal en la pantalla */

static const struct {
	uint16_t x, y;
	uint32_t rgb;
} points[] = {
	{ 20, 80, 0xFFFFFF }, { 60, 80, 0xFFFF00 }, { 100, 80, 0x00FFFF }, { 140, 80, 0x00FF00 },
	{ 180, 80, 0xFF00FF }, { 220, 80, 0xFF0000 }, { 260, 80, 0x0000FF }, { 300, 80, 0x000000 },
	{ 0, 180, 0x000000 }, { 319, 180, 0xFFFFFF }, { 160, 234, 0x202060 },
};
#define NPOINTS (sizeof points / sizeof points[0])

static struct {
	int isv;           /* 1 = "vitest: OK", -1 = "vitest: FALLO" */
	int core[2];       /* RDRAM verificada en RGBA5551 [0] y RGBA8888 [1] */
	int screen[2];
	int failed;
} st;

void selftest_start(void)
{
	memset(&st, 0, sizeof st);
}

void selftest_isv(const uint8_t *text, uint32_t len)
{
	static const char ok[] = "vitest: OK", bad[] = "vitest: FALLO";
	if (len >= sizeof ok - 1 && !memcmp(text, ok, sizeof ok - 1))
		st.isv = 1;
	else if (len >= sizeof bad - 1 && !memcmp(text, bad, sizeof bad - 1))
		st.isv = -1;
}

/* Color del píxel (x, y) del framebuffer del VI, como 0xRRGGBB de 8 bits
 * (los 5 bits se extienden repitiendo los altos, como el RSX). */
static uint32_t vi_pixel(const struct salsa64_bus *bus, const struct salsa64_vi_frame *f, uint32_t x, uint32_t y)
{
	const uint8_t *p = bus->rdram + f->origin + y * f->stride + x * f->bpp;
	if (f->bpp == 4)
		return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
	uint32_t v = salsa64_rd16(p), rgb = 0;
	for (int k = 0; k < 3; k++) {
		uint32_t c = (v >> (11 - 5 * k)) & 31;
		rgb = rgb << 8 | (c << 3 | c >> 2);
	}
	return rgb;
}

/* El color esperado tal como queda en un framebuffer de bpp bytes. */
static uint32_t expected(uint32_t rgb, uint32_t bpp)
{
	if (bpp == 4)
		return rgb;
	uint32_t out = 0;
	for (int k = 16; k >= 0; k -= 8) {
		uint32_t c = ((rgb >> k) & 0xFF) >> 3;
		out = out << 8 | (c << 3 | c >> 2);
	}
	return out;
}

static bool close_rgb(uint32_t a, uint32_t b, int tol)
{
	for (int k = 0; k < 24; k += 8) {
		int d = (int)((a >> k) & 0xFF) - (int)((b >> k) & 0xFF);
		if (d > tol || d < -tol)
			return false;
	}
	return true;
}

bool selftest_frame(const struct salsa64_bus *bus, const struct salsa64_vi_frame *f)
{
	int k = f->bpp == 4;
	if (st.failed || st.core[k] || f->width != 320 || f->height < 235)
		return false;
	for (unsigned i = 0; i < NPOINTS; i++) {
		uint32_t got = vi_pixel(bus, f, points[i].x, points[i].y);
		uint32_t want = expected(points[i].rgb, f->bpp);
		if (got != want) {
			salsa64_log("selftest: RDRAM %s (%u,%u) = %06x, se esperaba %06x",
				    k ? "RGBA8888" : "RGBA5551", points[i].x, points[i].y, got, want);
			st.failed = 1;
			return false;
		}
	}
	st.core[k] = 1;
	salsa64_log("selftest: framebuffer %s en la RDRAM correcto (origen 0x%06x)",
		    k ? "RGBA8888" : "RGBA5551", f->origin);
	return true;
}

void selftest_screen(const struct salsa64_vi_frame *f, const rsxkit_rect *r)
{
	int k = f->bpp == 4;
	uint32_t pitch;
	const uint32_t *px = rsxkit_back_buffer(&pitch);
	for (unsigned i = 0; i < NPOINTS; i++) {
		uint32_t sx = r->x + (uint32_t)((points[i].x + 0.5f) * r->w / f->width);
		uint32_t sy = r->y + (uint32_t)((points[i].y + 0.5f) * r->h / f->height);
		uint32_t got = px[sy * (pitch / 4) + sx] & 0xFFFFFF;
		uint32_t want = expected(points[i].rgb, f->bpp);
		if (!close_rgb(got, want, TOL)) {
			salsa64_log("selftest: pantalla %s (%u,%u)→(%u,%u) = %06x, se esperaba %06x",
				    k ? "RGBA8888" : "RGBA5551", points[i].x, points[i].y, sx, sy, got, want);
			st.failed = 1;
			return;
		}
	}
	st.screen[k] = 1;
	salsa64_log("selftest: pantalla %s correcta (rectángulo %ux%u en %d,%d)",
		    k ? "RGBA8888" : "RGBA5551", r->w, r->h, r->x, r->y);
}

enum selftest_state selftest_state(uint32_t fields)
{
	if (st.failed || st.isv < 0)
		return SELFTEST_FAILED;
	if (st.isv > 0 && st.core[0] && st.core[1] && st.screen[0] && st.screen[1])
		return SELFTEST_OK;
	return fields >= SELFTEST_MAX_FIELDS ? SELFTEST_FAILED : SELFTEST_RUNNING;
}
