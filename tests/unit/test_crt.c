/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas de lo que los filtros CRT calculan en la CPU
 * (src/frontend/filters/crt_params.c) y de la tesela de la máscara de
 * sombra: el fragment program crt_mask_tile.fcg guarda, para cada píxel de
 * una tesela de 3D × 2H, el punto más cercano de cada color relativo a la
 * tesela, y la pasada final lo lleva a la tabla de puntos. Aquí la misma
 * cuenta en C se compara con la del filtro original (el punto más cercano
 * buscado en cuatro filas, en cada píxel de la pantalla).
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "frontend/filters/crt_params.h"

/* nearestDot del original (Shadertoy v1.3, cuatro filas): centro del punto
 * de color ch más cercano a q. */
static void ref_nearest(float qx, float qy, float ch, float d, float h, float *cx, float *cy)
{
	float row = floorf(qy / h), best = 1e20f;
	for (int k = -1; k <= 2; k++) {
		float r = row + k;
		float fi = qx / d - 0.5f * r;
		float base = ch - 2.0f * r;
		float i = base + 3.0f * floorf((fi - base) / 3.0f + 0.5f);
		float x = (i + 0.5f * r) * d, y = r * h;
		float d2 = (qx - x) * (qx - x) + (qy - y) * (qy - y);
		if (d2 < best) {
			best = d2;
			*cx = x;
			*cy = y;
		}
	}
}

static void crt_fit(void)
{
	uint32_t r[4];
	crt_fit_rect(1920, 1080, 4.0f / 3.0f, 1.0f, r);
	CHECK_EQ(r[0], 240);
	CHECK_EQ(r[1], 0);
	CHECK_EQ(r[2], 1440);
	CHECK_EQ(r[3], 1080);
	crt_fit_rect(1280, 720, 4.0f / 3.0f, 1.0f, r);
	CHECK_EQ(r[0], 160);
	CHECK_EQ(r[2], 960);
	CHECK_EQ(r[3], 720);
	CHECK_EQ(crt_glow_scale(1440, 320), 2);
	CHECK_EQ(crt_glow_scale(960, 640), 1);
	CHECK_EQ(crt_glow_scale(1440, 640), 1);
}

/* La tesela da en cada píxel de la pantalla el mismo punto y la misma
 * cobertura que el original, y la celda cae dentro de la tabla de puntos
 * con el color que la pasada de puntos le asigna. */
static void crt_mask_tile_matches(void)
{
	static const uint32_t sizes[][4] = { { 1440, 1080, 320, 240 }, { 960, 720, 304, 228 }, { 1440, 1080, 640, 480 },
					     { 3840, 2880, 160, 120 } };
	for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
		struct crt_mask_layout l;
		crt_mask_layout(&crt_mask_monitor, sizes[s][0], sizes[s][1], sizes[s][2], sizes[s][3], &l);
		CHECK_EQ(l.tile_w, (uint32_t)(3 * l.d));
		CHECK_EQ(l.tile_h, (uint32_t)(2 * l.h));
		uint32_t bad = 0, out = 0, wrong_color = 0;
		double sum[3] = { 0, 0, 0 };
		for (uint32_t y = 0; y < sizes[s][1]; y += (s == 3 ? 3 : 1))
			for (uint32_t x = 0; x < sizes[s][0]; x += (s == 3 ? 3 : 1)) {
				float qx = x + 0.5f, qy = y + 0.5f;
				uint32_t tx = (uint32_t)floorf(qx / (3.0f * l.d)), ty = (uint32_t)floorf(qy / (2.0f * l.h));
				float lx = qx - tx * 3.0f * l.d - 0.5f, ly = qy - ty * 2.0f * l.h - 0.5f;
				for (int ch = 0; ch < 3; ch++) {
					float rx = 0, ry = 0, col, row, cover;
					ref_nearest(qx, qy, (float)ch, l.d, l.h, &rx, &ry);
					crt_mask_tile_cell(&l, lx, ly, ch, &col, &row, &cover);
					float c = col + 3.0f * tx, r = row + 2.0f * ty;
					float cx = (c + 0.5f * (fmodf(r, 2.0f))) * l.d, cy = r * l.h;
					if (fabsf(cx - rx) > 1e-3f || fabsf(cy - ry) > 1e-3f)
						bad++;
					float d = sqrtf((qx - rx) * (qx - rx) + (qy - ry) * (qy - ry));
					float ref_cover = fminf(fmaxf(l.r - d + 0.5f, 0.0f), 1.0f);
					if (fabsf(ref_cover - cover) > 1e-4f)
						bad++;
					if (c + l.mx < 0 || c + l.mx >= l.dots_w || r + l.my < 0 || r + l.my >= l.dots_h)
						out++;
					/* crt_mask_dots.fcg: el color de la celda (c, r). */
					float m = c - floorf(r * 0.5f) + 2.0f * r;
					float cc = m - 3.0f * floorf((m + 0.5f) / 3.0f);
					if (cc != (float)ch)
						wrong_color++;
					if (x < 3 * l.d * (sizes[s][0] / (3 * (uint32_t)l.d)) &&
					    y < 2 * l.h * (sizes[s][1] / (2 * (uint32_t)l.h)))
						sum[ch] += cover;
				}
			}
		CHECK_EQ(bad, 0);
		CHECK_EQ(out, 0);
		CHECK_EQ(wrong_color, 0);
		if (s < 3) {
			/* El promedio real de la máscara (el que conserva el brillo). */
			double tiles_px = (double)(3 * l.d * (sizes[s][0] / (3 * (uint32_t)l.d))) *
					  (2 * l.h * (sizes[s][1] / (2 * (uint32_t)l.h)));
			for (int ch = 0; ch < 3; ch++)
				CHECK(fabs(sum[ch] / tiles_px - l.avg) < 1e-3);
		}
	}
}

static void crt_mask_grid(void)
{
	struct crt_mask_layout l;
	crt_mask_layout(&crt_mask_monitor, 1440, 1080, 320, 240, &l);
	CHECK_EQ((uint32_t)l.d, 4);
	CHECK_EQ((uint32_t)l.h, 3);
	CHECK(fabsf(l.r - 1.6f) < 1e-6f);
	CHECK(l.peak > 1.0f);
	CHECK(fabsf(l.to_src[0] - 320.0f / 1440.0f) < 1e-6f);
	/* Mínimo 4 px entre puntos; con muchos más píxeles por píxel del
	 * juego, más separados (24 px por píxel: 1,5 tríadas → 6 px). */
	crt_mask_layout(&crt_mask_monitor, 2880, 2160, 320, 240, &l);
	CHECK_EQ((uint32_t)l.d, 4);
	crt_mask_layout(&crt_mask_monitor, 7680, 5760, 320, 240, &l);
	CHECK_EQ((uint32_t)l.d, 6);
	CHECK_EQ((uint32_t)l.h, 5);
}

static void crt_cable_weights(void)
{
	for (int cable = CRT_CABLE_RGB; cable <= CRT_CABLE_COMPOSITE; cable++)
		for (uint32_t w = 256; w <= 640; w += 64) {
			struct crt_cable_taps t;
			crt_cable_taps(&crt_cable_tv13, (enum crt_cable)cable, w, &t);
			CHECK_EQ(t.count, cable == CRT_CABLE_RGB ? 9 : 27);
			double s[3] = { 0, 0, 0 };
			for (int j = 0; j < t.count; j++)
				for (int c = 0; c < 3; c++)
					s[c] += t.w[j][c];
			for (int c = 0; c < 3; c++)
				CHECK(fabs(s[c] - 1.0) < 1e-5);
			CHECK(fabsf(t.dt - 0.25f * w / 170.667f) < 1e-4f);
			CHECK(fabsf(t.x0 - (t.jlo - 1) * t.dt) < 1e-4f);
			/* La decodificación en x = ancho (más una para interpolar) lee
			 * muestras que la tabla tiene. */
			float u = (w - t.x0) / t.dt;
			CHECK((uint32_t)floorf(u) + 1 + (uint32_t)(t.jlo + t.count - 1) < t.cols);
			CHECK(t.cols <= 1280);  /* cabe en la profundidad de la pantalla a 720p */
			CHECK_EQ(t.grid8, cable == CRT_CABLE_COMPOSITE);
		}
}

static void crt_grille_avg(void)
{
	struct crt_grille g;
	crt_grille(&crt_cable_tv13.tube, 1440.0f / 320.0f, &g);
	CHECK_EQ((uint32_t)g.pitch, 3);
	for (int c = 0; c < 3; c++) {
		CHECK(g.avg[c] > 0.1f && g.avg[c] <= 1.0f);
		CHECK(fabsf(g.peak[c] - 1.0f / g.avg[c]) < 1e-5f);
	}
}

void t_suite_crt(void)
{
	RUN(crt_fit);
	RUN(crt_mask_grid);
	RUN(crt_mask_tile_matches);
	RUN(crt_cable_weights);
	RUN(crt_grille_avg);
}
