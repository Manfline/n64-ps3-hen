/* SPDX-License-Identifier: GPL-2.0-or-later */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - musyx.c                                         *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2013 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Salsa64 — HLE de MusyX (Factor 5): Hydro Thunder, Rush 2049, NBA Showtime,
 * Rogue Squadron, Resident Evil 2, Indiana Jones… Adaptado de src/musyx.c de
 * mupen64plus-rsp-hle (commit 8a7a472a, 2026-06-23) y de
 * `adpcm_predict_sample`/`adpcm_compute_residuals` de su src/audio.c, para
 * Salsa64 (2026-10-16).
 *
 * Una tarea de MusyX no es una lista de comandos: `data_ptr` apunta a una
 * lista de «subtramas» (SFD) y `data_size` es cuántas son. Cada SFD trae la
 * máscara de las voces activas, una tabla de 32 voces de 0x50 bytes y los
 * parámetros del efecto de eco (SFX); produce 192 muestras estéreo
 * intercaladas (un u32 L:R cada una) en la RDRAM. Por subtrama:
 *
 *   1. volúmenes base (4 canales: L, R, cc0, e50): se les suma la última
 *      muestra de cada voz activa y decaen un 3 %; con ellos se inicializan
 *      las cuatro subtramas internas;
 *   2. voces: cada una carga sus muestras (PCM16 o ADPCM, en dos tramos
 *      que se pegan), las cambia de tono (interpolación de 4 puntos con la
 *      tabla de RESAMPLE), les aplica cuatro envolventes y las suma a las
 *      cuatro subtramas; la lista de voces termina en la primera que trae
 *      la dirección de salida;
 *   3. SFX: hasta 8 tomas de un búfer circular de eco, mezcladas a L y R;
 *      el filtro FIR de 4 puntos de e50 vuelve al búfer;
 *   4. intercalado y escritura de la salida.
 *
 * El estado entre tareas vive en la RDRAM del juego (volúmenes base, cc0,
 * últimas muestras y el eco), no en aud_state: MusyX no usa la DMEM ni el
 * estado de los demás ABI. v1 y v2 comparten voces y SFX; v2 cambia la
 * cabecera del SFD, las subtramas iniciales, la mezcla del SFX y el
 * intercalado final (con ptr_10 sin hacer, como en el original).
 *
 * Cambios respecto al original, además de la E/S de la RDRAM (aud_load y
 * aud_store: una lectura por estructura, no por campo, porque en el SPE
 * cada una es un DMA):
 *   - redondeos del microcódigo real, medidos con `salsa64-run --lle=all
 *     --lle-check-audio` (docs/estado.md): cada producto del resampler, de
 *     las tomas del eco y del filtro FIR se redondea por separado
 *     (+0x4000 >> 15) antes de sumar, las envolventes también y los
 *     coeficientes del FIR (el original trunca); con eso las tareas son
 *     idénticas byte a byte;
 *   - la compilación de Rogue Squadron lleva la envolvente del canal 3 (eco)
 *     en octavos y la avanza de ocho en ocho muestras, como el microcódigo
 *     real (SALSA64_ABI_MUSYX1_RS; ver mix_voice_samples);
 *   - todo acceso a los búferes locales está acotado (el original confía
 *     en los datos del juego: más de 16 cuadros de ADPCM, tomas del eco con
 *     retardo mayor que el búfer, más de 32 voces…): un dato absurdo da una
 *     subtrama sin sonido y suma un error al resultado en vez de salirse
 *     del búfer;
 *   - libro de ADPCM de 16 predictores (el nibble del cuadro indexa hasta
 *     16; el original lee más allá de la tabla de 8);
 *   - decaimiento de los volúmenes base en 64 bits y sumas sin
 *     desbordamiento con signo;
 *   - la tabla de RESAMPLE no es la del original: se toma del segmento de
 *     datos del microcódigo del propio juego (aud_find_lut, alist.c) o, si no
 *     está, es la de lut.c.
 *
 * Presupuesto en el SPE: el estado propio son los 520 bytes de la tabla de
 * interpolación (aud_state.lut); la pila usa unos 5 KB (cuatro
 * subtramas, 520 muestras, libro y búfer de ADPCM, subtrama del eco, los
 * 2 KB del segmento de datos mientras se busca la tabla).
 */
#include <string.h>

#include "rsp/audio/hle.h"

enum {
	SUBFRAME_SIZE = 192,
	MAX_VOICES = 32,
	SAMPLE_BUFFER_SIZE = 0x200,
	SAMPLE_GUARD = 8,   /* ceros tras el búfer: la interpolación lee 4 puntos */
	ADPCM_BUFFER = 320, /* 16 cuadros de 20 bytes */
	ADPCM_SLACK = 64,
	TAPS_MAX = 8,
};

enum {
	SFD_SFX_INDEX = 0x2,
	SFD_VOICE_BITMASK = 0x4,
	SFD_STATE_PTR = 0x8,
	SFD_SFX_PTR = 0xc,
	SFD_VOICES = 0x10,
	SFD_HEADER = 0x10,

	/* v2 */
	SFD2_10_PTR = 0x10,
	SFD2_14_BITMASK = 0x14,
	SFD2_15_BITMASK = 0x15,
	SFD2_16_BITMASK = 0x16,
	SFD2_18_PTR = 0x18,
	SFD2_1C_PTR = 0x1c,
	SFD2_20_PTR = 0x20,
	SFD2_24_PTR = 0x24,
	SFD2_VOICES = 0x28,
	SFD2_HEADER = 0x28,
};

enum {
	VOICE_ENV_BEGIN = 0x00,
	VOICE_ENV_STEP = 0x10,
	VOICE_PITCH_Q16 = 0x20,
	VOICE_PITCH_SHIFT = 0x22,
	VOICE_CATSRC_0 = 0x24,
	VOICE_CATSRC_1 = 0x30,
	VOICE_ADPCM_FRAMES = 0x3c,
	VOICE_SKIP_SAMPLES = 0x3e,
	VOICE_U16_40 = 0x40,  /* PCM16 */
	VOICE_U16_42 = 0x42,
	VOICE_ADPCM_TABLE_PTR = 0x40,  /* ADPCM */
	VOICE_INTERLEAVED_PTR = 0x44,
	VOICE_END_POINT = 0x48,
	VOICE_RESTART_POINT = 0x4a,
	VOICE_U16_4C = 0x4c,
	VOICE_U16_4E = 0x4e,
	VOICE_SIZE = 0x50,
};

enum { CATSRC_PTR1 = 0x00, CATSRC_PTR2 = 0x04, CATSRC_SIZE1 = 0x08, CATSRC_SIZE2 = 0x0a };

enum {
	STATE_LAST_SAMPLE = 0x0,
	STATE_BASE_VOL = 0x100,
	STATE_CC0 = 0x110,
	STATE_740_LAST4_V1 = 0x290,
	STATE_740_LAST4_V2 = 0x110,
};

enum {
	SFX_CBUFFER_PTR = 0x00,
	SFX_CBUFFER_LENGTH = 0x04,
	SFX_TAP_COUNT = 0x08,
	SFX_FIR4_HGAIN = 0x0a,
	SFX_TAP_DELAYS = 0x0c,
	SFX_TAP_GAINS = 0x2c,
	SFX_U16_3C = 0x3c,
	SFX_U16_3E = 0x3e,
	SFX_FIR4_HCOEFFS = 0x40,
	SFX_SIZE = 0x48,
};

/* Subtramas internas, volúmenes base y últimas 4 muestras del eco. */
struct mx {
	int16_t left[SUBFRAME_SIZE];
	int16_t right[SUBFRAME_SIZE];
	int16_t cc0[SUBFRAME_SIZE];
	int16_t e50[SUBFRAME_SIZE];
	int32_t base_vol[4];
	int16_t last4[4];
	bool early;  /* compilación de Rogue Squadron: paso del canal 3 en octavos */
};

/* Una voz, ya leída de la RDRAM. */
struct catsrc {
	uint32_t ptr1, ptr2;
	uint16_t size1, size2;
};

struct voice {
	uint32_t env[4], env_step[4];
	uint16_t pitch_q16, pitch_shift;
	struct catsrc cat[2];
	uint8_t adpcm_frames[2], skip[2];
	uint16_t u40, u42;
	uint32_t table_ptr;
	uint32_t out_ptr;
	uint16_t end_point, restart_point, u4c, u4e;
};

/* ---- RDRAM ----------------------------------------------------------------- */

static void mx_ld16(struct aud_state *st, int16_t *dst, uint32_t addr, uint32_t n)
{
	aud_load(st, dst, addr, 2 * n);
#if !SALSA64_HOST_BIG_ENDIAN
	for (uint32_t i = 0; i < n; i++)
		dst[i] = (int16_t)__builtin_bswap16((uint16_t)dst[i]);
#endif
}

/* n ≤ SUBFRAME_SIZE */
static void mx_st16(struct aud_state *st, uint32_t addr, const int16_t *src, uint32_t n)
{
#if SALSA64_HOST_BIG_ENDIAN
	aud_store(st, addr, src, 2 * n);
#else
	uint8_t buf[2 * SUBFRAME_SIZE];
	for (uint32_t i = 0; i < n; i++)
		aud_st16(buf + 2 * i, src[i]);
	aud_store(st, addr, buf, 2 * n);
#endif
}

static uint32_t add32(int32_t a, int32_t b)
{
	return (uint32_t)a + (uint32_t)b;
}

/* ---- Volúmenes base ----------------------------------------------------------- */

static void load_base_vol(struct aud_state *st, int32_t *base_vol, uint32_t address)
{
	uint8_t b[16];
	aud_load(st, b, address, sizeof b);
	for (unsigned k = 0; k < 4; k++)
		base_vol[k] = (int32_t)(((uint32_t)salsa64_rd16(b + 2 * k) << 16) | salsa64_rd16(b + 8 + 2 * k));
}

static void save_base_vol(struct aud_state *st, const int32_t *base_vol, uint32_t address)
{
	uint8_t b[16];
	for (unsigned k = 0; k < 4; k++) {
		salsa64_wr16(b + 2 * k, (uint16_t)((uint32_t)base_vol[k] >> 16));
		salsa64_wr16(b + 8 + 2 * k, (uint16_t)base_vol[k]);
	}
	aud_store(st, address, b, sizeof b);
}

/* Suma la última muestra de cada voz activa (8 bytes por voz) y, en v2, las
 * de otras 4 fuentes; después decae un 3 %. */
static void update_base_vol(struct aud_state *st, int32_t *base_vol, uint32_t voice_mask,
			    uint32_t last_sample_ptr, uint8_t mask_15, uint32_t ptr_24)
{
	int16_t v[4 * MAX_VOICES];

	if (voice_mask) {
		unsigned top = 32 - (unsigned)__builtin_clz(voice_mask);  /* hasta la última voz activa */
		mx_ld16(st, v, last_sample_ptr, 4 * top);
		for (unsigned i = 0; i < top; i++) {
			if (!(voice_mask & (1u << i)))
				continue;
			for (unsigned k = 0; k < 4; k++)
				base_vol[k] = (int32_t)add32(base_vol[k], v[4 * i + k]);
		}
	}
	if (mask_15) {
		mx_ld16(st, v, ptr_24, 16);
		for (unsigned i = 0; i < 4; i++) {
			if (!(mask_15 & (1u << i)))
				continue;
			for (unsigned k = 0; k < 4; k++)
				base_vol[k] = (int32_t)add32(base_vol[k], v[4 * i + k]);
		}
	}
	for (unsigned k = 0; k < 4; k++)
		base_vol[k] = (int32_t)(((int64_t)base_vol[k] * 0x0000f850) >> 16);
}

static void init_subframes_v1(struct mx *m)
{
	int16_t base_cc0 = aud_clamp16(m->base_vol[2]);
	int16_t base_e50 = aud_clamp16(m->base_vol[3]);

	for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
		m->e50[i] = base_e50;
		m->left[i] = aud_clamp16(m->cc0[i] + base_cc0);
		m->right[i] = aud_clamp16(-m->cc0[i] - base_cc0);
		m->cc0[i] = 0;
	}
}

static void init_subframes_v2(struct mx *m)
{
	int16_t values[4];
	for (unsigned k = 0; k < 4; k++)
		values[k] = aud_clamp16(m->base_vol[k]);
	for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
		m->left[i] = values[0];
		m->right[i] = values[1];
		m->cc0[i] = values[2];
		m->e50[i] = values[3];
	}
}

/* ---- Voces ------------------------------------------------------------------- */

static void voice_load(struct aud_state *st, struct voice *v, uint32_t addr)
{
	uint8_t b[VOICE_SIZE];
	aud_load(st, b, addr, sizeof b);
	for (unsigned k = 0; k < 4; k++) {
		v->env[k] = salsa64_rd32(b + VOICE_ENV_BEGIN + 4 * k);
		v->env_step[k] = salsa64_rd32(b + VOICE_ENV_STEP + 4 * k);
	}
	v->pitch_q16 = salsa64_rd16(b + VOICE_PITCH_Q16);
	v->pitch_shift = salsa64_rd16(b + VOICE_PITCH_SHIFT);
	for (unsigned c = 0; c < 2; c++) {
		const uint8_t *p = b + (c ? VOICE_CATSRC_1 : VOICE_CATSRC_0);
		v->cat[c].ptr1 = salsa64_rd32(p + CATSRC_PTR1);
		v->cat[c].ptr2 = salsa64_rd32(p + CATSRC_PTR2);
		v->cat[c].size1 = salsa64_rd16(p + CATSRC_SIZE1);
		v->cat[c].size2 = salsa64_rd16(p + CATSRC_SIZE2);
		v->adpcm_frames[c] = b[VOICE_ADPCM_FRAMES + c];
		v->skip[c] = b[VOICE_SKIP_SAMPLES + c];
	}
	v->u40 = salsa64_rd16(b + VOICE_U16_40);
	v->u42 = salsa64_rd16(b + VOICE_U16_42);
	v->table_ptr = salsa64_rd32(b + VOICE_ADPCM_TABLE_PTR);
	v->out_ptr = salsa64_rd32(b + VOICE_INTERLEAVED_PTR);
	v->end_point = salsa64_rd16(b + VOICE_END_POINT);
	v->restart_point = salsa64_rd16(b + VOICE_RESTART_POINT);
	v->u4c = salsa64_rd16(b + VOICE_U16_4C);
	v->u4e = salsa64_rd16(b + VOICE_U16_4E);
}

/* Los dos tramos de una fuente, pegados en `dst` (bytes o muestras de 16
 * bits). `cap` es lo que cabe; lo que sobra no se lee. */
static void dma_cat8(struct aud_state *st, uint8_t *dst, uint32_t cap, const struct catsrc *c)
{
	uint32_t n1 = c->size1 < cap ? c->size1 : cap;
	if (n1 < c->size1)
		st->res->errors++;
	aud_load(st, dst, c->ptr1, n1);
	if (c->size2 == 0)
		return;
	uint32_t n2 = c->size2 < cap - n1 ? c->size2 : cap - n1;
	if (n2 < c->size2)
		st->res->errors++;
	aud_load(st, dst + n1, c->ptr2, n2);
}

static void dma_cat16(struct aud_state *st, int16_t *dst, uint32_t cap, const struct catsrc *c)
{
	uint32_t n1 = (uint32_t)(c->size1 >> 1) < cap ? (uint32_t)(c->size1 >> 1) : cap;
	if (n1 < (uint32_t)(c->size1 >> 1))
		st->res->errors++;
	mx_ld16(st, dst, c->ptr1, n1);
	if (c->size2 == 0)
		return;
	uint32_t n2 = (uint32_t)(c->size2 >> 1) < cap - n1 ? (uint32_t)(c->size2 >> 1) : cap - n1;
	if (n2 < (uint32_t)(c->size2 >> 1))
		st->res->errors++;
	mx_ld16(st, dst + n1, c->ptr2, n2);
}

static int16_t predict_sample(uint8_t byte, uint8_t mask, unsigned lshift, unsigned rshift)
{
	int16_t sample = (int16_t)((uint16_t)(byte & mask) << lshift);
	return (int16_t)(sample >> rshift);
}

static uint32_t rdot(size_t n, const int16_t *x, const int16_t *y)
{
	uint32_t accu = 0;
	y += n;
	while (n--)
		accu += (uint32_t)(*x++ * *--y);
	return accu;
}

static void compute_residuals(int16_t *dst, const int16_t *src, const int16_t *cb_entry, const int16_t *last_samples,
			      size_t count)
{
	const int16_t *book1 = cb_entry, *book2 = cb_entry + 8;
	int16_t l1 = last_samples[0], l2 = last_samples[1];
	for (size_t i = 0; i < count; i++) {
		/* Sumas que pueden desbordar con datos absurdos: módulo 2^32. */
		uint32_t accu = (uint32_t)src[i] << 11;
		accu += (uint32_t)(book1[i] * l1) + (uint32_t)(book2[i] * l2) + rdot(i, book2, src);
		dst[i] = aud_clamp16((int32_t)accu >> 11);
	}
}

/* Cuadros de 32 muestras de ADPCM, en el orden de 20 bytes de MusyX: 4 de
 * muestras iniciales, 16 de residuos con la escala y el predictor en el
 * primero, y un salto cada dos cuadros. `src` trae al menos ADPCM_BUFFER +
 * ADPCM_SLACK bytes. */
static void adpcm_decode_frames(int16_t *dst, const uint8_t *src, const int16_t *table, unsigned count,
				unsigned skip_samples)
{
	int16_t frame[32];
	const uint8_t *nibbles = src + 8;
	bool jump_gap = false;

	if (skip_samples >= 32) {
		jump_gap = true;
		nibbles += 16;
		src += 4;
	}

	for (unsigned i = 0; i < count; i++) {
		uint8_t c2 = nibbles[0];
		const int16_t *book = table + (c2 & 0xf0);
		unsigned rshift = c2 & 0x0f;

		frame[0] = (int16_t)((src[0] << 8) | src[1]);
		frame[1] = (int16_t)((src[2] << 8) | src[3]);
		for (unsigned k = 1; k < 16; k++) {
			uint8_t byte = nibbles[k];
			frame[2 * k] = predict_sample(byte, 0xf0, 8, rshift);
			frame[2 * k + 1] = predict_sample(byte, 0x0f, 12, rshift);
		}

		memcpy(dst, frame, 2 * sizeof frame[0]);
		compute_residuals(dst + 2, frame + 2, book, dst, 6);
		compute_residuals(dst + 8, frame + 8, book, dst + 6, 8);
		compute_residuals(dst + 16, frame + 16, book, dst + 14, 8);
		compute_residuals(dst + 24, frame + 24, book, dst + 22, 8);

		if (jump_gap) {
			nibbles += 8;
			src += 32;
		}
		jump_gap = !jump_gap;
		nibbles += 16;
		src += 4;
		dst += 32;
	}
}

/* Carga las muestras de una voz en `samples` (SAMPLE_BUFFER_SIZE + guarda):
 * `segbase` es donde empieza el tramo 0 y `offset` cuántas se saltan. */
static void load_samples(struct aud_state *st, const struct voice *v, int16_t *samples, int16_t *table,
			 unsigned *segbase, unsigned *offset)
{
	memset(samples + SAMPLE_BUFFER_SIZE, 0, SAMPLE_GUARD * sizeof samples[0]);

	if (v->adpcm_frames[0] == 0) {
		/* PCM16 */
		unsigned count = (unsigned)aud_align(v->u40 + v->skip[0], 4);
		if (count > SAMPLE_BUFFER_SIZE) {
			st->res->errors++;
			count = SAMPLE_BUFFER_SIZE;
		}
		*segbase = SAMPLE_BUFFER_SIZE - count;
		*offset = v->skip[0];
		memset(samples, 0, SAMPLE_BUFFER_SIZE * sizeof samples[0]);
		dma_cat16(st, samples + *segbase, SAMPLE_BUFFER_SIZE - *segbase, &v->cat[0]);
		if (v->u42 != 0)
			dma_cat16(st, samples, SAMPLE_BUFFER_SIZE, &v->cat[1]);
		return;
	}

	/* ADPCM: las muestras decodificadas no pasan de 0x400 bytes (cuadros de
	 * 20 bytes → 64 bytes de muestras). */
	uint8_t buffer[ADPCM_BUFFER + ADPCM_SLACK];
	unsigned n0 = v->adpcm_frames[0], n1 = v->adpcm_frames[1];
	if (n0 > 16 || n1 > 16) {
		st->res->errors++;
		n0 = n0 > 16 ? 16 : n0;
		n1 = n1 > 16 ? 16 : n1;
	}

	mx_ld16(st, table, v->table_ptr, 128);

	*segbase = SAMPLE_BUFFER_SIZE - (n0 << 5);
	*offset = v->skip[0] & 0x1f;
	memset(samples, 0, SAMPLE_BUFFER_SIZE * sizeof samples[0]);

	memset(buffer, 0, sizeof buffer);
	dma_cat8(st, buffer, ADPCM_BUFFER, &v->cat[0]);
	adpcm_decode_frames(samples + *segbase, buffer, table, n0, v->skip[0]);

	if (n1 != 0) {
		memset(buffer, 0, sizeof buffer);
		dma_cat8(st, buffer, ADPCM_BUFFER, &v->cat[1]);
		adpcm_decode_frames(samples, buffer, table, n1, v->skip[1]);
	}
}

/* Interpolación de 4 puntos: cada producto se redondea (+0x4000 >> 15) por
 * separado, se suman los cuatro y solo el resultado se satura (el llamador).
 * Así el HLE da lo mismo que el microcódigo real byte a byte (Hydro
 * Thunder, Rush 2049, NBA Showtime: docs/estado.md); sumar primero y
 * redondear una vez, o redondear hacia abajo cada término como el original,
 * difiere en ±1; saturar en cada suma, en mucho más. */
static int32_t dot4(const int16_t *x, const int16_t *y)
{
	return ((x[0] * y[0] + 0x4000) >> 15) + ((x[1] * y[1] + 0x4000) >> 15) + ((x[2] * y[2] + 0x4000) >> 15) +
	       ((x[3] * y[3] + 0x4000) >> 15);
}

/* Cambia el tono, aplica las cuatro envolventes y suma a las subtramas. La
 * última muestra de cada canal queda en la RDRAM (last_sample_ptr). */
static void mix_voice_samples(struct aud_state *st, struct mx *m, const struct voice *v, const int16_t *samples,
			      unsigned segbase, unsigned offset, uint32_t last_sample_ptr)
{
	const int16_t *const lut_base = aud_lut(st);
	int32_t pos = (int32_t)(segbase + offset + v->u4e);
	const int32_t end = (int32_t)(segbase + v->end_point);
	const int32_t restart = (v->restart_point & 0x7fff) + ((v->restart_point & 0x8000) ? 0 : (int32_t)segbase);
	const int32_t pos_max = SAMPLE_BUFFER_SIZE + SAMPLE_GUARD - 4;

	uint32_t pitch_accu = v->pitch_q16;
	const uint32_t pitch_step = (uint32_t)v->pitch_shift << 4;

	uint32_t env[4], env_step[4];
	int16_t *dst[4] = { m->left, m->right, m->cc0, m->e50 };
	int16_t v4[4] = { 0, 0, 0, 0 };
	bool clipped = false;

	for (unsigned k = 0; k < 4; k++) {
		env[k] = v->env[k];
		env_step[k] = v->env_step[k];
	}
	/* La compilación de Rogue Squadron lleva la envolvente del canal 3 (envío
	 * al eco) en octavos (13.3 bits) y la avanza de ocho en ocho muestras:
	 * la base del bloque sube 8 × (paso >> 13) y la muestra j del bloque suma
	 * (j × paso) >> 13. Medido contra el microcódigo real con las rampas de
	 * ese canal (docs/estado.md): 36 096 de 36 096 muestras idénticas; con el
	 * paso entero en 32 bits (como los canales 0–2 y las demás
	 * compilaciones) coinciden 11 409. */
	const int32_t early_base = (int32_t)v->env[3] >> 13;
	const int32_t early_block = ((int32_t)v->env_step[3] >> 13) * 8;

	for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
		const int16_t *lut = lut_base + ((pitch_accu & 0xfc00) >> 8);

		pos += (int32_t)(pitch_accu >> 16);
		pitch_accu &= 0xffff;
		pitch_accu += pitch_step;

		int32_t dist = pos - end;
		if (dist >= 0)
			pos = restart + dist;
		if (pos < 0 || pos > pos_max) {
			pos = pos < 0 ? 0 : pos_max;
			clipped = true;
		}

		int16_t s = aud_clamp16(dot4(samples + pos, lut));

		int32_t gain[4] = { (int32_t)env[0] >> 16, (int32_t)env[1] >> 16, (int32_t)env[2] >> 16,
				    (int32_t)env[3] >> 16 };
		if (m->early)
			gain[3] = (early_base + (int32_t)(i >> 3) * early_block +
				   (int32_t)(((int64_t)(i & 7) * (int32_t)env_step[3]) >> 13)) >> 3;

		for (unsigned k = 0; k < 4; k++) {
			int32_t accu = (s * gain[k] + 0x4000) >> 15;
			v4[k] = aud_clamp16(accu);
			dst[k][i] = aud_clamp16(accu + dst[k][i]);
			env[k] += env_step[k];
		}
	}
	if (clipped)
		st->res->errors++;

	mx_st16(st, last_sample_ptr, v4, 4);
}

/* Mezcla las voces activas y devuelve la dirección de salida del SFD, la de
 * la primera voz con una. */
static uint32_t voice_stage(struct aud_state *st, struct mx *m, uint32_t voice_ptr, uint32_t last_sample_ptr)
{
	int16_t samples[SAMPLE_BUFFER_SIZE + SAMPLE_GUARD];
	int16_t table[256];
	struct voice v;
	uint32_t out_ptr = 0;

	/* Se salta la etapa si la primera voz no trae muestras. */
	voice_load(st, &v, voice_ptr);
	if (v.cat[0].size1 == 0)
		return v.out_ptr;

	memset(table, 0, sizeof table);  /* la mitad alta no se carga: 16 predictores */
	for (unsigned i = 0;; i++) {
		unsigned segbase, offset;

		load_samples(st, &v, samples, table, &segbase, &offset);
		mix_voice_samples(st, m, &v, samples, segbase, offset, last_sample_ptr + i * 8);
		st->res->commands++;

		out_ptr = v.out_ptr;
		if (out_ptr != 0)
			break;
		if (i + 1 >= MAX_VOICES) {
			st->res->errors++;
			break;
		}
		voice_ptr += VOICE_SIZE;
		voice_load(st, &v, voice_ptr);
	}
	return out_ptr;
}

/* ---- Efecto de eco (SFX) --------------------------------------------------------- */

/* Como en el resampler, los productos se redondean uno a uno (+0x4000 >>
 * 15) y se suman: lo comprobado contra el microcódigo real (docs/estado.md)
 * en los cuatro coeficientes del filtro FIR del eco y en cada toma. */
static void mix_samples(int16_t *y, int16_t x, int16_t hgain)
{
	*y = aud_clamp16(*y + ((x * hgain + 0x4000) >> 15));
}

static void mix_fir4(int16_t *y, const int16_t *x, int16_t hgain, const int16_t *hcoeffs)
{
	int32_t h[4];
	for (unsigned k = 0; k < 4; k++)
		h[k] = (hgain * hcoeffs[k] + 0x4000) >> 15;
	for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
		int32_t s = ((h[0] * x[i] + 0x4000) >> 15) + ((h[1] * x[i + 1] + 0x4000) >> 15) +
			    ((h[2] * x[i + 2] + 0x4000) >> 15) + ((h[3] * x[i + 3] + 0x4000) >> 15);
		y[i] = aud_clamp16(y[i] + s);
	}
}

static void mix_sfx_v1(struct mx *m, const int16_t *subframe, const uint16_t *gains)
{
	(void)gains;
	for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
		int16_t v = subframe[i];
		m->left[i] = aud_clamp16(m->left[i] + v);
		m->right[i] = aud_clamp16(m->right[i] + v);
	}
}

static void mix_sfx_v2(struct mx *m, const int16_t *subframe, const uint16_t *gains)
{
	for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
		int16_t v = subframe[i];
		int16_t v1 = (int16_t)((int32_t)(v * gains[0]) >> 16);
		int16_t v2 = (int16_t)((int32_t)(v * gains[1]) >> 16);
		m->left[i] = aud_clamp16(m->left[i] + v1);
		m->right[i] = aud_clamp16(m->right[i] + v1);
		m->cc0[i] = aud_clamp16(m->cc0[i] + v2);
	}
}

static void sfx_stage(struct aud_state *st, struct mx *m, bool v2, uint32_t sfx_ptr, uint16_t idx)
{
	int16_t buffer[SUBFRAME_SIZE + 4];
	int16_t *subframe = buffer + 4;
	int16_t delayed[SUBFRAME_SIZE];
	uint8_t b[SFX_SIZE];
	uint32_t tap_delays[TAPS_MAX];
	int16_t tap_gains[TAPS_MAX], fir4_hcoeffs[4];
	uint16_t sfx_gains[2];
	const uint32_t pos = (uint32_t)idx * SUBFRAME_SIZE;

	if (sfx_ptr == 0)
		return;

	aud_load(st, b, sfx_ptr, sizeof b);
	const uint32_t cbuffer_ptr = salsa64_rd32(b + SFX_CBUFFER_PTR);
	const uint32_t cbuffer_length = salsa64_rd32(b + SFX_CBUFFER_LENGTH);
	unsigned tap_count = salsa64_rd16(b + SFX_TAP_COUNT);
	const int16_t fir4_hgain = (int16_t)salsa64_rd16(b + SFX_FIR4_HGAIN);
	for (unsigned k = 0; k < TAPS_MAX; k++) {
		tap_delays[k] = salsa64_rd32(b + SFX_TAP_DELAYS + 4 * k);
		tap_gains[k] = (int16_t)salsa64_rd16(b + SFX_TAP_GAINS + 2 * k);
	}
	for (unsigned k = 0; k < 4; k++)
		fir4_hcoeffs[k] = (int16_t)salsa64_rd16(b + SFX_FIR4_HCOEFFS + 2 * k);
	sfx_gains[0] = salsa64_rd16(b + SFX_U16_3C);
	sfx_gains[1] = salsa64_rd16(b + SFX_U16_3E);

	if (tap_count > TAPS_MAX) {
		st->res->errors++;
		tap_count = TAPS_MAX;
	}

	/* Hasta 8 subtramas retardadas del búfer circular. */
	memset(subframe, 0, SUBFRAME_SIZE * sizeof subframe[0]);
	for (unsigned i = 0; i < tap_count; i++) {
		uint32_t dpos = pos - tap_delays[i];
		if ((int32_t)dpos <= 0)
			dpos += cbuffer_length;
		if ((int32_t)dpos < 0 || dpos > cbuffer_length) {
			st->res->errors++;  /* retardo mayor que el búfer */
			continue;
		}
		uint32_t dlength = SUBFRAME_SIZE;
		if ((uint64_t)dpos + SUBFRAME_SIZE > cbuffer_length) {
			dlength = cbuffer_length - dpos;
			mx_ld16(st, delayed + dlength, cbuffer_ptr, SUBFRAME_SIZE - dlength);
		}
		mx_ld16(st, delayed, cbuffer_ptr + dpos * 2, dlength);

		for (unsigned k = 0; k < SUBFRAME_SIZE; k++)
			mix_samples(&subframe[k], delayed[k], tap_gains[i]);
	}

	(v2 ? mix_sfx_v2 : mix_sfx_v1)(m, subframe, sfx_gains);

	/* FIR de 4 puntos sobre e50 y escritura al búfer. */
	memcpy(buffer, m->last4, 4 * sizeof buffer[0]);
	memcpy(m->last4, subframe + SUBFRAME_SIZE - 4, 4 * sizeof buffer[0]);
	mix_fir4(m->e50, buffer + 1, fir4_hgain, fir4_hcoeffs);
	mx_st16(st, cbuffer_ptr + pos * 2, m->e50, SUBFRAME_SIZE);
}

/* ---- Salida ------------------------------------------------------------------------ */

/* L:R de 16 bits cada uno, 192 pares. */
static void store_interleaved(struct aud_state *st, uint32_t output_ptr, const int16_t *l, const int16_t *r)
{
	uint8_t out[4 * SUBFRAME_SIZE];
	for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
		salsa64_wr16(out + 4 * i, (uint16_t)l[i]);
		salsa64_wr16(out + 4 * i + 2, (uint16_t)r[i]);
	}
	aud_store(st, output_ptr, out, sizeof out);
}

static void interleave_stage_v1(struct aud_state *st, struct mx *m, uint32_t output_ptr)
{
	int16_t base_left = aud_clamp16(m->base_vol[0]);
	int16_t base_right = aud_clamp16(m->base_vol[1]);
	int16_t l[SUBFRAME_SIZE], r[SUBFRAME_SIZE];

	for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
		l[i] = aud_clamp16(m->left[i] + base_left);
		r[i] = aud_clamp16(m->right[i] + base_right);
	}
	store_interleaved(st, output_ptr, l, r);
}

static void interleave_stage_v2(struct aud_state *st, struct mx *m, uint16_t mask_16, uint32_t ptr_18,
				uint32_t ptr_1c, uint32_t output_ptr)
{
	int16_t subframe[SUBFRAME_SIZE];
	int16_t src[3 * SUBFRAME_SIZE];
	uint8_t g[8];

	/* L_total y R_total, y la subtrama de ptr_1c. */
	memset(subframe, 0, sizeof subframe);
	mx_ld16(st, m->left, ptr_1c, SUBFRAME_SIZE);
	for (unsigned i = 0; i < SUBFRAME_SIZE; i++)
		m->right[i] = aud_clamp16(-m->left[i]);

	for (unsigned k = 0; k < 8; k++, ptr_18 += 8) {
		if (!(mask_16 & (1u << k)))
			continue;
		aud_load(st, g, ptr_18, sizeof g);
		uint32_t address = salsa64_rd32(g);
		int16_t hgain = (int16_t)salsa64_rd16(g + 4);

		mx_ld16(st, src, address, 3 * SUBFRAME_SIZE);
		for (unsigned i = 0; i < SUBFRAME_SIZE; i++) {
			mix_samples(&m->left[i], src[i], hgain);
			mix_samples(&m->right[i], src[SUBFRAME_SIZE + i], hgain);
			mix_samples(&subframe[i], src[2 * SUBFRAME_SIZE + i], hgain);
		}
	}

	store_interleaved(st, output_ptr, m->left, m->right);
	mx_st16(st, ptr_1c, subframe, SUBFRAME_SIZE);
}

/* ---- Tareas ---------------------------------------------------------------------------- */

static void musyx_v1_task(struct aud_state *st, uint32_t sfd_ptr, uint32_t sfd_count, bool early)
{
	struct mx m;

	m.early = early;
	uint8_t sfd[SFD_HEADER];

	aud_load(st, sfd, sfd_ptr, sizeof sfd);
	uint32_t state_ptr = salsa64_rd32(sfd + SFD_STATE_PTR);

	load_base_vol(st, m.base_vol, state_ptr + STATE_BASE_VOL);
	mx_ld16(st, m.cc0, state_ptr + STATE_CC0, SUBFRAME_SIZE);
	mx_ld16(st, m.last4, state_ptr + STATE_740_LAST4_V1, 4);

	for (;;) {
		uint16_t sfx_index = salsa64_rd16(sfd + SFD_SFX_INDEX);
		uint32_t voice_mask = salsa64_rd32(sfd + SFD_VOICE_BITMASK);
		uint32_t sfx_ptr = salsa64_rd32(sfd + SFD_SFX_PTR);
		uint32_t last_sample_ptr = state_ptr + STATE_LAST_SAMPLE;

		update_base_vol(st, m.base_vol, voice_mask, last_sample_ptr, 0, 0);
		init_subframes_v1(&m);

		uint32_t output_ptr = voice_stage(st, &m, sfd_ptr + SFD_VOICES, last_sample_ptr);
		sfx_stage(st, &m, false, sfx_ptr, sfx_index);
		if (output_ptr)
			interleave_stage_v1(st, &m, output_ptr);
		else
			st->res->errors++;  /* ninguna voz con dirección de salida */
		st->res->commands++;

		if (--sfd_count == 0)
			break;
		sfd_ptr += SFD_VOICES + MAX_VOICES * VOICE_SIZE;
		aud_load(st, sfd, sfd_ptr, sizeof sfd);
		state_ptr = salsa64_rd32(sfd + SFD_STATE_PTR);
	}

	save_base_vol(st, m.base_vol, state_ptr + STATE_BASE_VOL);
	mx_st16(st, state_ptr + STATE_CC0, m.cc0, SUBFRAME_SIZE);
	mx_st16(st, state_ptr + STATE_740_LAST4_V1, m.last4, 4);
}

static void musyx_v2_task(struct aud_state *st, uint32_t sfd_ptr, uint32_t sfd_count)
{
	struct mx m;

	m.early = false;
	uint8_t sfd[SFD2_HEADER];

	for (;;) {
		aud_load(st, sfd, sfd_ptr, sizeof sfd);
		uint16_t sfx_index = salsa64_rd16(sfd + SFD_SFX_INDEX);
		uint32_t voice_mask = salsa64_rd32(sfd + SFD_VOICE_BITMASK);
		uint32_t state_ptr = salsa64_rd32(sfd + SFD_STATE_PTR);
		uint32_t sfx_ptr = salsa64_rd32(sfd + SFD_SFX_PTR);
		uint32_t ptr_10 = salsa64_rd32(sfd + SFD2_10_PTR);
		uint8_t mask_15 = sfd[SFD2_15_BITMASK];
		uint16_t mask_16 = salsa64_rd16(sfd + SFD2_16_BITMASK);
		uint32_t ptr_18 = salsa64_rd32(sfd + SFD2_18_PTR);
		uint32_t ptr_1c = salsa64_rd32(sfd + SFD2_1C_PTR);
		uint32_t ptr_20 = salsa64_rd32(sfd + SFD2_20_PTR);
		uint32_t ptr_24 = salsa64_rd32(sfd + SFD2_24_PTR);
		uint32_t last_sample_ptr = state_ptr + STATE_LAST_SAMPLE;

		load_base_vol(st, m.base_vol, state_ptr + STATE_BASE_VOL);
		mx_ld16(st, m.last4, state_ptr + STATE_740_LAST4_V2, 4);

		update_base_vol(st, m.base_vol, voice_mask, last_sample_ptr, mask_15, ptr_24);
		init_subframes_v2(&m);

		/* ptr_10 y mask_14 (una etapa que el original tampoco hace): se
		 * cuenta como comando sin implementar para verla en el registro. */
		if (ptr_10) {
			st->res->unknown++;
			st->res->last_unknown = ptr_10;
		}

		uint32_t output_ptr = voice_stage(st, &m, sfd_ptr + SFD2_VOICES, last_sample_ptr);
		sfx_stage(st, &m, true, sfx_ptr, sfx_index);

		if (output_ptr) {
			mx_st16(st, output_ptr, m.left, SUBFRAME_SIZE);
			mx_st16(st, output_ptr + 2 * SUBFRAME_SIZE, m.right, SUBFRAME_SIZE);
			mx_st16(st, output_ptr + 4 * SUBFRAME_SIZE, m.cc0, SUBFRAME_SIZE);
		} else {
			st->res->errors++;
		}

		save_base_vol(st, m.base_vol, state_ptr + STATE_BASE_VOL);
		mx_st16(st, state_ptr + STATE_740_LAST4_V2, m.last4, 4);

		if (mask_16 && ptr_20)
			interleave_stage_v2(st, &m, mask_16, ptr_18, ptr_1c, ptr_20);
		st->res->commands++;

		if (--sfd_count == 0)
			break;
		sfd_ptr += SFD2_VOICES + MAX_VOICES * VOICE_SIZE;
	}
}

void aud_musyx(struct aud_state *st, bool v2, bool early)
{
	uint32_t sfd_ptr = st->job->alist & 0xFFFFFF, sfd_count = st->job->alist_size;

	if (sfd_count == 0 || sfd_count > AUD_MUSYX_MAX_SFD) {
		st->res->errors++;
		return;
	}
	aud_find_lut(st);
	if (v2)
		musyx_v2_task(st, sfd_ptr, sfd_count);
	else
		musyx_v1_task(st, sfd_ptr, sfd_count, early);
}
