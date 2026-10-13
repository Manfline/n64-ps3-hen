/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — filtros de la presentación en el RSX. Ver filters.h y
 * docs/filtros.md.
 *
 * Filtros CRT originales del autor, portados de sus versiones de RetroMac
 * (Shaders.metal), que a su vez reparten en pasadas la cuenta de sus
 * shaders de Shadertoy: lo que no cambia de un píxel de pantalla al
 * vecino se calcula antes, una vez, en una tabla. Aquí, además:
 * - la imagen se reduce primero a la resolución del juego (filtro de caja
 *   sobre la escala interna) y se pasa a luz lineal una vez;
 * - el resplandor sale de una rejilla de 1 o 2 puntos por píxel del juego
 *   (como la rejilla del resplandor de RetroMac) y lee la luz lineal con
 *   filtro bilineal (el original interpola los valores codificados);
 * - la máscara de sombra guarda en teselas lo que depende solo de la
 *   posición en pantalla (el punto más cercano de cada color y cuánto
 *   cubre al píxel), y en la tabla de puntos el color que da cada punto
 *   como A·cobertura + B;
 * - el cable decodifica la señal en muestras fijas de cada línea (4 por
 *   ciclo de subportadora; el compuesto en dos tablas corridas medio paso)
 *   y la pasada de líneas las interpola: el original la decodifica en cada
 *   columna de pantalla.
 * El RSX no escribe dos salidas desde un programa de cgcomp: la tabla de
 * líneas del tubo va en dos pasadas.
 */
#include "frontend/filters/filters.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <rsxkit/post.h>

#include "frontend/filters/crt_params.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"

#include "crt_cable_decode27_fpo.h"
#include "crt_cable_decode9_fpo.h"
#include "crt_cable_signal_fpo.h"
#include "crt_debug_fpo.h"
#include "crt_glow_fpo.h"
#include "crt_lineal_fpo.h"
#include "crt_mask_dots_fpo.h"
#include "crt_mask_final_fpo.h"
#include "crt_mask_tile_fpo.h"
#include "crt_tube_final_fpo.h"
#include "crt_tube_lines_a4_fpo.h"
#include "crt_tube_lines_a8_fpo.h"
#include "crt_tube_lines_b4_fpo.h"
#include "crt_tube_lines_b8_fpo.h"

/* Tramos medidos de cada cuadro (marcas de tiempo del RSX entre pasadas). */
enum { SEG_REDUCE, SEG_LINEAR, SEG_TABLES, SEG_LINES, SEG_FINAL, SEGS };
static const char *const seg_mask[SEGS] = { "reducir", "lineal y resplandor", "teselas y puntos", NULL, "final" };
static const char *const seg_cable[SEGS] = { "reducir", "lineal y resplandor", "señal y decodificación", "líneas",
					     "final" };
/* Cuadros en vuelo cuyas marcas se leen cuando su barrera pasó. */
#define SLOTS 4

enum {
	P_LINEAR, P_GLOW, P_TILE_A, P_TILE_B, P_TILE_C, P_DOTS, P_MASK_FINAL,
	P_SIGNAL_E, P_SIGNAL_O, P_DECODE9, P_DECODE27, P_LINES_A4, P_LINES_B4, P_LINES_A8, P_LINES_B8, P_TUBE_FINAL,
	P_DEBUG, PROGRAMS
};

static const u8 *const program_fpo[PROGRAMS] = {
	[P_LINEAR] = crt_lineal_fpo,
	[P_GLOW] = crt_glow_fpo,
	[P_TILE_A] = crt_mask_tile_fpo,
	[P_TILE_B] = crt_mask_tile_fpo,
	[P_TILE_C] = crt_mask_tile_fpo,
	[P_DOTS] = crt_mask_dots_fpo,
	[P_MASK_FINAL] = crt_mask_final_fpo,
	[P_SIGNAL_E] = crt_cable_signal_fpo,
	[P_SIGNAL_O] = crt_cable_signal_fpo,
	[P_DECODE9] = crt_cable_decode9_fpo,
	[P_DECODE27] = crt_cable_decode27_fpo,
	[P_LINES_A4] = crt_tube_lines_a4_fpo,
	[P_LINES_B4] = crt_tube_lines_b4_fpo,
	[P_LINES_A8] = crt_tube_lines_a8_fpo,
	[P_LINES_B8] = crt_tube_lines_b8_fpo,
	[P_TUBE_FINAL] = crt_tube_final_fpo,
	[P_DEBUG] = crt_debug_fpo,
};

enum { S_SRC, S_LIN, S_GLOW, S_TILE_A, S_TILE_B, S_TILE_C, S_DOTS, S_SIG_E, S_SIG_O, S_DEC_E, S_DEC_O, S_LINES_A,
       S_LINES_B, SURFACES };

static struct {
	bool failed;            /* no hay memoria o programas: sin filtro hasta liberar */
	rsxkit_program *prog[PROGRAMS];
	rsxkit_surface *surf[SURFACES];
	/* Configuración de las tablas y constantes. */
	int filter;
	uint32_t sw, sh;        /* imagen del juego */
	uint32_t ow, oh;        /* pantalla */
	float aspect;
	uint32_t rect[4];       /* imagen en pantalla */
	uint32_t g;             /* rejilla del resplandor */
	struct crt_mask_layout ml;
	struct crt_cable_taps ct;
	struct crt_grille gr;
	/* Medición. */
	struct {
		uint32_t fence;
		int filter;
		bool pending;
	} slot[SLOTS];
	uint32_t frame;
	uint64_t ns[SEGS];
	uint32_t measured, drawn;
	int last_filter;
	bool check;             /* falta la comprobación de esta configuración */
} fl = { .filter = -1, .last_filter = -1 };

static rsxkit_post_input in_surface(int s, rsxkit_filter filter, uint8_t wrap)
{
	return rsxkit_post_surface_input(fl.surf[s], filter, wrap);
}

static void set4(int p, const char *name, float x, float y, float z, float w)
{
	const float v[4] = { x, y, z, w };
	rsxkit_program_set(fl.prog[p], name, v, 1);
}

static void free_surfaces(void)
{
	for (int i = 0; i < SURFACES; i++) {
		rsxkit_surface_destroy(fl.surf[i]);
		fl.surf[i] = NULL;
	}
}

void salsa64_filters_release(void)
{
	rsxkit_finish();
	free_surfaces();
	for (int i = 0; i < PROGRAMS; i++) {
		rsxkit_program_destroy(fl.prog[i]);
		fl.prog[i] = NULL;
	}
	memset(fl.slot, 0, sizeof fl.slot);
	fl.filter = -1;
	fl.failed = false;
}

static bool make_surface(int s, uint32_t w, uint32_t h, rsxkit_post_format f)
{
	fl.surf[s] = rsxkit_surface_create(w, h, f);
	if (!fl.surf[s])
		salsa64_log("filtros: no hay memoria del RSX para una tabla de %ux%u (%s)", w, h,
			    f == RSXKIT_POST_RGBA8 ? "RGBA8" : "RGBA16F");
	return fl.surf[s] != NULL;
}

static bool is_cable(int f)
{
	return f == SALSA64_FILTER_CRT_RGB || f == SALSA64_FILTER_CRT_SVIDEO || f == SALSA64_FILTER_CRT_COMPOSITE;
}

/* Las constantes de la máscara de sombra. */
static void mask_constants(void)
{
	const struct crt_mask_preset *p = &crt_mask_monitor;
	const struct crt_mask_layout *l = &fl.ml;
	float k = p->brightness * (1.0f - p->black_level);
	set4(P_TILE_A, "grid", l->d, l->h, l->r, 0.0f);
	set4(P_TILE_A, "which", 1.0f, 0.0f, 0.0f, 0.0f);
	set4(P_TILE_B, "grid", l->d, l->h, l->r, 0.0f);
	set4(P_TILE_B, "which", 0.0f, 1.0f, 0.0f, 0.0f);
	set4(P_TILE_C, "grid", l->d, l->h, l->r, 0.0f);
	set4(P_TILE_C, "which", 0.0f, 0.0f, 1.0f, 0.0f);
	set4(P_DOTS, "g1", l->d, l->h, (float)l->mx, (float)l->my);
	set4(P_DOTS, "g2", l->to_src[0], l->to_src[1], 0.5f * l->h * l->to_src[1],
	     -1.442695f * 0.5f / (p->h_blur * p->h_blur));
	set4(P_DOTS, "g3", p->beam_min, p->beam_max, p->saturation, 0.0f);
	float pk1 = l->peak - 1.0f > 1e-4f ? l->peak - 1.0f : 1e-4f;
	set4(P_DOTS, "o1", l->peak, 1.0f / pk1, p->keep_bright, p->mask_strength / l->avg * k);
	set4(P_DOTS, "o2", (1.0f - p->mask_strength) * k, k, 0.0f, 0.0f);
	set4(P_MASK_FINAL, "t1", 1.0f / (3.0f * l->d), 1.0f / (2.0f * l->h), 3.0f * l->d, 2.0f * l->h);
	set4(P_MASK_FINAL, "t2", l->mx + 0.5f, l->my + 0.5f, l->to_src[0] * fl.g, l->to_src[1] * fl.g);
	set4(P_MASK_FINAL, "t3", p->black_level, 1.0f / p->gamma_out, 0.0f, 0.0f);
}

/* El resplandor y la luz lineal, comunes a los dos CRT. */
static void glow_constants(float gamma_in, float glow, float radius, float scale)
{
	float sp[6][4];
	for (int i = 0; i < 6; i++) {
		sp[i][0] = crt_glow_spiral[2 * i][0] * radius;
		sp[i][1] = crt_glow_spiral[2 * i][1] * radius;
		sp[i][2] = crt_glow_spiral[2 * i + 1][0] * radius;
		sp[i][3] = crt_glow_spiral[2 * i + 1][1] * radius;
	}
	rsxkit_program_set(fl.prog[P_GLOW], "sp", &sp[0][0], 6);
	set4(P_GLOW, "k", glow * scale / 12.0f, 0.0f, 0.0f, 0.0f);
	set4(P_LINEAR, "k", gamma_in, 0.0f, 0.0f, 0.0f);
}

/* Las constantes del CRT con cable. */
static void cable_constants(int f)
{
	const struct crt_cable_preset *p = &crt_cable_tv13;
	const struct crt_tube_preset *t = &p->tube;
	const struct crt_cable_taps *c = &fl.ct;
	float to_x = (float)fl.sw / fl.rect[2], to_y = (float)fl.sh / fl.rect[3];
	for (int s = 0; s < 2; s++) {
		int pi = s ? P_SIGNAL_O : P_SIGNAL_E;
		set4(pi, "s1", c->x0 + (s ? 0.5f * c->dt : 0.0f), c->dt, c->hold, 1.0f / c->hold);
		set4(pi, "s2", c->f, p->line_phase, 0.0f, c->kc);
		set4(pi, "s3", c->yiq ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	}
	int dec = c->count > 9 ? P_DECODE27 : P_DECODE9;
	int taps = c->count > 9 ? 27 : 9;
	float we[16][4], wo[16][4], wz[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	memset(we, 0, sizeof we);
	memset(wo, 0, sizeof wo);
	for (int j = 0; j < taps && j < c->count; j++) {
		float *w = (j & 1) ? wo[j / 2] : we[j / 2];
		if ((taps & 1) && j == taps - 1)
			w = wz;
		w[0] = c->w[j][0];
		w[1] = c->w[j][1];
		w[2] = c->w[j][2];
	}
	rsxkit_program_set(fl.prog[dec], "we", &we[0][0], (uint32_t)taps / 2);
	rsxkit_program_set(fl.prog[dec], "wo", &wo[0][0], (uint32_t)taps / 2);
	rsxkit_program_set(fl.prog[dec], "wz", wz, 1);
	set4(dec, "k0", c->jlo + 0.5f, t->gamma_in, t->saturation, 0.0f);
	if (c->yiq) {
		set4(dec, "m0", 1.0f, 0.956f, 0.621f, 0.0f);
		set4(dec, "m1", 1.0f, -0.272f, -0.647f, 0.0f);
		set4(dec, "m2", 1.0f, -1.106f, 1.703f, 0.0f);
	} else {
		set4(dec, "m0", 1.0f, 0.0f, 0.0f, 0.0f);
		set4(dec, "m1", 0.0f, 1.0f, 0.0f, 0.0f);
		set4(dec, "m2", 0.0f, 0.0f, 1.0f, 0.0f);
	}
	int la = c->grid8 ? P_LINES_A8 : P_LINES_A4, lb = c->grid8 ? P_LINES_B8 : P_LINES_B4;
	float hw = t->stripe_width / 6.0f;
	float pk[3], ipk[3], iavg[3];
	for (int i = 0; i < 3; i++) {
		pk[i] = fl.gr.peak[i];
		ipk[i] = 1.0f / (pk[i] - 1.0f > 1e-4f ? pk[i] - 1.0f : 1e-4f);
		iavg[i] = 1.0f / fl.gr.avg[i];
	}
	for (int k = 0; k < 2; k++) {
		int pi = k ? lb : la;
		set4(pi, "q1", to_x, c->x0, 1.0f / c->dt, 0.0f);
		set4(pi, "q2", t->beam_min, t->beam_max, t->keep_bright, t->brightness);
		set4(pi, "q3", 1.0f / fl.gr.pitch, t->mask_strength, hw * (1.0f - t->stripe_soft), 1.0f / (hw * t->stripe_soft));
		set4(pi, "q4", iavg[0], iavg[1], iavg[2], 0.0f);
		set4(pi, "q5", pk[0], pk[1], pk[2], 0.0f);
		set4(pi, "q6", ipk[0], ipk[1], ipk[2], 0.0f);
	}
	set4(P_TUBE_FINAL, "f1", to_x, to_y, 0.0f, 0.0f);
	set4(P_TUBE_FINAL, "f2", to_x * fl.g, to_y * fl.g, 1.0f / t->gamma_out, 0.0f);
	(void)f;
}

/* Prepara tablas y constantes para el filtro f con una imagen de sw × sh
 * en la pantalla de ahora. false si no hay memoria. */
static bool configure(int f, uint32_t sw, uint32_t sh, float aspect)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	if (fl.filter == f && fl.sw == sw && fl.sh == sh && fl.ow == sc->width && fl.oh == sc->height &&
	    fl.aspect == aspect)
		return true;
	rsxkit_finish();
	free_surfaces();
	fl.filter = -1;
	for (int i = 0; i < PROGRAMS; i++)
		if (!fl.prog[i] && !(fl.prog[i] = rsxkit_program_create(program_fpo[i]))) {
			salsa64_log("filtros: no se pudo cargar el programa %d", i);
			return false;
		}
	float px_aspect = sc->aspect * sc->height / sc->width;
	crt_fit_rect(sc->width, sc->height, aspect, px_aspect, fl.rect);
	fl.sw = sw;
	fl.sh = sh;
	fl.ow = sc->width;
	fl.oh = sc->height;
	fl.aspect = aspect;
	fl.g = crt_glow_scale(fl.rect[2], sw);
	bool ok = make_surface(S_SRC, sw, sh, RSXKIT_POST_RGBA8) && make_surface(S_LIN, sw, sh, RSXKIT_POST_RGBA16F) &&
		  make_surface(S_GLOW, sw * fl.g, sh * fl.g, RSXKIT_POST_RGBA16F);
	if (ok && f == SALSA64_FILTER_CRT_MASK) {
		const struct crt_mask_preset *p = &crt_mask_monitor;
		crt_mask_layout(p, fl.rect[2], fl.rect[3], sw, sh, &fl.ml);
		ok = make_surface(S_TILE_A, fl.ml.tile_w, fl.ml.tile_h, RSXKIT_POST_RGBA16F) &&
		     make_surface(S_TILE_B, fl.ml.tile_w, fl.ml.tile_h, RSXKIT_POST_RGBA16F) &&
		     make_surface(S_TILE_C, fl.ml.tile_w, fl.ml.tile_h, RSXKIT_POST_RGBA16F) &&
		     make_surface(S_DOTS, fl.ml.dots_w, fl.ml.dots_h, RSXKIT_POST_RGBA16F);
		if (ok) {
			glow_constants(p->gamma_in, p->glow, p->glow_radius, p->brightness * (1.0f - p->black_level));
			mask_constants();
			salsa64_log("filtros: «%s» en %ux%u (%u,%u) desde %ux%u: puntos cada %.0f px, filas de %.0f, radio "
				    "%.1f, promedio %.3f, pico %.2f; tabla de puntos %ux%u, tesela %ux%u, resplandor ×%u",
				    salsa64_filter_name(f), fl.rect[2], fl.rect[3], fl.rect[0], fl.rect[1], sw, sh, fl.ml.d,
				    fl.ml.h, fl.ml.r, fl.ml.avg, fl.ml.peak, fl.ml.dots_w, fl.ml.dots_h, fl.ml.tile_w,
				    fl.ml.tile_h, fl.g);
		}
	} else if (ok && is_cable(f)) {
		const struct crt_cable_preset *p = &crt_cable_tv13;
		enum crt_cable cable = f == SALSA64_FILTER_CRT_RGB      ? CRT_CABLE_RGB
				       : f == SALSA64_FILTER_CRT_SVIDEO ? CRT_CABLE_SVIDEO
									: CRT_CABLE_COMPOSITE;
		crt_cable_taps(p, cable, sw, &fl.ct);
		crt_grille(&p->tube, (float)fl.rect[2] / sw, &fl.gr);
		ok = make_surface(S_SIG_E, fl.ct.cols, sh, RSXKIT_POST_RGBA16F) &&
		     make_surface(S_DEC_E, fl.ct.cols, sh, RSXKIT_POST_RGBA16F) &&
		     (!fl.ct.grid8 || (make_surface(S_SIG_O, fl.ct.cols, sh, RSXKIT_POST_RGBA16F) &&
				       make_surface(S_DEC_O, fl.ct.cols, sh, RSXKIT_POST_RGBA16F))) &&
		     make_surface(S_LINES_A, fl.rect[2], sh, RSXKIT_POST_RGBA16F) &&
		     make_surface(S_LINES_B, fl.rect[2], sh, RSXKIT_POST_RGBA16F);
		if (ok) {
			glow_constants(p->tube.gamma_in, p->tube.glow, p->tube.glow_radius, p->tube.brightness);
			cable_constants(f);
			salsa64_log("filtros: «%s» en %ux%u (%u,%u) desde %ux%u: %d muestras desde %d cada %.3f px (%u "
				    "columnas%s), franjas cada %.0f px, resplandor ×%u",
				    salsa64_filter_name(f), fl.rect[2], fl.rect[3], fl.rect[0], fl.rect[1], sw, sh,
				    fl.ct.count, fl.ct.jlo, fl.ct.dt, fl.ct.cols, fl.ct.grid8 ? ", dos tablas" : "",
				    fl.gr.pitch, fl.g);
		}
	} else if (ok) {
		ok = false;
	}
	if (!ok) {
		free_surfaces();
		return false;
	}
	salsa64_log("filtros: %.1f MB de memoria del RSX en tablas", rsxkit_surface_bytes() / 1048576.0);
	fl.filter = f;
	fl.check = true;
	return true;
}

/* ---- Comprobación en la consola ------------------------------------------------
 * Una vez por configuración, después del primer cuadro: se leen desde el
 * PPE las tablas que dejó el RSX y se comparan con la misma cuenta en la
 * CPU. Dice en el log si las superficies fp16, su filtro bilineal, el modo
 * BORDER (0 fuera) y el orden de los canales hacen lo que se espera en la
 * PS3 real (RPCS3 no es prueba de eso). Cuesta unas decenas de ms. */

static float half_to_float(uint16_t h)
{
	uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023;
	uint32_t bits;
	if (!e)
		return (h >> 15 ? -1.0f : 1.0f) * m * (1.0f / 16777216.0f);
	if (e == 31)
		bits = s | 0x7F800000u | (m << 13);
	else
		bits = s | ((e + 112) << 23) | (m << 13);
	float f;
	memcpy(&f, &bits, sizeof f);
	return f;
}

/* Un texel fp16 (R, G, B, A) de la superficie s; 0 fuera. */
static void texel16(int s, int x, int y, float out[4])
{
	uint32_t pitch, w, h;
	const uint8_t *p = rsxkit_surface_data(fl.surf[s], &pitch);
	rsxkit_surface_size(fl.surf[s], &w, &h);
	if (!p || x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
	const volatile uint16_t *t = (const volatile uint16_t *)(p + (uint32_t)y * pitch + (uint32_t)x * 8);
	for (int c = 0; c < 4; c++)
		out[c] = half_to_float(t[c]);
}

/* Una vez por filtro y por alto de la salida en cada corrida: la imagen que
 * entra al filtro (la reducida) y lo que dibujó en la pantalla, para
 * compararlos en la Mac con el filtro original (tools/crt-ref.c). En
 * SALSA64_DATA_DIR/filtro-<clave>-<alto>-fuente.ppm y -pantalla.ppm. */
static void dump_once(void)
{
	static uint32_t done[SALSA64_FILTERS][2];
	uint32_t *d = &done[fl.filter][fl.oh >= 1000];
	if (*d)
		return;
	*d = 1;
	char path[96];
	uint32_t ps, pitch;
	const volatile uint8_t *S = rsxkit_surface_data(fl.surf[S_SRC], &ps);
	snprintf(path, sizeof path, SALSA64_DATA_DIR "/filtro-%s-%u-fuente.ppm", salsa64_filter_key(fl.filter), fl.oh);
	FILE *f = fopen(path, "wb");
	if (f && S) {
		static uint8_t line[3 * 640];
		fprintf(f, "P6\n%u %u\n255\n", fl.sw, fl.sh);
		for (uint32_t y = 0; y < fl.sh; y++) {
			for (uint32_t x = 0; x < fl.sw; x++)
				for (int c = 0; c < 3; c++)
					line[3 * x + c] = S[y * ps + x * 4 + 1 + c];
			fwrite(line, 3, fl.sw, f);
		}
	}
	if (f)
		fclose(f);
	const uint32_t *px = rsxkit_back_buffer(&pitch);
	snprintf(path, sizeof path, SALSA64_DATA_DIR "/filtro-%s-%u-pantalla.ppm", salsa64_filter_key(fl.filter), fl.oh);
	f = fopen(path, "wb");
	if (f && px) {
		static uint8_t line[3 * 1920];
		fprintf(f, "P6\n%u %u\n255\n", fl.ow, fl.oh);
		for (uint32_t y = 0; y < fl.oh; y++) {
			const volatile uint32_t *row = px + y * (pitch / 4);
			for (uint32_t x = 0; x < fl.ow && x < 1920; x++) {
				uint32_t c = row[x];
				line[3 * x] = (uint8_t)(c >> 16);
				line[3 * x + 1] = (uint8_t)(c >> 8);
				line[3 * x + 2] = (uint8_t)c;
			}
			fwrite(line, 3, fl.ow < 1920 ? fl.ow : 1920, f);
		}
	}
	if (f)
		fclose(f);
	salsa64_log("filtros: entrada y pantalla de «%s» en filtro-%s-%u-fuente.ppm y -pantalla.ppm",
		    salsa64_filter_name(fl.filter), salsa64_filter_key(fl.filter), fl.oh);
}

static void check_tables(void)
{
	fl.check = false;
	rsxkit_finish();
	uint32_t ps;
	const volatile uint8_t *S = rsxkit_surface_data(fl.surf[S_SRC], &ps);
	float gin = fl.filter == SALSA64_FILTER_CRT_MASK ? crt_mask_monitor.gamma_in : crt_cable_tv13.tube.gamma_in;
	/* La luz lineal contra la fuente (uno de cada 3 × 3 texeles). */
	float lin_err = 0.0f, lin_swap = 0.0f;
	for (uint32_t y = 1; y < fl.sh; y += 3)
		for (uint32_t x = 1; x < fl.sw; x += 3) {
			float l[4];
			texel16(S_LIN, (int)x, (int)y, l);
			for (int c = 0; c < 3; c++) {
				float sv = S[y * ps + x * 4 + 1 + c] / 255.0f;
				float e = fabsf(l[c] - powf(sv, gin));
				float sw = fabsf(l[2 - c] - powf(sv, gin));
				lin_err = e > lin_err ? e : lin_err;
				lin_swap = sw > lin_swap ? sw : lin_swap;
			}
		}
	/* El resplandor contra 12 lecturas bilineales de la luz lineal hechas
	 * aquí (0 fuera de la imagen), en 16 puntos de la rejilla. */
	float glow_err = 0.0f, glow_point = 0.0f;
	const float k = (fl.filter == SALSA64_FILTER_CRT_MASK
				 ? crt_mask_monitor.glow * crt_mask_monitor.brightness * (1.0f - crt_mask_monitor.black_level)
				 : crt_cable_tv13.tube.glow * crt_cable_tv13.tube.brightness) / 12.0f;
	const float radius = fl.filter == SALSA64_FILTER_CRT_MASK ? crt_mask_monitor.glow_radius
								  : crt_cable_tv13.tube.glow_radius;
	for (int i = 0; i < 16; i++) {
		int gx = (int)((fl.sw * fl.g) * (i % 4 * 2 + 1) / 8), gy = (int)((fl.sh * fl.g) * (i / 4 * 2 + 1) / 8);
		float px = (gx + 0.5f) / fl.g, py = (gy + 0.5f) / fl.g;
		float want[3] = { 0, 0, 0 }, near[3] = { 0, 0, 0 }, got[4];
		for (int t = 0; t < 12; t++) {
			float u = px + crt_glow_spiral[t][0] * radius - 0.5f, v = py + crt_glow_spiral[t][1] * radius - 0.5f;
			int x0 = (int)floorf(u), y0 = (int)floorf(v);
			float fx = u - x0, fy = v - y0, a[4], b[4], c[4], d[4], n[4];
			texel16(S_LIN, x0, y0, a);
			texel16(S_LIN, x0 + 1, y0, b);
			texel16(S_LIN, x0, y0 + 1, c);
			texel16(S_LIN, x0 + 1, y0 + 1, d);
			texel16(S_LIN, (int)floorf(u + 0.5f), (int)floorf(v + 0.5f), n);
			for (int ch = 0; ch < 3; ch++) {
				want[ch] += ((a[ch] * (1 - fx) + b[ch] * fx) * (1 - fy) + (c[ch] * (1 - fx) + d[ch] * fx) * fy) * k;
				near[ch] += n[ch] * k;
			}
		}
		texel16(S_GLOW, gx, gy, got);
		for (int ch = 0; ch < 3; ch++) {
			float e = fabsf(got[ch] - want[ch]), en = fabsf(got[ch] - near[ch]);
			glow_err = e > glow_err ? e : glow_err;
			glow_point = en > glow_point ? en : glow_point;
		}
	}
	char extra[160] = "";
	if (fl.filter == SALSA64_FILTER_CRT_MASK) {
		/* Las teselas contra la misma cuenta en la CPU. */
		uint32_t bad = 0;
		for (uint32_t ty = 0; ty < fl.ml.tile_h; ty++)
			for (uint32_t tx = 0; tx < fl.ml.tile_w; tx++) {
				float a[4], b[4], c[4], col[3], row[3], cov[3];
				texel16(S_TILE_A, (int)tx, (int)ty, a);
				texel16(S_TILE_B, (int)tx, (int)ty, b);
				texel16(S_TILE_C, (int)tx, (int)ty, c);
				for (int ch = 0; ch < 3; ch++)
					crt_mask_tile_cell(&fl.ml, (float)tx, (float)ty, ch, &col[ch], &row[ch], &cov[ch]);
				if (a[0] != col[0] || a[1] != row[0] || a[2] != col[1] || a[3] != row[1] || b[0] != col[2] ||
				    b[1] != row[2] || fabsf(c[0] - cov[0]) > 2e-3f || fabsf(c[1] - cov[1]) > 2e-3f ||
				    fabsf(c[2] - cov[2]) > 2e-3f)
					bad++;
			}
		float d[4];
		texel16(S_DOTS, (int)fl.ml.dots_w / 2, (int)fl.ml.dots_h / 2, d);
		snprintf(extra, sizeof extra, "; teselas: %u de %u píxeles distintos de la CPU; punto del centro A %.4f B "
			 "%.4f energía %.4f apertura %.3f", bad, fl.ml.tile_w * fl.ml.tile_h, d[0], d[1], d[2], d[3]);
	} else {
		float a[4], b[4];
		texel16(S_LINES_A, (int)fl.rect[2] / 2, (int)fl.sh / 2, a);
		texel16(S_LINES_B, (int)fl.rect[2] / 2, (int)fl.sh / 2, b);
		snprintf(extra, sizeof extra, "; línea del centro: amplitud %.4f %.4f %.4f, k %.3f %.3f %.3f", a[0], a[1],
			 a[2], a[3], b[0], b[1]);
	}
	salsa64_log("filtros: comprobación en el RSX («%s»): luz lineal error máx. %.4f (%.4f con R y B cambiados); "
		    "resplandor error máx. %.5f contra bilineal (%.5f contra el texel más cercano)%s",
		    salsa64_filter_name(fl.filter), lin_err, lin_swap, glow_err, glow_point, extra);
	dump_once();
}

/* Lee las marcas de los cuadros cuya barrera ya pasó. */
static void collect(void)
{
	for (int s = 0; s < SLOTS; s++) {
		if (!fl.slot[s].pending || !rsxkit_fence_done(fl.slot[s].fence))
			continue;
		fl.slot[s].pending = false;
		if (fl.slot[s].filter != fl.last_filter)
			continue;
		uint64_t t[SEGS + 1];
		for (int i = 0; i <= SEGS; i++)
			t[i] = rsxkit_stamp_ns((uint32_t)(s * (SEGS + 1) + i));
		bool sane = true;
		for (int i = 0; i < SEGS; i++)
			if (t[i + 1] < t[i] || t[i + 1] - t[i] > 100000000u)
				sane = false;
		if (!sane)
			continue;
		for (int i = 0; i < SEGS; i++)
			fl.ns[i] += t[i + 1] - t[i];
		fl.measured++;
	}
}

static void stamp(int slot, int i)
{
	rsxkit_stamp((uint32_t)(slot * (SEGS + 1) + i));
}

static void run(rsxkit_post_pass *p)
{
	int rc = rsxkit_post_run(p);
	if (rc != RSXKIT_OK && !fl.failed) {
		salsa64_log("filtros: una pasada falló (%d)", rc);
		fl.failed = true;
	}
}

static void pass_init(rsxkit_post_pass *p, int prog, int out, uint32_t w, uint32_t h)
{
	memset(p, 0, sizeof *p);
	p->program = fl.prog[prog];
	p->out = out >= 0 ? fl.surf[out] : NULL;
	p->rect = (rsxkit_rect){ 0, 0, w, h };
}

static void draw_mask(int slot)
{
	const struct crt_mask_layout *l = &fl.ml;
	rsxkit_post_pass p;
	static const int tiles[3] = { P_TILE_A, P_TILE_B, P_TILE_C };
	for (int i = 0; i < 3; i++) {
		pass_init(&p, tiles[i], S_TILE_A + i, l->tile_w, l->tile_h);
		run(&p);
	}
	pass_init(&p, P_DOTS, S_DOTS, l->dots_w, l->dots_h);
	p.in[0] = in_surface(S_LIN, RSXKIT_FILTER_NEAREST, RSXKIT_POST_BORDER);
	p.inputs = 1;
	run(&p);
	stamp(slot, SEG_TABLES + 1);
	stamp(slot, SEG_LINES + 1);
	pass_init(&p, P_MASK_FINAL, -1, fl.rect[2], fl.rect[3]);
	p.rect.x = (int32_t)fl.rect[0];
	p.rect.y = (int32_t)fl.rect[1];
	for (int i = 0; i < 3; i++)
		p.in[i] = in_surface(S_TILE_A + i, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
	p.in[3] = in_surface(S_DOTS, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
	p.in[4] = in_surface(S_GLOW, RSXKIT_FILTER_LINEAR, RSXKIT_POST_CLAMP);
	p.inputs = 5;
	run(&p);
}

static void draw_cable(int slot)
{
	const struct crt_cable_taps *c = &fl.ct;
	rsxkit_post_pass p;
	int dec = c->count > 9 ? P_DECODE27 : P_DECODE9;
	for (int s = 0; s < (c->grid8 ? 2 : 1); s++) {
		pass_init(&p, s ? P_SIGNAL_O : P_SIGNAL_E, s ? S_SIG_O : S_SIG_E, c->cols, fl.sh);
		p.in[0] = in_surface(S_SRC, RSXKIT_FILTER_LINEAR, RSXKIT_POST_BORDER);
		p.inputs = 1;
		run(&p);
		pass_init(&p, dec, s ? S_DEC_O : S_DEC_E, c->cols, fl.sh);
		p.in[0] = in_surface(s ? S_SIG_O : S_SIG_E, RSXKIT_FILTER_NEAREST, RSXKIT_POST_BORDER);
		p.inputs = 1;
		run(&p);
	}
	stamp(slot, SEG_TABLES + 1);
	for (int k = 0; k < 2; k++) {
		int prog = c->grid8 ? (k ? P_LINES_B8 : P_LINES_A8) : (k ? P_LINES_B4 : P_LINES_A4);
		pass_init(&p, prog, k ? S_LINES_B : S_LINES_A, fl.rect[2], fl.sh);
		p.in[0] = in_surface(S_DEC_E, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
		p.inputs = 1;
		if (c->grid8) {
			p.in[1] = in_surface(S_DEC_O, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
			p.inputs = 2;
		}
		run(&p);
	}
	stamp(slot, SEG_LINES + 1);
	pass_init(&p, P_TUBE_FINAL, -1, fl.rect[2], fl.rect[3]);
	p.rect.x = (int32_t)fl.rect[0];
	p.rect.y = (int32_t)fl.rect[1];
	p.in[0] = in_surface(S_LINES_A, RSXKIT_FILTER_NEAREST, RSXKIT_POST_BORDER);
	p.in[1] = in_surface(S_LINES_B, RSXKIT_FILTER_NEAREST, RSXKIT_POST_BORDER);
	p.in[2] = in_surface(S_GLOW, RSXKIT_FILTER_LINEAR, RSXKIT_POST_CLAMP);
	p.inputs = 3;
	run(&p);
}

/* Diagnóstico (salsa64_filters_debug): la tabla `s` en la esquina de la
 * pantalla, un texel por píxel, en lugar de la imagen final. */
static int debug_surface = -1;
static float debug_scale = 1.0f;

static const char *const surface_names[SURFACES] = { "fuente", "lineal", "resplandor", "teselaA", "teselaB", "teselaC",
						     "puntos", "senal", "senalO", "decodificada", "decodificadaO",
						     "lineasA", "lineasB" };

void salsa64_filters_debug(const char *name, float scale)
{
	debug_surface = -1;
	for (int i = 0; name && i < SURFACES; i++)
		if (!strcmp(name, surface_names[i]))
			debug_surface = i;
	debug_scale = scale > 0.0f ? scale : 1.0f;
	if (name)
		salsa64_log("filtros: diagnóstico: se ve la tabla «%s» ×%.3g (%s)", name, debug_scale,
			    debug_surface >= 0 ? "RGB = canales 0, 1, 2" : "no existe");
}

static void draw_debug(void)
{
	rsxkit_post_pass p;
	uint32_t w, h;
	rsxkit_surface_size(fl.surf[debug_surface], &w, &h);
	const rsxkit_screen *sc = rsxkit_screen_info();
	pass_init(&p, P_DEBUG, -1, w < sc->width ? w : sc->width, h < sc->height ? h : sc->height);
	p.in[0] = in_surface(debug_surface, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
	p.inputs = 1;
	set4(P_DEBUG, "k0", debug_scale, debug_scale, debug_scale, 0.0f);
	set4(P_DEBUG, "m0", 1.0f, 0.0f, 0.0f, 0.0f);
	set4(P_DEBUG, "m1", 0.0f, 1.0f, 0.0f, 0.0f);
	set4(P_DEBUG, "m2", 0.0f, 0.0f, 1.0f, 0.0f);
	run(&p);
}

bool salsa64_filters_draw(int f, const struct salsa64_filter_src *src, rsxkit_rect *out)
{
	if (f <= SALSA64_FILTER_NONE || f >= SALSA64_FILTERS || fl.failed || !src || src->factor < 1 ||
	    src->factor > 6)
		return false;
	uint32_t sw = src->region.w / src->factor, sh = src->region.h / src->factor;
	if (sw < 16 || sh < 16 || sw > 640 || sh > 480)
		return false;
	collect();
	if (!configure(f, sw, sh, src->aspect)) {
		fl.failed = true;
		return false;
	}
	if (fl.last_filter != f) {
		fl.last_filter = f;
		memset(fl.ns, 0, sizeof fl.ns);
		fl.measured = 0;
	}
	/* Una ranura de marcas libre (con más de SLOTS cuadros en vuelo, la
	 * más vieja se pierde para la medición). */
	int slot = (int)(fl.frame++ % SLOTS);
	fl.slot[slot].pending = false;

	stamp(slot, 0);
	if (rsxkit_post_reduce(&src->tex, &src->region, src->factor, fl.surf[S_SRC]) != RSXKIT_OK) {
		if (!fl.failed)
			salsa64_log("filtros: no se pudo reducir la imagen (%ux%u desde %ux%u, factor %u)", sw, sh,
				    src->region.w, src->region.h, src->factor);
		fl.failed = true;
		return false;
	}
	stamp(slot, SEG_REDUCE + 1);

	rsxkit_post_pass p;
	pass_init(&p, P_LINEAR, S_LIN, sw, sh);
	p.in[0] = in_surface(S_SRC, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
	p.inputs = 1;
	run(&p);
	pass_init(&p, P_GLOW, S_GLOW, sw * fl.g, sh * fl.g);
	p.step[0] = p.step[1] = 1.0f / fl.g;
	p.in[0] = in_surface(S_LIN, RSXKIT_FILTER_LINEAR, RSXKIT_POST_BORDER);
	p.inputs = 1;
	run(&p);
	stamp(slot, SEG_LINEAR + 1);

	if (f == SALSA64_FILTER_CRT_MASK)
		draw_mask(slot);
	else
		draw_cable(slot);
	stamp(slot, SEG_FINAL + 1);
	if (debug_surface >= 0 && fl.surf[debug_surface])
		draw_debug();
	fl.slot[slot].fence = rsxkit_fence();
	fl.slot[slot].filter = f;
	fl.slot[slot].pending = true;
	fl.drawn++;
	if (fl.check && !fl.failed)
		check_tables();
	if (out)
		*out = (rsxkit_rect){ (int32_t)fl.rect[0], (int32_t)fl.rect[1], fl.rect[2], fl.rect[3] };
	return !fl.failed;
}

bool salsa64_filters_report(char *buf, size_t size)
{
	collect();
	if (!fl.drawn || fl.last_filter < 0) {
		fl.measured = 0;
		return false;
	}
	const char *const *names = fl.last_filter == SALSA64_FILTER_CRT_MASK ? seg_mask : seg_cable;
	double k = fl.measured ? 1e-6 / fl.measured : 0.0, total = 0.0;
	for (int i = 0; i < SEGS; i++)
		total += fl.ns[i] * k;
	int n = snprintf(buf, size, "filtro «%s» %ux%u desde %ux%u: %.2f ms del RSX por cuadro (",
			 salsa64_filter_name(fl.last_filter), fl.rect[2], fl.rect[3], fl.sw, fl.sh, total);
	for (int i = 0, first = 1; i < SEGS && n > 0 && (size_t)n < size; i++) {
		if (!names[i])
			continue;
		n += snprintf(buf + n, size - (size_t)n, "%s%s %.2f", first ? "" : ", ", names[i], fl.ns[i] * k);
		first = 0;
	}
	if (n > 0 && (size_t)n < size)
		snprintf(buf + n, size - (size_t)n, "; %u cuadros, %u medidos)", fl.drawn, fl.measured);
	memset(fl.ns, 0, sizeof fl.ns);
	fl.measured = 0;
	fl.drawn = 0;
	return true;
}
