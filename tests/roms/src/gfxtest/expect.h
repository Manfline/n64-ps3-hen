/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — gfxtest: escena de la ROM y píxeles que deben verse.
 *
 * La comparten la ROM (que dibuja con estos números) y la verificación del
 * frontend en la PS3 (src/frontend/selftest_gfx.c), que lee la pantalla del
 * RSX en estos puntos. Coordenadas en píxeles del framebuffer de la N64
 * (320×240); colores 0xRRGGBB.
 *
 * Objetos de prueba con proyección ortográfica (clip = vértice / 32): un
 * vértice (vx, vy) cae en el píxel (160 + 5·vx, 120 − 3,75·vy).
 *
 * Orientación: como en OpenGL (y libultra), el frente es antihorario con
 * la y hacia arriba.
 */
#ifndef SALSA64_GFXTEST_EXPECT_H
#define SALSA64_GFXTEST_EXPECT_H

#define GFXTEST_W 320
#define GFXTEST_H 240

/* Fondo: relleno RGBA5551 (4, 4, 12) → (33, 33, 99). */
#define GFXTEST_BG_5551 ((4u << 11) | (4u << 6) | (12u << 1) | 1u)
#define GFXTEST_BG      0x212163u

/* Mensajes por el ISViewer. */
#define GFXTEST_OK_TEXT    "gfxtest: OK"
#define GFXTEST_FAIL_TEXT  "gfxtest: FALLO"
/* Cuadros que dibuja antes de reportar. */
#define GFXTEST_FRAMES 120

struct gfxtest_point {
	unsigned short x, y;
	unsigned int rgb;
	const char *what;
};

/* Tolerancia por canal: el framebuffer de 16 bits de la N64 contra los 8
 * bits del RSX y el filtrado al escalar a la pantalla. */
#define GFXTEST_TOL 28

#define GFXTEST_POINTS                                                                  \
	{ 5, 5, GFXTEST_BG, "fondo (FILLRECT)" },                                       \
	{ 80, 100, 0xFF0000, "z-buffer: rojo delante del verde" },                     \
	{ 50, 65, 0xFF0000, "cuadro rojo" },                                            \
	{ 110, 130, 0x00FF00, "cuadro verde detrás" },                                  \
	{ 145, 35, 0xFFFF00, "textura RGBA16, texel (0,0)" },                          \
	{ 155, 35, 0x0000FF, "textura RGBA16, texel (1,0)" },                          \
	{ 155, 44, 0xFFFF00, "textura RGBA16, texel (1,1) (fila impar)" },             \
	{ 241, 31, 0xFFFF00, "TEXRECT I4 con alpha compare: borde" },                   \
	{ 256, 38, GFXTEST_BG, "TEXRECT I4 con alpha compare: hueco" },                \
	{ 20, 201, 0x000000, "TEXRECT COPY CI4 + TLUT: índice 0" },                    \
	{ 27, 201, 0x737373, "TEXRECT COPY CI4 + TLUT: índice 7" },                    \
	{ 35, 202, 0xF7F7F7, "TEXRECT COPY CI4 + TLUT: índice 15" },                   \
	{ 285, 165, 0x9090B1, "mezcla alfa 50 % sobre el fondo" },                    \
	{ 255, 75, 0x00FFFF, "G_CULL_BACK: antihorario se ve" },                        \
	{ 290, 75, GFXTEST_BG, "G_CULL_BACK: horario no se ve" },                      \
	{ 255, 120, GFXTEST_BG, "G_CULL_FRONT: antihorario no se ve" },                \
	{ 290, 120, 0xFF00FF, "G_CULL_FRONT: horario se ve" },                          \
	{ 80, 217, 0x800080, "2 ciclos, 2 texturas: lerp(rojo, azul, 0,5)" },          \
	{ 295, 15, 0xFF8000, "w = 1000: PRIM·TEXEL0_ALPHA" },

#endif
