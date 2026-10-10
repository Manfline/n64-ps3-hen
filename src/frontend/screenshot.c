/*
 * Salsa64 — captura de la pantalla del RSX. Ver screenshot.h.
 */
#include "screenshot.h"

#include <stdio.h>

void salsa64_screenshot(const char *path, const struct salsa64_vi_frame *f, const rsxkit_rect *r)
{
	uint32_t pitch;
	const uint32_t *px = rsxkit_back_buffer(&pitch);
	FILE *out = fopen(path, "wb");
	if (!out || !f->width || !f->height)
		return;
	fprintf(out, "P6\n%u %u\n255\n", f->width, f->height);
	static uint8_t line[3 * 640];
	uint32_t w = f->width < 640 ? f->width : 640;
	for (uint32_t y = 0; y < f->height; y++) {
		uint32_t sy = r->y + (uint32_t)((y + 0.5f) * r->h / f->height);
		for (uint32_t x = 0; x < w; x++) {
			uint32_t sx = r->x + (uint32_t)((x + 0.5f) * r->w / f->width);
			uint32_t c = px[sy * (pitch / 4) + sx];
			line[3 * x] = (uint8_t)(c >> 16);
			line[3 * x + 1] = (uint8_t)(c >> 8);
			line[3 * x + 2] = (uint8_t)c;
		}
		fwrite(line, 3, w, out);
	}
	fclose(out);
}
