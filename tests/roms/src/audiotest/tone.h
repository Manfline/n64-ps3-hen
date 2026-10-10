/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — audiotest: el tono que genera la ROM. Lo comparten la ROM y el
 * verificador del host (salsa64-run --audiotest-check), que repite el
 * generador y compara muestra por muestra lo que salió por el AI.
 *
 * Secuencia (a 32 000 Hz nominales, se repite): La 4 (440 Hz) 1 s en los
 * dos canales, para comprobar la afinación con un afinador; Do 5 0,5 s
 * solo a la izquierda, Mi 5 0,5 s solo a la derecha, Sol 5 y Do 6 0,5 s en
 * los dos, y 0,5 s de silencio. Cada nota sube y baja en 5 ms para no hacer
 * chasquidos. Seno de 256 puntos (32767·sen(2πi/256), redondeado) con
 * interpolación lineal; amplitud 0,5. Todo en enteros de 32 bits: la ROM
 * (MIPS) y el host dan lo mismo.
 */
#ifndef AUDIOTEST_TONE_H
#define AUDIOTEST_TONE_H

#include <stdint.h>

#define TONE_RATE    32000u   /* frecuencia nominal: AI_DACRATE la aproxima */
#define TONE_FRAMES  512u     /* muestras estéreo por búfer del AI */
#define TONE_AMP     16384
#define TONE_RAMP    160u     /* 5 ms */
#define TONE_RAMP_K  204      /* 32767 / TONE_RAMP */

/* Incremento de fase de 32 bits para una frecuencia en centésimas de Hz. */
#define TONE_INC(centihz) ((uint32_t)(((uint64_t)(centihz) << 32) / (100ull * TONE_RATE)))

struct tone_note {
	uint32_t inc;     /* 0: silencio */
	uint32_t frames;
	uint8_t left, right;
};

static const struct tone_note tone_seq[] = {
	{ TONE_INC(44000), 32000, 1, 1 },   /* La 4 */
	{ TONE_INC(52325), 16000, 1, 0 },   /* Do 5 */
	{ TONE_INC(65926), 16000, 0, 1 },   /* Mi 5 */
	{ TONE_INC(78399), 16000, 1, 1 },   /* Sol 5 */
	{ TONE_INC(104650), 16000, 1, 1 },  /* Do 6 */
	{ 0, 16000, 0, 0 },
};
#define TONE_NOTES (sizeof tone_seq / sizeof tone_seq[0])

static const int16_t tone_sine[256] = {
	     0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
	  6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
	 12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
	 18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
	 23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
	 27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
	 30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
	 32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
	 32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
	 32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
	 30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
	 27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
	 23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
	 18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
	 12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
	  6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
	     0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
	 -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
	-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
	-18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
	-23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
	-27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
	-30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
	-32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
	-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
	-32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
	-30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
	-27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
	-23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
	-18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
	-12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
	 -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

struct tone_gen {
	uint32_t note, pos, phase;
};

static inline void tone_next(struct tone_gen *g, int16_t *l, int16_t *r)
{
	const struct tone_note *n = &tone_seq[g->note];
	int32_t s = 0;
	if (n->inc) {
		uint32_t i = g->phase >> 24;
		int32_t frac = (int32_t)((g->phase >> 8) & 0xFFFF);
		int32_t a = tone_sine[i], b = tone_sine[(i + 1) & 255];
		s = a + (((b - a) * frac) >> 16);
		s = (s * TONE_AMP) >> 15;
		int32_t env = 32767;
		if (g->pos < TONE_RAMP)
			env = (int32_t)g->pos * TONE_RAMP_K;
		else if (n->frames - g->pos <= TONE_RAMP)
			env = (int32_t)(n->frames - 1 - g->pos) * TONE_RAMP_K;
		s = (s * env) >> 15;
		g->phase += n->inc;
	}
	*l = (int16_t)(n->left ? s : 0);
	*r = (int16_t)(n->right ? s : 0);
	if (++g->pos == n->frames) {
		g->pos = 0;
		g->phase = 0;
		g->note = g->note + 1 == TONE_NOTES ? 0 : g->note + 1;
	}
}

#endif
