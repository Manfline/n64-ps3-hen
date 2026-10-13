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

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ppu-lv2.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include <rsxkit/post.h>
#include <spukit/spukit.h>

#include "frontend/filters/crt_params.h"
#include "frontend/filters/crt_spe.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"

#include "crt_cable_decode27_fpo.h"
#include "crt_cable_decode9_fpo.h"
#include "crt_cable_signal_fpo.h"
#include "crt_cable_spe_lin_fpo.h"
#include "spu_crt_bin.h"
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
#include "crt_trinitron_lines_a_fpo.h"
#include "crt_trinitron_lines_b_fpo.h"

static const struct {
	const char *name, *key;
} crt_names[SALSA64_CRTS] = {
	[SALSA64_CRT_NONE] = { "Ninguno", "ninguno" },
	[SALSA64_CRT_MASK] = { "CRT máscara de sombra (experimental)", "crt-mascara" },
	[SALSA64_CRT_RGB] = { "CRT cable RGB (experimental)", "crt-rgb" },
	[SALSA64_CRT_SVIDEO] = { "CRT cable S-Video (experimental)", "crt-svideo" },
	[SALSA64_CRT_COMPOSITE] = { "CRT cable compuesto (experimental)", "crt-compuesto" },
	[SALSA64_CRT_TRINITRON] = { "CRT Trinitron (experimental)", "crt-trinitron" },
};

const char *salsa64_crt_name(int f)
{
	return f >= 0 && f < SALSA64_CRTS ? crt_names[f].name : "?";
}

const char *salsa64_crt_key(int f)
{
	return f >= 0 && f < SALSA64_CRTS ? crt_names[f].key : "ninguno";
}

int salsa64_crt_from_key(const char *key)
{
	for (int f = 0; f < SALSA64_CRTS; f++)
		if (!strcmp(key, crt_names[f].key))
			return f;
	return -1;
}

/* Tramos medidos de cada cuadro (marcas de tiempo del RSX entre pasadas). */
enum { SEG_REDUCE, SEG_LINEAR, SEG_TABLES, SEG_LINES, SEG_FINAL, SEGS };
static const char *const seg_mask[SEGS] = { "reducir", "lineal y resplandor", "teselas y puntos", NULL, "final" };
static const char *const seg_cable[SEGS] = { "reducir", "lineal y resplandor", "señal y decodificación", "líneas",
					     "final" };
static const char *const seg_trin[SEGS] = { "reducir", "lineal y resplandor", NULL, "líneas", "final" };
/* Cuadros en vuelo cuyas marcas se leen cuando su barrera pasó; cada uno
 * con STAMPS_SLOT marcas: 0 .. SEGS por tramo y, con el SPE, cuando el RSX
 * dejó la imagen y cuando terminó de esperar al SPE. */
#define SLOTS 4
#define STAMPS_SLOT  8
#define STAMP_SPE_IN (SEGS + 1)
#define STAMP_SPE_GO (SEGS + 2)

enum {
	P_LINEAR, P_GLOW, P_TILE_A, P_TILE_B, P_TILE_C, P_DOTS, P_MASK_FINAL,
	P_SIGNAL_E, P_SIGNAL_O, P_DECODE9, P_DECODE27, P_LINES_A4, P_LINES_B4, P_LINES_A8, P_LINES_B8, P_TUBE_FINAL,
	P_TRIN_A, P_TRIN_B, P_SPE_LIN, P_DEBUG, PROGRAMS
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
	[P_TRIN_A] = crt_trinitron_lines_a_fpo,
	[P_TRIN_B] = crt_trinitron_lines_b_fpo,
	[P_SPE_LIN] = crt_cable_spe_lin_fpo,
	[P_DEBUG] = crt_debug_fpo,
};

/* S_SPE_*: en la memoria principal (spe.mem), con `filtros-spe`. */
enum { S_SRC, S_LIN, S_GLOW, S_TILE_A, S_TILE_B, S_TILE_C, S_DOTS, S_SIG_E, S_SIG_O, S_DEC_E, S_DEC_O, S_LINES_A,
       S_LINES_B, S_SPE_IN, S_SPE_E, S_SPE_O, SURFACES };

static struct {
	/* Algo falló (sin memoria, una pasada que el RSX no aceptó): sin filtro
	 * mientras la imagen y el filtro sean los de `fail`; con otros (otra
	 * escala, otro tamaño, otro filtro) se vuelve a intentar. */
	bool failed;
	struct {
		int f;
		uint32_t sw, sh, factor, ow, oh;
		float aspect;
	} fail;
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
		bool reused;    /* solo la pasada final (marcas 0 y SEGS) */
		bool spe;       /* señal y decodificación en el SPE */
	} slot[SLOTS];
	uint32_t frame;
	uint64_t ns[SEGS], ns_reused, ns_spe_wait, ns_spe_trip;
	uint32_t measured, measured_reused, measured_spe, drawn, reused;
	bool spe_frame;         /* este cuadro va con el SPE (spe_kick) */
	int last_filter;
	bool check;             /* falta la comprobación de este filtro */
	bool checked[SALSA64_CRTS];
	uint32_t setup_ms[SALSA64_CRTS];  /* lo que tardó la última preparación */
	uint32_t since_setup;   /* cuadros dibujados desde la última preparación */
	/* La imagen del último cuadro completo: si no cambió, solo la pasada
	 * final (las tablas siguen valiendo). */
	struct {
		bool valid;
		uint32_t gen, offset, factor;
		uint8_t location;
		rsxkit_rect region;
	} last;
} fl = { .filter = -1, .last_filter = -1 };

static bool spe_prepare(int f);
static void spe_release(void);

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

static void cap_release(void);

void salsa64_filters_release(void)
{
	spe_release();
	rsxkit_finish();
	cap_release();
	free_surfaces();
	for (int i = 0; i < PROGRAMS; i++) {
		rsxkit_program_destroy(fl.prog[i]);
		fl.prog[i] = NULL;
	}
	memset(fl.slot, 0, sizeof fl.slot);
	fl.filter = -1;
	fl.failed = false;
	fl.last.valid = false;
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
	return f == SALSA64_CRT_RGB || f == SALSA64_CRT_SVIDEO || f == SALSA64_CRT_COMPOSITE;
}

/* El tubo de franjas de un filtro de franjas (los cables y el Trinitron). */
static const struct crt_tube_preset *tube_of(int f)
{
	return f == SALSA64_CRT_TRINITRON ? &crt_trinitron_kv29.tube : &crt_cable_tv13.tube;
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

/* Las pasadas de líneas `la` y `lb` y la final del tubo `t` (franjas de
 * fl.gr); q1 = (píxeles del juego por píxel de pantalla en x, q1y, q1z). */
static void tube_constants(int la, int lb, const struct crt_tube_preset *t, float q1y, float q1z)
{
	float to_x = (float)fl.sw / fl.rect[2], to_y = (float)fl.sh / fl.rect[3];
	float hw = t->stripe_width / 6.0f;
	float pk[3], ipk[3], iavg[3];
	for (int i = 0; i < 3; i++) {
		pk[i] = fl.gr.peak[i];
		ipk[i] = 1.0f / (pk[i] - 1.0f > 1e-4f ? pk[i] - 1.0f : 1e-4f);
		iavg[i] = 1.0f / fl.gr.avg[i];
	}
	for (int k = 0; k < 2; k++) {
		int pi = k ? lb : la;
		set4(pi, "q1", to_x, q1y, q1z, 0.0f);
		set4(pi, "q2", t->beam_min, t->beam_max, t->keep_bright, t->brightness);
		set4(pi, "q3", 1.0f / fl.gr.pitch, t->mask_strength, hw * (1.0f - t->stripe_soft), 1.0f / (hw * t->stripe_soft));
		set4(pi, "q4", iavg[0], iavg[1], iavg[2], 0.0f);
		set4(pi, "q5", pk[0], pk[1], pk[2], 0.0f);
		set4(pi, "q6", ipk[0], ipk[1], ipk[2], 0.0f);
	}
	set4(P_TUBE_FINAL, "f1", to_x, to_y, 0.0f, 0.0f);
	set4(P_TUBE_FINAL, "f2", to_x * fl.g, to_y * fl.g, 1.0f / t->gamma_out, 0.0f);
}

/* Las constantes del CRT con cable. */
static void cable_constants(int f)
{
	const struct crt_cable_preset *p = &crt_cable_tv13;
	const struct crt_tube_preset *t = &p->tube;
	const struct crt_cable_taps *c = &fl.ct;
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
	tube_constants(c->grid8 ? P_LINES_A8 : P_LINES_A4, c->grid8 ? P_LINES_B8 : P_LINES_B4, t, c->x0, 1.0f / c->dt);
	(void)f;
}

/* Las constantes del Trinitron: el tubo, y en q1 el desenfoque del haz a lo
 * ancho y la saturación. */
static void trinitron_constants(void)
{
	const struct crt_trinitron_preset *p = &crt_trinitron_kv29;
	tube_constants(P_TRIN_A, P_TRIN_B, &p->tube, -1.442695f * 0.5f / (p->h_blur * p->h_blur), p->tube.saturation);
}

/* Si las tablas y constantes ya son las del filtro f con una imagen de
 * sw × sh en la pantalla de ahora. */
static bool configured(int f, uint32_t sw, uint32_t sh, float aspect)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	return fl.filter == f && fl.sw == sw && fl.sh == sh && fl.ow == sc->width && fl.oh == sc->height &&
	       fl.aspect == aspect;
}

/* Prepara tablas y constantes para el filtro f con una imagen de sw × sh
 * en la pantalla de ahora. false si no hay memoria. */
static bool configure(int f, uint32_t sw, uint32_t sh, float aspect)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	if (configured(f, sw, sh, aspect))
		return true;
	fl.last.valid = false;
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
	if (ok && f == SALSA64_CRT_MASK) {
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
				    salsa64_crt_name(f), fl.rect[2], fl.rect[3], fl.rect[0], fl.rect[1], sw, sh, fl.ml.d,
				    fl.ml.h, fl.ml.r, fl.ml.avg, fl.ml.peak, fl.ml.dots_w, fl.ml.dots_h, fl.ml.tile_w,
				    fl.ml.tile_h, fl.g);
		}
	} else if (ok && is_cable(f)) {
		const struct crt_cable_preset *p = &crt_cable_tv13;
		enum crt_cable cable = f == SALSA64_CRT_RGB      ? CRT_CABLE_RGB
				       : f == SALSA64_CRT_SVIDEO ? CRT_CABLE_SVIDEO
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
			ok = spe_prepare(f);
			salsa64_log("filtros: «%s» en %ux%u (%u,%u) desde %ux%u: %d muestras desde %d cada %.3f px (%u "
				    "columnas%s), franjas cada %.0f px, resplandor ×%u",
				    salsa64_crt_name(f), fl.rect[2], fl.rect[3], fl.rect[0], fl.rect[1], sw, sh,
				    fl.ct.count, fl.ct.jlo, fl.ct.dt, fl.ct.cols, fl.ct.grid8 ? ", dos tablas" : "",
				    fl.gr.pitch, fl.g);
		}
	} else if (ok && f == SALSA64_CRT_TRINITRON) {
		const struct crt_trinitron_preset *p = &crt_trinitron_kv29;
		crt_grille(&p->tube, (float)fl.rect[2] / sw, &fl.gr);
		ok = make_surface(S_LINES_A, fl.rect[2], sh, RSXKIT_POST_RGBA16F) &&
		     make_surface(S_LINES_B, fl.rect[2], sh, RSXKIT_POST_RGBA16F);
		if (ok) {
			glow_constants(p->tube.gamma_in, p->tube.glow, p->tube.glow_radius, p->tube.brightness);
			trinitron_constants();
			salsa64_log("filtros: «%s» en %ux%u (%u,%u) desde %ux%u: franjas cada %.0f px, desenfoque del haz "
				    "%.2f, resplandor ×%u",
				    salsa64_crt_name(f), fl.rect[2], fl.rect[3], fl.rect[0], fl.rect[1], sw, sh, fl.gr.pitch,
				    p->h_blur, fl.g);
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
	fl.check = !fl.checked[f];
	fl.since_setup = 0;
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

static void check_tables(void)
{
	fl.check = false;
	fl.checked[fl.filter] = true;
	rsxkit_finish();
	uint32_t ps;
	const volatile uint8_t *S = rsxkit_surface_data(fl.surf[S_SRC], &ps);
	const bool mask = fl.filter == SALSA64_CRT_MASK;
	const struct crt_tube_preset *tube = tube_of(fl.filter);
	float gin = mask ? crt_mask_monitor.gamma_in : tube->gamma_in;
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
	const float k = (mask ? crt_mask_monitor.glow * crt_mask_monitor.brightness * (1.0f - crt_mask_monitor.black_level)
			      : tube->glow * tube->brightness) /
			12.0f;
	const float radius = mask ? crt_mask_monitor.glow_radius : tube->glow_radius;
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
	if (mask) {
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
		    salsa64_crt_name(fl.filter), lin_err, lin_swap, glow_err, glow_point, extra);
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
		uint64_t t[STAMPS_SLOT];
		for (int i = 0; i < STAMPS_SLOT; i++)
			t[i] = rsxkit_stamp_ns((uint32_t)(s * STAMPS_SLOT + i));
		if (fl.slot[s].reused) {
			if (t[SEGS] >= t[0] && t[SEGS] - t[0] <= 100000000u) {
				fl.ns_reused += t[SEGS] - t[0];
				fl.measured_reused++;
			}
			continue;
		}
		bool sane = true;
		for (int i = 0; i < SEGS; i++)
			if (t[i + 1] < t[i] || t[i + 1] - t[i] > 100000000u)
				sane = false;
		if (!sane)
			continue;
		for (int i = 0; i < SEGS; i++)
			fl.ns[i] += t[i + 1] - t[i];
		fl.measured++;
		if (fl.slot[s].spe && t[STAMP_SPE_GO] >= t[STAMP_SPE_IN] && t[STAMP_SPE_GO] >= t[SEG_LINEAR + 1] &&
		    t[STAMP_SPE_GO] - t[STAMP_SPE_IN] < 100000000u) {
			fl.ns_spe_trip += t[STAMP_SPE_GO] - t[STAMP_SPE_IN];
			fl.ns_spe_wait += t[STAMP_SPE_GO] - t[SEG_LINEAR + 1];
			fl.measured_spe++;
		}
	}
}

static void stamp(int slot, int i)
{
	rsxkit_stamp((uint32_t)(slot * STAMPS_SLOT + i));
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

/* La pasada final del filtro en curso: en la pantalla (out NULL, en el
 * rectángulo de la imagen) o en una superficie del tamaño de la imagen. Lee
 * solo tablas, que siguen valiendo mientras la imagen no cambie. */
static void final_pass(rsxkit_surface *out)
{
	rsxkit_post_pass p;
	if (fl.filter == SALSA64_CRT_MASK) {
		pass_init(&p, P_MASK_FINAL, -1, fl.rect[2], fl.rect[3]);
		for (int i = 0; i < 3; i++)
			p.in[i] = in_surface(S_TILE_A + i, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
		p.in[3] = in_surface(S_DOTS, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
		p.in[4] = in_surface(S_GLOW, RSXKIT_FILTER_LINEAR, RSXKIT_POST_CLAMP);
		p.inputs = 5;
	} else {
		pass_init(&p, P_TUBE_FINAL, -1, fl.rect[2], fl.rect[3]);
		p.in[0] = in_surface(S_LINES_A, RSXKIT_FILTER_NEAREST, RSXKIT_POST_BORDER);
		p.in[1] = in_surface(S_LINES_B, RSXKIT_FILTER_NEAREST, RSXKIT_POST_BORDER);
		p.in[2] = in_surface(S_GLOW, RSXKIT_FILTER_LINEAR, RSXKIT_POST_CLAMP);
		p.inputs = 3;
	}
	if (out) {
		p.out = out;
	} else {
		p.rect.x = (int32_t)fl.rect[0];
		p.rect.y = (int32_t)fl.rect[1];
	}
	run(&p);
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
	final_pass(NULL);
}

/* Las dos pasadas de líneas del tubo (A: amplitud y k rojo; B: k verde y
 * azul) desde las tablas `e` y `o` (o < 0: una sola). */
static void tube_lines(int pa, int pb, int e, int o)
{
	rsxkit_post_pass p;
	for (int k = 0; k < 2; k++) {
		pass_init(&p, k ? pb : pa, k ? S_LINES_B : S_LINES_A, fl.rect[2], fl.sh);
		p.in[0] = in_surface(e, RSXKIT_FILTER_NEAREST, o < 0 && e == S_LIN ? RSXKIT_POST_BORDER : RSXKIT_POST_CLAMP);
		p.inputs = 1;
		if (o >= 0) {
			p.in[1] = in_surface(o, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
			p.inputs = 2;
		}
		run(&p);
	}
}

static void spe_wait_rsx(int slot);

static void draw_cable(int slot)
{
	const struct crt_cable_taps *c = &fl.ct;
	rsxkit_post_pass p;
	int dec = c->count > 9 ? P_DECODE27 : P_DECODE9;
	for (int s = 0; !fl.spe_frame && s < (c->grid8 ? 2 : 1); s++) {
		pass_init(&p, s ? P_SIGNAL_O : P_SIGNAL_E, s ? S_SIG_O : S_SIG_E, c->cols, fl.sh);
		p.in[0] = in_surface(S_SRC, RSXKIT_FILTER_LINEAR, RSXKIT_POST_BORDER);
		p.inputs = 1;
		run(&p);
		pass_init(&p, dec, s ? S_DEC_O : S_DEC_E, c->cols, fl.sh);
		p.in[0] = in_surface(s ? S_SIG_O : S_SIG_E, RSXKIT_FILTER_NEAREST, RSXKIT_POST_BORDER);
		p.inputs = 1;
		run(&p);
	}
	if (fl.spe_frame)
		spe_wait_rsx(slot);
	stamp(slot, SEG_TABLES + 1);
	if (c->grid8)
		tube_lines(P_LINES_A8, P_LINES_B8, S_DEC_E, S_DEC_O);
	else
		tube_lines(P_LINES_A4, P_LINES_B4, S_DEC_E, -1);
	stamp(slot, SEG_LINES + 1);
	final_pass(NULL);
}

static void draw_trinitron(int slot)
{
	stamp(slot, SEG_TABLES + 1);
	tube_lines(P_TRIN_A, P_TRIN_B, S_LIN, -1);
	stamp(slot, SEG_LINES + 1);
	final_pass(NULL);
}

/* Diagnóstico (salsa64_filters_debug): la tabla `s` en la esquina de la
 * pantalla, un texel por píxel, en lugar de la imagen final. */
static int debug_surface = -1;
static float debug_scale = 1.0f;

static const char *const surface_names[SURFACES] = { "fuente", "lineal", "resplandor", "teselaA", "teselaB", "teselaC",
						     "puntos", "senal", "senalO", "decodificada", "decodificadaO",
						     "lineasA", "lineasB", "spe", "speE", "speO" };

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

/* Copia la superficie s (sus canales 0 a 2 por `scale`) en `out` o, con
 * out NULL, en la esquina de la pantalla. */
static void copy_surface(int s, float scale, rsxkit_surface *out)
{
	rsxkit_post_pass p;
	uint32_t w, h;
	rsxkit_surface_size(fl.surf[s], &w, &h);
	const rsxkit_screen *sc = rsxkit_screen_info();
	pass_init(&p, P_DEBUG, -1, out || w < sc->width ? w : sc->width, out || h < sc->height ? h : sc->height);
	p.out = out;
	p.in[0] = in_surface(s, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
	p.inputs = 1;
	set4(P_DEBUG, "k0", scale, scale, scale, 0.0f);
	set4(P_DEBUG, "m0", 1.0f, 0.0f, 0.0f, 0.0f);
	set4(P_DEBUG, "m1", 0.0f, 1.0f, 0.0f, 0.0f);
	set4(P_DEBUG, "m2", 0.0f, 0.0f, 1.0f, 0.0f);
	run(&p);
}

/* ---- Capturas (salsa64_filters_options) ---------------------------------------
 * La primera vez de cada filtro y alto de pantalla: el RSX vuelve a dibujar
 * la pasada final en una superficie de la memoria principal y copia ahí la
 * imagen que entró al filtro; un hilo espera su barrera y escribe los .ppm.
 * Leer la pantalla desde el PPE (la memoria local del RSX) congelaba la
 * imagen ~2 s. */

#define CAP_BYTES (10u << 20)  /* 1920 × 1080 y 640 × 480 en RGBA8, en un bloque mapeado */
/* La captura espera estos cuadros del filtro (la imagen del primero puede
 * ser la de antes de cambiar). */
#define CAP_AFTER 60

static struct {
	bool on;
	bool done[SALSA64_CRTS][2];
	uint8_t *mem;
	uint32_t offset;
	rsxkit_surface *scr, *src;
	volatile int busy;
	bool thread;
	sys_ppu_thread_t tid;
	/* Lo que escribe el hilo. */
	uint32_t fence, sw, sh, src_pitch, rx, ry, rw, rh, scr_pitch, ow, oh;
	int filter;
} cap;

static void cap_write(void)
{
	char path[96];
	snprintf(path, sizeof path, SALSA64_DATA_DIR "/filtro-%s-%u-fuente.ppm", salsa64_crt_key(cap.filter), cap.oh);
	FILE *f = fopen(path, "wb");
	static uint8_t line[3 * 1920];
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", cap.sw, cap.sh);
		const uint8_t *S = cap.mem + cap.rh * cap.scr_pitch;
		for (uint32_t y = 0; y < cap.sh; y++) {
			for (uint32_t x = 0; x < cap.sw; x++)
				for (int c = 0; c < 3; c++)
					line[3 * x + c] = S[y * cap.src_pitch + x * 4 + 1 + c];
			fwrite(line, 3, cap.sw, f);
		}
		fclose(f);
	}
	snprintf(path, sizeof path, SALSA64_DATA_DIR "/filtro-%s-%u-pantalla.ppm", salsa64_crt_key(cap.filter), cap.oh);
	f = fopen(path, "wb");
	if (f) {
		uint32_t ow = cap.ow < 1920 ? cap.ow : 1920;
		fprintf(f, "P6\n%u %u\n255\n", ow, cap.oh);
		for (uint32_t y = 0; y < cap.oh; y++) {
			memset(line, 0, 3 * ow);
			if (y >= cap.ry && y < cap.ry + cap.rh) {
				const uint8_t *row = cap.mem + (y - cap.ry) * cap.scr_pitch;
				for (uint32_t x = 0; x < cap.rw && cap.rx + x < ow; x++)
					for (int c = 0; c < 3; c++)
						line[3 * (cap.rx + x) + c] = row[x * 4 + 1 + c];
			}
			fwrite(line, 3, ow, f);
		}
		fclose(f);
	}
	salsa64_log("filtros: entrada y pantalla de «%s» en filtro-%s-%u-fuente.ppm y -pantalla.ppm",
		    salsa64_crt_name(cap.filter), salsa64_crt_key(cap.filter), cap.oh);
}

static void cap_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 2500 && !rsxkit_fence_done(cap.fence); i++)
		sysUsleep(2000);
	if (rsxkit_fence_done(cap.fence))
		cap_write();
	else
		salsa64_log("filtros: la captura no llegó (el RSX no pasó su barrera en 5 s)");
	cap.busy = 0;
	sysThreadExit(0);
}

static void cap_join(void)
{
	if (!cap.thread)
		return;
	u64 rv;
	sysThreadJoin(cap.tid, &rv);
	cap.thread = false;
}

/* Espera al hilo de las capturas y suelta sus superficies (la memoria
 * mapeada queda para la próxima). */
static void cap_release(void)
{
	while (cap.busy)
		sysUsleep(2000);
	cap_join();
	rsxkit_surface_destroy(cap.scr);
	rsxkit_surface_destroy(cap.src);
	cap.scr = cap.src = NULL;
}

/* Después de un cuadro completo: si toca, la captura de este filtro. */
static void capture(void)
{
	bool *done = &cap.done[fl.filter][fl.oh >= 1000];
	if (!cap.on || !cap.mem || *done || cap.busy || fl.since_setup < CAP_AFTER)
		return;
	cap_join();
	rsxkit_surface_destroy(cap.scr);
	rsxkit_surface_destroy(cap.src);
	cap.scr = cap.src = NULL;
	uint32_t sp = (fl.rect[2] * 4 + 63) & ~63u, cp = (fl.sw * 4 + 63) & ~63u;
	uint32_t sbytes = (sp * fl.rect[3] + 127) & ~127u;
	if (sbytes + cp * fl.sh > CAP_BYTES)
		return;
	cap.scr = rsxkit_surface_wrap_main(fl.rect[2], fl.rect[3], RSXKIT_POST_RGBA8, cap.mem, cap.offset, sp);
	cap.src = rsxkit_surface_wrap_main(fl.sw, fl.sh, RSXKIT_POST_RGBA8, cap.mem + sbytes, cap.offset + sbytes, cp);
	if (!cap.scr || !cap.src)
		return;
	*done = true;
	final_pass(cap.scr);
	copy_surface(S_SRC, 1.0f, cap.src);
	cap.filter = fl.filter;
	cap.sw = fl.sw;
	cap.sh = fl.sh;
	cap.src_pitch = cp;
	cap.rx = fl.rect[0];
	cap.ry = fl.rect[1];
	cap.rw = fl.rect[2];
	cap.rh = fl.rect[3];
	cap.scr_pitch = sp;
	cap.ow = fl.ow;
	cap.oh = fl.oh;
	cap.fence = rsxkit_fence();
	cap.busy = 1;
	if (sysThreadCreate(&cap.tid, cap_thread, NULL, 1500, 0x10000, THREAD_JOINABLE, (char *)"salsa64_capturas") != 0) {
		salsa64_log("filtros: no se pudo crear el hilo de las capturas");
		cap.busy = 0;
		return;
	}
	cap.thread = true;
}

void salsa64_filters_options(bool captures, int reduce_passes)
{
	cap.on = captures;
	if (captures && !cap.mem) {
		cap.mem = memalign(1u << 20, CAP_BYTES);
		if (!cap.mem || rsxkit_map_main(cap.mem, CAP_BYTES, &cap.offset) != RSXKIT_OK) {
			salsa64_log("filtros: sin memoria principal mapeada para las capturas");
			free(cap.mem);
			cap.mem = NULL;
			cap.on = false;
		}
	}
	rsxkit_post_reduce_passes(reduce_passes);
	salsa64_log("filtros: reducir en %d pasada%s; capturas %s", rsxkit_post_reduce_passes(0),
		    rsxkit_post_reduce_passes(0) == 1 ? "" : "s", captures ? "sí (filtro-<clave>-<alto>-*.ppm)" : "no");
}

/* ---- Señal y decodificación en el SPE (filtros-spe; crt_spe.h) -----------------
 * S-Video y compuesto: el RSX copia la imagen reducida a la memoria
 * principal y escribe L_SPE_IN; dos SPEs (la mitad de las líneas cada uno)
 * hacen la señal y la decodificación y escriben L_SPE_OUT(i); mientras, el
 * RSX hace lineal y resplandor, y antes de linealizar la tabla del SPE
 * espera las dos etiquetas. Si un SPE no termina (el RSX ya le dejó la
 * imagen y pasaron SPE_TIMEOUT_US), el PPE escribe su etiqueta para soltar
 * al RSX y desde ahí todo va en el RSX. */

#define SPE_BYTES      (8u << 20)  /* configuración, imagen reducida y tablas E y O (≤ 7,1 MB) */
#define SPE_IN_OFF     (32u << 10)
#define L_SPE_IN       236
#define L_SPE_OUT(i)   (237 + (i))
#define SPE_TIMEOUT_US 300000u
#define SPE_INFLIGHT   4

_Static_assert(L_SPE_OUT(CRT_SPE_WORKERS - 1) <= RSXKIT_LABEL_USER_MAX && L_SPE_IN >= RSXKIT_LABEL_USER_MIN,
	       "etiquetas del SPE fuera de las de quien usa rsxkit");
_Static_assert(sizeof(struct crt_spe_config) <= SPE_IN_OFF, "la configuración del SPE no cabe");

static struct {
	bool want, on, failed;
	spukit_worker *w[CRT_SPE_WORKERS];
	uint8_t *mem;
	uint32_t offset;
	uint32_t cfg_id, seq;
	uint32_t in_pitch, out_pitch, e_off, o_off;
	/* Cuadros en vuelo (en orden). */
	struct {
		uint32_t seq, id[CRT_SPE_WORKERS];
		bool done[CRT_SPE_WORKERS];
		uint64_t t_in;  /* cuando se vio que el RSX dejó la imagen (0: aún no) */
		uint32_t wait, work;
	} job[SPE_INFLIGHT];
	unsigned head, n;
	uint64_t wait_ticks, work_ticks;
	uint32_t frames;
} spe;

void salsa64_filters_spe(bool on)
{
	if (spe.want == on)
		return;
	spe.want = on;
	fl.filter = -1;  /* la configuración se rehace en el próximo cuadro */
	salsa64_log("filtros: la señal y la decodificación de S-Video y compuesto, en %s",
		    on ? (spe.failed ? "el RSX (el SPE falló antes)" : "dos SPE") : "el RSX");
}

static volatile uint32_t *spe_label(uint32_t i)
{
	return rsxkit_label_address(i);
}

static uint64_t ea_of(const void *p)
{
	return (uint64_t)(uintptr_t)p;
}

/* Los resultados de los cuadros en vuelo, en orden; con `all`, espera
 * a todos. El que no llega: ver arriba. */
static void spe_collect(bool all)
{
	static bool told;
	while (spe.n) {
		__typeof__(spe.job[0]) *e = &spe.job[spe.head];
		bool pending = false;
		for (int i = 0; i < CRT_SPE_WORKERS; i++) {
			while (!e->done[i]) {
				uint64_t now = sysGetSystemTime();
				if (!e->t_in && (int32_t)(*spe_label(L_SPE_IN) - e->seq) >= 0)
					e->t_in = now;
				if (spukit_done(spe.w[i], e->id[i])) {
					const spukit_result *r = spukit_wait(spe.w[i], e->id[i]);
					struct crt_spe_result res;
					memcpy(&res, r->data, sizeof res);
					e->done[i] = true;
					if (res.err) {
						/* No escribió su etiqueta: la escribe el PPE. Sin la
						 * imagen en 2 s (el RSX retenido) no es falla del SPE. */
						*spe_label(L_SPE_OUT(i)) = e->seq;
						if (res.err != -3) {
							spe.failed = true;
							spe.on = false;
						}
						if (!told) {
							salsa64_log("filtros: el SPE %d devolvió %d en el cuadro %u%s", i, res.err, e->seq,
								    spe.failed ? ": la señal va en el RSX" : "");
							told = true;
						}
					}
					e->wait = res.wait_ticks > e->wait ? res.wait_ticks : e->wait;
					e->work = res.work_ticks > e->work ? res.work_ticks : e->work;
				} else if (e->t_in && now - e->t_in > SPE_TIMEOUT_US) {
					*spe_label(L_SPE_OUT(i)) = e->seq;
					e->done[i] = true;
					spe.failed = true;
					spe.on = false;
					salsa64_log("filtros: el SPE %d no terminó el cuadro %u en %u ms: la señal va en el RSX", i,
						    e->seq, SPE_TIMEOUT_US / 1000);
				} else if (all) {
					sysUsleep(100);
				} else {
					pending = true;
					break;
				}
			}
			if (pending)
				break;
		}
		if (pending)
			break;
		spe.wait_ticks += e->wait;
		spe.work_ticks += e->work;
		spe.frames++;
		spe.head = (spe.head + 1) % SPE_INFLIGHT;
		spe.n--;
	}
}

static bool spe_start(void)
{
	if (!spe.mem) {
		spe.mem = memalign(1u << 20, SPE_BYTES);
		if (!spe.mem || rsxkit_map_main(spe.mem, SPE_BYTES, &spe.offset) != RSXKIT_OK) {
			salsa64_log("filtros: sin memoria principal mapeada para el SPE: la señal va en el RSX");
			free(spe.mem);
			spe.mem = NULL;
			spe.failed = true;
			return false;
		}
	}
	for (int i = 0; i < CRT_SPE_WORKERS; i++)
		if (!spe.w[i] && !(spe.w[i] = spukit_worker_start(spu_crt_bin, "salsa64_crt", 0))) {
			salsa64_log("filtros: no arrancó el SPE %d (%d): la señal va en el RSX", i, spukit_last_error());
			spe.failed = true;
			return false;
		}
	return true;
}

/* Lo que no cambia de un cuadro a otro, para el SPE (la cuenta de
 * crt_cable_signal.fcg con las constantes de cable_constants). */
static void spe_config(void)
{
	const struct crt_cable_preset *p = &crt_cable_tv13;
	const struct crt_cable_taps *c = &fl.ct;
	struct crt_spe_config *k = (struct crt_spe_config *)spe.mem;
	static const float yiq[3][3] = { { 0.299f, 0.587f, 0.114f }, { 0.596f, -0.274f, -0.322f }, { 0.211f, -0.523f, 0.312f } };
	static const float rgb[3][3] = { { 1.0f, 0.956f, 0.621f }, { 1.0f, -0.272f, -0.647f }, { 1.0f, -1.106f, 1.703f } };
	memset(k, 0, sizeof *k);
	k->id = ++spe.cfg_id;
	k->sw = fl.sw;
	k->sh = fl.sh;
	k->cols = c->cols;
	k->grids = c->grid8 ? 2 : 1;
	k->jlo = c->jlo;
	k->kc = c->kc;
	for (int j = 0; j < CRT_SPE_TAPS; j++) {
		k->wl[j] = c->w[j][0];
		k->wc[j] = c->w[j][1];
	}
	for (int i = 0; i < 3; i++)
		for (int x = 0; x < 3; x++) {
			k->yiq[i][x] = yiq[i][x];
			k->rgb[i][x] = rgb[i][x];
		}
	for (uint32_t y = 0; y < fl.sh; y++) {
		double ph = 6.283185307179586 * fmod(y * (double)p->line_phase + p->frame_phase, 1.0);
		k->line[y][0] = (float)cos(ph);
		k->line[y][1] = (float)sin(ph);
	}
	for (uint32_t g = 0; g < k->grids; g++)
		for (uint32_t m = 0; m < c->cols; m++) {
			double x = c->x0 + (g ? 0.5 * c->dt : 0.0) + m * (double)c->dt, a = x + 0.5 * c->hold, n = floor(a);
			double t = (a - n) / c->hold, th = 6.283185307179586 * (c->f * x - floor(c->f * x));
			k->col[g][m].n = (int32_t)n;
			k->col[g][m].t = (float)(t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t);
			k->col[g][m].c = (float)cos(th);
			k->col[g][m].s = (float)sin(th);
		}
}

/* Desde configure, con las tablas del cable listas: si este filtro va con
 * el SPE, su memoria, sus superficies y su configuración. false solo si
 * falta memoria del RSX (como las demás tablas). */
static bool spe_prepare(int f)
{
	spe.on = false;
	if (!spe.want || spe.failed || (f != SALSA64_CRT_SVIDEO && f != SALSA64_CRT_COMPOSITE))
		return true;
	const struct crt_cable_taps *c = &fl.ct;
	bool fits = c->count == CRT_SPE_TAPS && c->cols <= CRT_SPE_MAX_COLS && fl.sw <= CRT_SPE_MAX_SW &&
		    fl.sh <= CRT_SPE_MAX_LINES && c->jlo >= -(int)CRT_SPE_PAD && c->jlo + CRT_SPE_TAPS + 1 <= (int)CRT_SPE_PAD;
	for (int j = 0; fits && j < c->count; j++)
		fits = c->w[j][1] == c->w[j][2];
	if (!fits) {
		salsa64_log("filtros: «%s» no cabe en el SPE (%u columnas, %d muestras desde %d): va en el RSX",
			    salsa64_crt_name(f), c->cols, c->count, c->jlo);
		return true;
	}
	spe_collect(true);
	if (!spe_start())
		return true;
	spe.in_pitch = (fl.sw * 4 + 63) & ~63u;
	spe.out_pitch = (c->cols * 8 + 63) & ~63u;
	spe.e_off = (SPE_IN_OFF + spe.in_pitch * fl.sh + 127) & ~127u;
	spe.o_off = (spe.e_off + spe.out_pitch * fl.sh + 127) & ~127u;
	if (spe.o_off + spe.out_pitch * fl.sh > SPE_BYTES)
		return true;
	fl.surf[S_SPE_IN] = rsxkit_surface_wrap_main(fl.sw, fl.sh, RSXKIT_POST_RGBA8, spe.mem + SPE_IN_OFF,
						     spe.offset + SPE_IN_OFF, spe.in_pitch);
	fl.surf[S_SPE_E] = rsxkit_surface_wrap_main(c->cols, fl.sh, RSXKIT_POST_RGBA16F, spe.mem + spe.e_off,
						    spe.offset + spe.e_off, spe.out_pitch);
	if (c->grid8)
		fl.surf[S_SPE_O] = rsxkit_surface_wrap_main(c->cols, fl.sh, RSXKIT_POST_RGBA16F, spe.mem + spe.o_off,
							    spe.offset + spe.o_off, spe.out_pitch);
	if (!fl.surf[S_SPE_IN] || !fl.surf[S_SPE_E] || (c->grid8 && !fl.surf[S_SPE_O]))
		return false;
	spe_config();
	set4(P_SPE_LIN, "k0", crt_cable_tv13.tube.gamma_in, crt_cable_tv13.tube.saturation, 0.0f, 0.0f);
	/* Las etiquetas desde 0 (configure esperó al RSX y spe_collect a los SPEs). */
	*spe_label(L_SPE_IN) = 0;
	for (int i = 0; i < CRT_SPE_WORKERS; i++)
		*spe_label(L_SPE_OUT(i)) = 0;
	spe.seq = 0;
	spe.on = true;
	salsa64_log("filtros: señal y decodificación de «%s» en %d SPE (%u columnas × %u líneas%s; configuración %u)",
		    salsa64_crt_name(f), CRT_SPE_WORKERS, c->cols, fl.sh, c->grid8 ? ", dos tablas" : "", spe.cfg_id);
	return true;
}

/* Después de reducir: el RSX copia la imagen a la memoria principal y
 * escribe L_SPE_IN; los SPEs reciben su trabajo. */
static void spe_kick(int slot)
{
	fl.spe_frame = false;
	spe_collect(false);
	if (!spe.on || !is_cable(fl.filter))
		return;
	if (spe.n == SPE_INFLIGHT)
		spe_collect(true);
	uint32_t seq = ++spe.seq;
	copy_surface(S_SRC, 1.0f, fl.surf[S_SPE_IN]);
	rsxkit_post_label_write(L_SPE_IN, seq);
	stamp(slot, STAMP_SPE_IN);
	struct crt_spe_job j;
	memset(&j, 0, sizeof j);
	j.config_ea = ea_of(spe.mem);
	j.config_id = spe.cfg_id;
	j.config_bytes = sizeof(struct crt_spe_config);
	j.seq = seq;
	j.src_ea = ea_of(spe.mem + SPE_IN_OFF);
	j.src_pitch = spe.in_pitch;
	j.out_ea[0] = ea_of(spe.mem + spe.e_off);
	j.out_ea[1] = ea_of(spe.mem + spe.o_off);
	j.out_pitch = spe.out_pitch;
	j.label_in_ea = ea_of((const void *)spe_label(L_SPE_IN));
	uint32_t mid = ((fl.sh / 2) + 3) & ~3u;
	if (mid > fl.sh)
		mid = fl.sh;
	__typeof__(spe.job[0]) *e = &spe.job[(spe.head + spe.n) % SPE_INFLIGHT];
	memset(e, 0, sizeof *e);
	e->seq = seq;
	for (int i = 0; i < CRT_SPE_WORKERS; i++) {
		j.label_out_ea = ea_of((const void *)spe_label(L_SPE_OUT(i)));
		j.y0 = i ? mid : 0;
		j.y1 = i ? fl.sh : mid;
		e->id[i] = spukit_post(spe.w[i], CRT_SPE_JOB, &j, sizeof j);
		if (!e->id[i]) {
			/* Sin trabajo: el RSX no se queda esperando. */
			*spe_label(L_SPE_OUT(i)) = seq;
			e->done[i] = true;
		}
	}
	spe.n++;
	fl.spe_frame = true;
}

/* En draw_cable, en lugar de señal y decodifica: el RSX espera a los dos
 * SPEs y pasa su tabla a luz lineal, a la memoria local. */
static void spe_wait_rsx(int slot)
{
	const struct crt_cable_taps *c = &fl.ct;
	for (int i = 0; i < CRT_SPE_WORKERS; i++)
		rsxkit_post_label_wait(L_SPE_OUT(i), spe.seq);
	stamp(slot, STAMP_SPE_GO);
	rsxkit_post_pass p;
	for (int s = 0; s < (c->grid8 ? 2 : 1); s++) {
		pass_init(&p, P_SPE_LIN, s ? S_DEC_O : S_DEC_E, c->cols, fl.sh);
		p.in[0] = in_surface(s ? S_SPE_O : S_SPE_E, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
		p.inputs = 1;
		run(&p);
	}
}

/* Espera lo que esté en vuelo y suelta los SPEs (la memoria mapeada queda). */
static void spe_release(void)
{
	spe_collect(true);
	for (int i = 0; i < CRT_SPE_WORKERS; i++)
		if (spe.w[i]) {
			spukit_worker_stop(spe.w[i]);
			spe.w[i] = NULL;
		}
	spe.on = false;
}

/* El tamaño de la imagen del juego que ve el filtro; false si no se puede
 * filtrar. */
static bool wanted(int f, const struct salsa64_filter_src *src, uint32_t *sw, uint32_t *sh)
{
	if (f <= SALSA64_CRT_NONE || f >= SALSA64_CRTS || !src || src->factor < 1 || src->factor > 6)
		return false;
	*sw = src->region.w / src->factor;
	*sh = src->region.h / src->factor;
	return *sw >= 16 && *sh >= 16 && *sw <= 640 && *sh <= 480;
}

/* Si lo que falló fue con este filtro y esta imagen. */
static bool fail_same(int f, const struct salsa64_filter_src *src, uint32_t sw, uint32_t sh)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	return fl.fail.f == f && fl.fail.sw == sw && fl.fail.sh == sh && fl.fail.factor == src->factor &&
	       fl.fail.ow == sc->width && fl.fail.oh == sc->height && fl.fail.aspect == src->aspect;
}

static void fail_note(int f, const struct salsa64_filter_src *src, uint32_t sw, uint32_t sh)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	fl.failed = true;
	fl.fail.f = f;
	fl.fail.sw = sw;
	fl.fail.sh = sh;
	fl.fail.factor = src->factor;
	fl.fail.ow = sc->width;
	fl.fail.oh = sc->height;
	fl.fail.aspect = src->aspect;
}

uint32_t salsa64_filters_setup_ms(int f, const struct salsa64_filter_src *src)
{
	uint32_t sw, sh;
	if (!wanted(f, src, &sw, &sh) || (fl.failed && fail_same(f, src, sw, sh)) ||
	    (configured(f, sw, sh, src->aspect) && !fl.check))
		return 0;
	return fl.setup_ms[f] ? fl.setup_ms[f] : 100;
}

/* Si la imagen es la del último cuadro completo. */
static bool same_image(const struct salsa64_filter_src *src)
{
	return fl.last.valid && src->has_gen && fl.last.gen == src->gen && fl.last.offset == src->tex.offset &&
	       fl.last.location == src->tex.location && fl.last.factor == src->factor &&
	       !memcmp(&fl.last.region, &src->region, sizeof src->region);
}

bool salsa64_filters_draw(int f, const struct salsa64_filter_src *src, rsxkit_rect *out)
{
	uint32_t sw, sh;
	if (!wanted(f, src, &sw, &sh))
		return false;
	if (fl.failed) {
		if (fail_same(f, src, sw, sh))
			return false;
		fl.failed = false;
		salsa64_log("filtros: se vuelve a intentar «%s» (%ux%u, factor %u)", salsa64_crt_name(f), sw, sh,
			    src->factor);
	}
	collect();
	uint64_t t0 = sysGetSystemTime();
	bool fresh = !configured(f, sw, sh, src->aspect) || fl.check;
	if (!configure(f, sw, sh, src->aspect)) {
		fail_note(f, src, sw, sh);
		return false;
	}
	if (fl.last_filter != f) {
		fl.last_filter = f;
		memset(fl.ns, 0, sizeof fl.ns);
		fl.ns_reused = fl.ns_spe_wait = fl.ns_spe_trip = 0;
		fl.measured = fl.measured_reused = fl.measured_spe = fl.drawn = fl.reused = 0;
	}
	/* Una ranura de marcas libre (con más de SLOTS cuadros en vuelo, la
	 * más vieja se pierde para la medición). */
	int slot = (int)(fl.frame++ % SLOTS);
	fl.slot[slot].pending = false;
	bool reuse = !fresh && debug_surface < 0 && same_image(src);

	stamp(slot, 0);
	if (reuse) {
		final_pass(NULL);
		stamp(slot, SEGS);
		fl.reused++;
	} else {
		int rc = rsxkit_post_reduce(&src->tex, &src->region, src->factor, fl.surf[S_SRC]);
		if (rc != RSXKIT_OK) {
			salsa64_log("filtros: no se pudo reducir la imagen (%ux%u desde %ux%u, factor %u; %d): sin filtro "
				    "hasta que cambie la imagen o el filtro",
				    sw, sh, src->region.w, src->region.h, src->factor, rc);
			fail_note(f, src, sw, sh);
			return false;
		}
		stamp(slot, SEG_REDUCE + 1);
		spe_kick(slot);

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

		if (f == SALSA64_CRT_MASK)
			draw_mask(slot);
		else if (f == SALSA64_CRT_TRINITRON)
			draw_trinitron(slot);
		else
			draw_cable(slot);
		stamp(slot, SEG_FINAL + 1);
		fl.last.valid = src->has_gen;
		fl.last.gen = src->gen;
		fl.last.offset = src->tex.offset;
		fl.last.location = src->tex.location;
		fl.last.factor = src->factor;
		fl.last.region = src->region;
	}
	if (debug_surface >= 0 && fl.surf[debug_surface])
		copy_surface(debug_surface, debug_scale, NULL);
	fl.slot[slot].fence = rsxkit_fence();
	fl.slot[slot].filter = f;
	fl.slot[slot].reused = reuse;
	fl.slot[slot].spe = !reuse && fl.spe_frame;
	fl.spe_frame = false;
	fl.slot[slot].pending = true;
	fl.drawn++;
	fl.since_setup++;
	bool checked = fl.check && !fl.failed;
	if (checked)
		check_tables();
	if (!reuse && !fl.failed)
		capture();
	if (fresh) {
		uint32_t ms = (uint32_t)((sysGetSystemTime() - t0) / 1000);
		fl.setup_ms[f] = ms ? ms : 1;
		salsa64_log("filtros: «%s» listo en %u ms%s", salsa64_crt_name(f), ms, checked ? " (con la comprobación)" : "");
	}
	if (fl.failed)
		fail_note(f, src, sw, sh);  /* una pasada falló (run) */
	if (out)
		*out = (rsxkit_rect){ (int32_t)fl.rect[0], (int32_t)fl.rect[1], fl.rect[2], fl.rect[3] };
	return !fl.failed;
}

bool salsa64_filters_report(char *buf, size_t size)
{
	collect();
	if (!fl.drawn || fl.last_filter < 0) {
		fl.measured = fl.measured_reused = 0;
		return false;
	}
	const char *const *names = fl.last_filter == SALSA64_CRT_MASK		? seg_mask
				   : fl.last_filter == SALSA64_CRT_TRINITRON ? seg_trin
										: seg_cable;
	double k = fl.measured ? 1e-6 / fl.measured : 0.0, total = 0.0;
	for (int i = 0; i < SEGS; i++)
		total += fl.ns[i] * k;
	int passes = rsxkit_post_reduce_passes(0);
	int n = snprintf(buf, size, "filtro «%s» %ux%u desde %ux%u, reducir en %d pasada%s: %.2f ms del RSX por cuadro (",
			 salsa64_crt_name(fl.last_filter), fl.rect[2], fl.rect[3], fl.sw, fl.sh, passes, passes == 1 ? "" : "s",
			 total);
	for (int i = 0, first = 1; i < SEGS && n > 0 && (size_t)n < size; i++) {
		if (!names[i])
			continue;
		n += snprintf(buf + n, size - (size_t)n, "%s%s %.2f", first ? "" : ", ", names[i], fl.ns[i] * k);
		first = 0;
	}
	if (n > 0 && (size_t)n < size)
		n += snprintf(buf + n, size - (size_t)n, "; %u cuadros, %u medidos)", fl.drawn - fl.reused, fl.measured);
	spe_collect(false);
	if (fl.measured_spe && n > 0 && (size_t)n < size) {
		double hz = (double)spukit_tick_hz();
		n += snprintf(buf + n, size - (size_t)n,
			      "\nfiltro, señal y decodificación en %d SPE: el RSX esperó %.2f ms, ida y vuelta %.2f ms (%u medidos); "
			      "el SPE más lento, %.2f ms de trabajo y %.2f ms esperando la imagen (%u cuadros)",
			      CRT_SPE_WORKERS, fl.ns_spe_wait * 1e-6 / fl.measured_spe, fl.ns_spe_trip * 1e-6 / fl.measured_spe,
			      fl.measured_spe, spe.frames ? spe.work_ticks * 1e3 / hz / spe.frames : 0.0,
			      spe.frames ? spe.wait_ticks * 1e3 / hz / spe.frames : 0.0, spe.frames);
	}
	if (fl.reused && n > 0 && (size_t)n < size) {
		double fin = fl.measured_reused ? fl.ns_reused * 1e-6 / fl.measured_reused : 0.0;
		uint32_t m = fl.measured + fl.measured_reused;
		double avg = m ? (total * fl.measured + fin * fl.measured_reused) / m : 0.0;
		snprintf(buf + n, size - (size_t)n,
			 "\nfiltro, %u con la imagen repetida, solo la pasada final: %.2f ms (%u medidos); promedio %.2f ms por cuadro",
			 fl.reused, fin, fl.measured_reused, avg);
	}
	memset(fl.ns, 0, sizeof fl.ns);
	fl.ns_reused = fl.ns_spe_wait = fl.ns_spe_trip = 0;
	fl.measured = fl.measured_reused = fl.measured_spe = 0;
	fl.drawn = fl.reused = 0;
	spe.wait_ticks = spe.work_ticks = 0;
	spe.frames = 0;
	return true;
}
