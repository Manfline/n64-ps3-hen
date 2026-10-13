/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — filtros CRT originales del autor, portados de sus versiones de
 * RetroMac. Ver crt_params.h.
 */
#include "crt_params.h"

#include <math.h>
#include <string.h>

/* Preset v1.3 (Shadertoy/crt_mascara_sombra_v1.3.glsl; «monitor» en RetroMac). */
const struct crt_mask_preset crt_mask_monitor = {
	.triads_per_pixel = 1.50f,
	.dot_size = 0.80f,
	.mask_strength = 1.00f,
	.keep_bright = 0.65f,
	.beam_min = 0.22f,
	.beam_max = 0.62f,
	.h_blur = 0.40f,
	.glow = 0.12f,
	.glow_radius = 2.0f,
	.brightness = 1.15f,
	.saturation = 1.05f,
	.black_level = 0.00f,
	.gamma_in = 2.4f,
	.gamma_out = 2.2f,
};

/* Preset v1.1 (Shadertoy/crt_cable_v1.1.glsl; «tv13» en RetroMac): consola
 * por cable a una Trinitron chica de consumo. */
const struct crt_cable_preset crt_cable_tv13 = {
	.tube = {
		.triads_per_pixel = 2.0f,
		.stripe_width = 0.75f,
		.stripe_soft = 0.45f,
		.mask_strength = 1.00f,
		.keep_bright = 0.65f,
		.beam_min = 0.20f,
		.beam_max = 0.55f,
		.glow = 0.12f,
		.glow_radius = 2.0f,
		.brightness = 1.15f,
		.saturation = 1.00f,
		.gamma_in = 2.4f,
		.gamma_out = 2.2f,
	},
	.cycles_per_line = 170.667f,
	.line_phase = 0.5f,
	.frame_phase = 0.0f,
	.luma_sigma = 0.45f,
	.chroma_tau = 1.20f,
};

/* «kv29» en RetroMac (FilterPresets.swift): un Sony KV-29 de consumo, el
 * preset final (v6) del Trinitron de Shadertoy sin los alambres
 * amortiguadores. */
const struct crt_trinitron_preset crt_trinitron_kv29 = {
	.tube = {
		.triads_per_pixel = 2.0f,
		.stripe_width = 0.75f,
		.stripe_soft = 0.45f,
		.mask_strength = 1.00f,
		.keep_bright = 0.65f,
		.beam_min = 0.22f,
		.beam_max = 0.62f,
		.glow = 0.15f,
		.glow_radius = 2.0f,
		.brightness = 1.10f,
		.saturation = 1.05f,
		.gamma_in = 2.4f,
		.gamma_out = 2.2f,
	},
	.h_blur = 0.40f,
};

const float crt_glow_spiral[12][2] = {
	{ 0.20412414523193154f, 0.0f },
	{ -0.2606984956313073f, 0.23882272583565667f },
	{ 0.039901264240885144f, -0.45468804959588777f },
	{ 0.32859869344219084f, 0.4285902067648674f },
	{ -0.6030127734008008f, -0.10665643494639425f },
	{ 0.5712191669786738f, -0.36337583382707683f },
	{ -0.19104982190671727f, 0.7107507525258614f },
	{ -0.3643948584312288f, -0.7015813474921385f },
	{ 0.7905641319543769f, 0.28868960251549736f },
	{ -0.8224326176737932f, 0.33951620882205175f },
	{ 0.39644426089886425f, -0.8472496373562832f },
	{ 0.29301563046687257f, 0.9340637952706629f },
};

void crt_fit_rect(uint32_t out_w, uint32_t out_h, float aspect, float px_aspect, uint32_t r[4])
{
	/* La forma en píxeles de la salida (en 480 y 576 no son cuadrados). */
	float a = px_aspect > 0.0f ? aspect / px_aspect : aspect;
	float w, h;
	if ((float)out_w / (float)out_h > a) {
		w = out_h * a;
		h = (float)out_h;
	} else {
		w = (float)out_w;
		h = out_w / a;
	}
	w = floorf(w);
	h = floorf(h);
	r[0] = (uint32_t)floorf((out_w - w) * 0.5f);
	r[1] = (uint32_t)floorf((out_h - h) * 0.5f);
	r[2] = (uint32_t)w;
	r[3] = (uint32_t)h;
}

uint32_t crt_glow_scale(uint32_t img_w, uint32_t src_w)
{
	uint32_t g = src_w ? img_w / src_w : 1;
	if (g > 2)
		g = 2;
	if (g < 1 || src_w * g > 640)
		g = 1;
	return g;
}

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : x > hi ? hi : x;
}

static float mixf(float a, float b, float t)
{
	return a + (b - a) * t;
}

float crt_dot_cover_sum(float r)
{
	int n = (int)ceilf(r + 1.0f);
	float s = 0.0f;
	for (int b = 0; b < 2 * n && b < 32; b++)
		for (int a = 0; a < 2 * n && a < 32; a++) {
			float qx = (float)(a - n) + 0.5f, qy = (float)(b - n) + 0.5f;
			s += clampf(r - sqrtf(qx * qx + qy * qy) + 0.5f, 0.0f, 1.0f);
		}
	return s > 1e-3f ? s : 1e-3f;
}

void crt_mask_tile_cell(const struct crt_mask_layout *l, float tx, float ty, int ch, float *col, float *row,
			float *cover)
{
	float qx = tx + 0.5f, qy = ty + 0.5f;
	float r0 = floorf(qy / l->h), r1 = r0 + 1.0f;
	float b0 = ch - 2.0f * r0, b1 = ch - 2.0f * r1;
	float i0 = b0 + 3.0f * floorf((qx / l->d - 0.5f * r0 - b0) / 3.0f + 0.5f);
	float i1 = b1 + 3.0f * floorf((qx / l->d - 0.5f * r1 - b1) / 3.0f + 0.5f);
	float x0 = qx - (i0 + 0.5f * r0) * l->d, y0 = qy - r0 * l->h;
	float x1 = qx - (i1 + 0.5f * r1) * l->d, y1 = qy - r1 * l->h;
	float e0 = x0 * x0 + y0 * y0, e1 = x1 * x1 + y1 * y1;
	float i = e1 < e0 ? i1 : i0, r = e1 < e0 ? r1 : r0;
	*col = i + floorf(r * 0.5f);
	*row = r;
	*cover = clampf(l->r - sqrtf(e0 < e1 ? e0 : e1) + 0.5f, 0.0f, 1.0f);
}

void crt_mask_layout(const struct crt_mask_preset *p, uint32_t img_w, uint32_t img_h, uint32_t src_w, uint32_t src_h,
		     struct crt_mask_layout *l)
{
	memset(l, 0, sizeof *l);
	float px_per_src = (float)img_w / (float)src_w;
	/* Red de puntos en píxeles enteros: D par (media fila = píxel entero) y
	 * H entero. Mínimo 4 px: con 2 px no cabe un punto redondo con hueco
	 * negro alrededor. */
	float d = floorf(px_per_src / (3.0f * p->triads_per_pixel) * 0.5f + 0.5f);
	l->d = 2.0f * (d > 2.0f ? d : 2.0f);
	float h = floorf(l->d * 0.8660254f + 0.5f);
	l->h = h > 1.0f ? h : 1.0f;
	l->r = 0.5f * p->dot_size * l->d;
	l->avg = crt_dot_cover_sum(l->r) / (3.0f * l->d * l->h);
	float cover_max = clampf(l->r + 0.5f - 0.7071068f, 0.0f, 1.0f);
	l->peak = mixf(1.0f, cover_max / l->avg, p->mask_strength);
	l->to_src[0] = (float)src_w / (float)img_w;
	l->to_src[1] = (float)src_h / (float)img_h;
	l->tile_w = (uint32_t)(3.0f * l->d);
	l->tile_h = (uint32_t)(2.0f * l->h);
	/* En una tesela, el punto más cercano de cada color está en las filas
	 * 0 a 2 y en las columnas −2 a 4 de la tabla (relativas a la tesela). */
	l->mx = 2;
	l->my = 0;
	l->dots_w = (uint32_t)ceilf(img_w / l->d) + 8;
	l->dots_h = (uint32_t)ceilf(img_h / l->h) + 3;
}

void crt_grille_at(const struct crt_tube_preset *p, float x, float pitch, float out[3])
{
	float ph = x / pitch - floorf(x / pitch);
	float hw = p->stripe_width / 6.0f;
	float e0 = hw * (1.0f - p->stripe_soft), e1 = hw;
	for (int c = 0; c < 3; c++) {
		float d = fabsf(ph - (1.0f + 2.0f * c) / 6.0f);
		d = d < 1.0f - d ? d : 1.0f - d;
		float t = clampf((d - e0) / (e1 - e0), 0.0f, 1.0f);
		out[c] = 1.0f - t * t * (3.0f - 2.0f * t);
	}
}

void crt_grille(const struct crt_tube_preset *p, float px_per_src, struct crt_grille *g)
{
	float k = floorf(px_per_src / (3.0f * p->triads_per_pixel) + 0.5f);
	g->pitch = 3.0f * (k > 1.0f ? k : 1.0f);
	float s[3] = { 0.0f, 0.0f, 0.0f };
	for (int i = 0; i < 96 && (float)i < g->pitch; i++) {
		float v[3];
		crt_grille_at(p, (float)i + 0.5f, g->pitch, v);
		for (int c = 0; c < 3; c++)
			s[c] += v[c];
	}
	for (int c = 0; c < 3; c++) {
		float a = s[c] / g->pitch;
		g->avg[c] = a > 1e-3f ? a : 1e-3f;
		g->peak[c] = mixf(1.0f, 1.0f / g->avg[c], p->mask_strength);
	}
}

void crt_cable_taps(const struct crt_cable_preset *p, enum crt_cable cable, uint32_t src_w,
		    struct crt_cable_taps *t)
{
	memset(t, 0, sizeof *t);
	double f = p->cycles_per_line / (double)src_w;  /* ciclos de subportadora por píxel */
	double dt = 0.25 / f;                           /* 4 muestras por ciclo */
	double scale = src_w / 256.0;                   /* filtros en tiempo de señal */
	double ls = p->luma_sigma * scale, tau = p->chroma_tau * scale;
	/* Una tele barata retrasa la luma solo hasta el pico del filtro del
	 * color (tau), no hasta su promedio (2 tau): el color arranca en el
	 * píxel y deja la estela a la derecha. */
	double lead = tau + 1.5 * dt, ek = exp(dt / tau);
	int jlo, jhi;
	if (cable == CRT_CABLE_RGB) {
		int jr = (int)ceil(3.0 * ls / dt);
		jlo = -jr;
		jhi = jr;
	} else {
		double lo = -3.0 * ls < lead - 7.0 * tau - 3.0 * dt ? -3.0 * ls : lead - 7.0 * tau - 3.0 * dt;
		double hi = 3.0 * ls > lead ? 3.0 * ls : lead;
		jlo = (int)floor(lo / dt);
		jhi = (int)ceil(hi / dt);
	}
	if (jhi - jlo + 1 > CRT_TAPS_MAX)
		jhi = jlo + CRT_TAPS_MAX - 1;
	double wl[CRT_TAPS_MAX], wc[CRT_TAPS_MAX], sl = 0.0, sc = 0.0;
	for (int j = jlo; j <= jhi; j++) {
		double dx = j * dt, l = 0.0, c = 0.0;
		if (cable == CRT_CABLE_RGB) {
			l = exp(-0.5 * dx * dx / (ls * ls));
		} else {
			/* Color: filtro analógico RC de 2 polos, g(u) = u exp(−u/tau),
			 * promediado sobre un ciclo completo (4 muestras): ceros en la
			 * subportadora y su doble, como el pasa banda de la tele. */
			double u = lead - dx;
			if (fabs(dx) <= 3.0 * ls)
				l = exp(-0.5 * dx * dx / (ls * ls));
			if (u >= 0.0 && u <= 7.0 * tau + 3.0 * dt) {
				double m = 1.0;
				for (int k = 0; k < 4; k++) {
					double v = u - k * dt;
					if (v >= 0.0 && v <= 7.0 * tau)
						c += v * m;
					m *= ek;
				}
				c *= exp(-u / tau);
			}
		}
		wl[j - jlo] = l;
		wc[j - jlo] = c;
		sl += l;
		sc += c;
	}
	if (sl < 1e-6)
		sl = 1e-6;
	if (sc < 1e-6)
		sc = 1e-6;
	t->f = (float)f;
	t->dt = (float)dt;
	t->hold = (float)(dt < 1.0 ? dt : 1.0);
	t->ls = (float)ls;
	t->tau = (float)tau;
	t->jlo = jlo;
	t->count = jhi - jlo + 1;
	t->yiq = cable != CRT_CABLE_RGB;
	t->grid8 = cable == CRT_CABLE_COMPOSITE;
	t->kc = cable == CRT_CABLE_COMPOSITE ? 1.0f : 0.0f;
	for (int i = 0; i < t->count; i++) {
		t->w[i][0] = (float)(wl[i] / sl);
		t->w[i][1] = t->yiq ? (float)(wc[i] / sc) : t->w[i][0];
		t->w[i][2] = t->w[i][1];
	}
	/* Columna 0 de las tablas: la decodificación en x = 0 lee desde la
	 * columna 1; la última hace falta en x = ancho (más una para
	 * interpolar). */
	t->x0 = (float)((jlo - 1) * dt);
	t->cols = (uint32_t)ceil((src_w - t->x0) / dt) + 2 + (uint32_t)jhi;
}
