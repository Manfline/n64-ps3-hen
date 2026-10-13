/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — filtros CRT originales del autor, portados de sus versiones de
 * RetroMac (máscara de sombra v1.3 y CRT con cable v1.1). Lo que se
 * calcula una vez en la CPU (al cambiar de resolución o de filtro) y los
 * presets: la geometría de la red de puntos y su promedio, la rejilla de
 * franjas del tubo y los pesos del filtro de la señal del cable. La cuenta
 * es la de los shaders originales; los fragment programs del RSX
 * (los .fcg de este directorio) hacen el resto por píxel.
 *
 * No depende del RSX: compila en el host (tests/unit/test_crt.c).
 */
#ifndef SALSA64_FRONTEND_FILTERS_CRT_PARAMS_H
#define SALSA64_FRONTEND_FILTERS_CRT_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

/* Monitor de máscara de sombra (puntos de fósforo redondos en delta). */
struct crt_mask_preset {
	float triads_per_pixel;  /* tríadas por píxel del juego en horizontal */
	float dot_size;          /* diámetro del punto / separación */
	float mask_strength;
	float keep_bright;       /* cuánto se abre la máscara en los puntos brillantes */
	float beam_min, beam_max;
	float h_blur;            /* desenfoque horizontal del haz (píxeles del juego) */
	float glow, glow_radius;
	float brightness, saturation;
	float black_level;       /* velo plano del vidrio */
	float gamma_in, gamma_out;
};
extern const struct crt_mask_preset crt_mask_monitor;  /* v1.3 */

/* El tubo del CRT con cable: un Trinitron chico (franjas verticales). */
struct crt_tube_preset {
	float triads_per_pixel, stripe_width, stripe_soft, mask_strength, keep_bright;
	float beam_min, beam_max;
	float glow, glow_radius;
	float brightness, saturation;
	float gamma_in, gamma_out;
};

/* Señal NTSC entre la consola y la tele. */
struct crt_cable_preset {
	struct crt_tube_preset tube;
	float cycles_per_line;  /* ciclos de subportadora en la línea visible */
	float line_phase;       /* fase que avanza en cada línea (ciclos) */
	float frame_phase;      /* fase que avanza en cada cuadro (0 = patrón quieto) */
	float luma_sigma;       /* filtro de luma, en píxeles de una línea de 256 */
	float chroma_tau;       /* filtro de color RC, en píxeles de una línea de 256 */
};
extern const struct crt_cable_preset crt_cable_tv13;  /* v1.1 */

enum crt_cable { CRT_CABLE_RGB, CRT_CABLE_SVIDEO, CRT_CABLE_COMPOSITE };

/* Rectángulo centrado con la forma de la imagen (fitRect de los shaders):
 * tamaño y origen en píxeles enteros. `px_aspect` es ancho/alto de un
 * píxel de la salida (1 en 720p y 1080p). */
void crt_fit_rect(uint32_t out_w, uint32_t out_h, float aspect, float px_aspect, uint32_t r[4]);

/* La espiral del resplandor: 12 puntos a sqrt(i + 0,5)/sqrt(12) del centro,
 * a 2,39996 rad uno del otro (en unidades del radio). */
extern const float crt_glow_spiral[12][2];

/* Puntos de la rejilla del resplandor por píxel del juego (1 o 2). */
uint32_t crt_glow_scale(uint32_t img_w, uint32_t src_w);

/* ---- Máscara de sombra ------------------------------------------------------ */

/* La red delta (filas cada H píxeles de pantalla, un punto cada D en cada
 * fila, las impares corridas D/2; el punto de la columna c y la fila r de
 * la tabla tiene su centro en ((c + (r mod 2)/2)·D, r·H) y color
 * (c − floor(r/2) + 2r) mod 3) y lo que de ella es igual en toda la
 * imagen. El patrón se repite cada 3D × 2H píxeles (una tesela). */
struct crt_mask_layout {
	float d, h, r;          /* separación, alto de fila y radio del punto (píxeles) */
	float avg;              /* promedio real de la máscara por canal */
	float peak;             /* pico del píxel más cubierto, sobre el promedio */
	float to_src[2];        /* píxeles del juego por píxel de pantalla */
	uint32_t tile_w, tile_h;
	uint32_t dots_w, dots_h;  /* tabla de puntos: una celda por punto */
	uint32_t mx, my;          /* margen de la tabla (columna c en c + mx) */
};
void crt_mask_layout(const struct crt_mask_preset *p, uint32_t img_w, uint32_t img_h, uint32_t src_w, uint32_t src_h,
		     struct crt_mask_layout *l);
/* Suma de la cobertura de un punto de radio R sobre la rejilla de píxeles
 * (centro en una esquina): con ella el promedio de la máscara es exacto. */
float crt_dot_cover_sum(float r);
/* Lo que la pasada «tesela» (crt_mask_tile.fcg) deja en el píxel (tx, ty)
 * de la tesela para el color ch: celda del punto más cercano (columna y
 * fila de la tabla de puntos, relativas a la tesela) y su cobertura. */
void crt_mask_tile_cell(const struct crt_mask_layout *l, float tx, float ty, int ch, float *col, float *row,
			float *cover);

/* ---- CRT con cable ------------------------------------------------------------ */

/* Franjas del tubo alineadas a píxeles enteros (tríada = múltiplo de 3). */
struct crt_grille {
	float pitch;
	float avg[3];   /* promedio real por canal sobre una tríada */
	float peak[3];  /* mix(1, 1/avg, fuerza de la máscara) */
};
void crt_grille(const struct crt_tube_preset *p, float px_per_src, struct crt_grille *g);
/* El valor de la franja en la columna x (píxeles de pantalla) para cada
 * canal (grille() de los shaders). */
void crt_grille_at(const struct crt_tube_preset *p, float x, float pitch, float out[3]);

#define CRT_TAPS_MAX 32
/* El filtro de la señal: cada muestra de una línea promedia la señal de la
 * consola sobre un tramo dt (4 muestras por ciclo de subportadora) y la
 * tele suma `count` muestras desde jlo con pesos fijos. Las tablas del RSX
 * guardan las muestras de cada línea en columnas: la columna m está en
 * x = x0 + m·dt (y, con grid8, otra tabla corrida dt/2 para el compuesto:
 * su luma lleva la subportadora y hace falta la mitad del paso para
 * interpolar). */
struct crt_cable_taps {
	float f;         /* ciclos de subportadora por píxel del juego */
	float dt;        /* entre muestras, en píxeles del juego */
	float hold;      /* tramo de la señal sostenida: min(dt, 1) */
	float ls, tau;   /* filtros de luma y de color en píxeles del juego */
	int jlo, count;
	float w[CRT_TAPS_MAX][3];  /* pesos ya normalizados: luma, color, color (RGB: los tres de luma) */
	float x0;        /* x de la columna 0 */
	uint32_t cols;   /* columnas de las tablas */
	bool yiq;        /* S-Video o compuesto: la señal va en luma y color */
	bool grid8;      /* compuesto: dos tablas, la segunda corrida dt/2 */
	float kc;        /* 1 en compuesto (luma y color comparten cable) */
};
void crt_cable_taps(const struct crt_cable_preset *p, enum crt_cable cable, uint32_t src_w,
		    struct crt_cable_taps *t);

#endif
