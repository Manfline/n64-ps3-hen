/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - mp3.c                                           *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *   Copyright (C) 2002 Hacktarux                                          *
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
 * Salsa64 — comando MP3 de naudio: la síntesis de MPEG-1 Layer III (DCT de
 * 32 puntos y ventana de 512) que el microcódigo de audio de Perfect Dark,
 * Banjo-Tooie, Jet Force Gemini y Conker hace en el RSP; el resto del
 * decodificador corre en el CPU de la N64. Adaptado de src/mp3.c de
 * mupen64plus-rsp-hle (commit 8a7a472a, 2026-06-23) para Salsa64
 * (2026-10-10): mismo orden de operaciones y redondeos, con estos cambios:
 *
 *   - La memoria de trabajo (DMEM 0x800–0xFFF del decodificador) es
 *     aud_state.mp3, en el orden de bytes de la N64; la RDRAM se lee y
 *     escribe por aud_load/aud_store (DMA en el SPE).
 *   - Los productos que en el original desbordan int32 (comportamiento
 *     indefinido en C) se hacen en uint32_t: el mismo resultado módulo
 *     2^32 que da el original en la práctica.
 *   - La ventana de síntesis (aud_mp3_window, mp3_lut.c) no es la tabla
 *     del microcódigo: la genera tools/audio-mp3-lut.py desde la ventana
 *     estándar de MPEG-1 y coincide con ella en todas las entradas que se
 *     leen.
 *
 * Por comando: 8 bytes de ganancias (las palabras de 0xCE8 y 0xCEC) y 3
 * bloques de 0x180 bytes de muestras de subbanda; cada bloque son 6 pasos
 * de 32 muestras (InnerLoop) y sale encima de la entrada, en la RDRAM.
 */
#include <string.h>

#include "rsp/audio/hle.h"

static uint8_t *mp(struct aud_state *st, uint32_t off)
{
	return st->mp3 + (off & (AUD_MP3_SIZE - 1));
}

static int32_t ld(struct aud_state *st, uint32_t off)
{
	return aud_ld16(mp(st, off));
}

static void st16(struct aud_state *st, uint32_t off, int32_t v)
{
	aud_st16(mp(st, off), (int16_t)v);
}

/* (a · b) >> 16 con el producto truncado a 32 bits, como el original (en
 * uint32_t: sin desbordes de int32 ni multiplicaciones de 64 bits, que en
 * el SPU son caras). */
static int32_t mulhi(int32_t a, int32_t b)
{
	return (int32_t)((uint32_t)a * (uint32_t)b) >> 16;
}

/* Producto de la ventana: Q15 redondeado. */
static int32_t win(struct aud_state *st, uint32_t off, unsigned k)
{
	return (ld(st, off) * aud_mp3_window[k] + 0x4000) >> 15;
}

/* Partes 2–4 de la DCT: mariposas de 8, 4 y 2 con cosenos en Q16 (los del
 * original: cos(kπ/64) con su redondeo). */
static void mp3ab0(int32_t *v)
{
	static const uint16_t LUT2[8] = {
		0xFEC4, 0xF4FA, 0xC5E4, 0xE1C4,
		0x1916, 0x4A50, 0xA268, 0x78AE
	};
	static const uint16_t LUT3[4] = { 0xFB14, 0xD4DC, 0x31F2, 0x8E3A };
	int i;

	for (i = 0; i < 8; i++) {
		v[16 + i] = v[0 + i] + v[8 + i];
		v[24 + i] = mulhi(v[0 + i] - v[8 + i], LUT2[i]);
	}
	for (i = 0; i < 4; i++) {
		v[0 + i] = v[16 + i] + v[20 + i];
		v[4 + i] = mulhi(v[16 + i] - v[20 + i], LUT3[i]);
		v[8 + i] = v[24 + i] + v[28 + i];
		v[12 + i] = mulhi(v[24 + i] - v[28 + i], LUT3[i]);
	}
	for (i = 0; i < 16; i += 4) {
		v[16 + i] = v[0 + i] + v[2 + i];
		v[18 + i] = mulhi(v[0 + i] - v[2 + i], 0xEC84);
		v[17 + i] = v[1 + i] + v[3 + i];
		v[19 + i] = mulhi(v[1 + i] - v[3 + i], 0x61F8);
	}
}

/* Las 32 muestras de subbanda en el orden en que las combina la DCT:
 * v[k] = in[a[k]] ± in[b[k]] (offsets en bytes desde inPtr). */
static void load_pairs(struct aud_state *st, int32_t *v, uint32_t in, int sign)
{
	static const uint8_t a[16] = { 0x00, 0x02, 0x06, 0x04, 0x0E, 0x0C, 0x08, 0x0A,
				       0x1E, 0x1C, 0x18, 0x1A, 0x10, 0x12, 0x16, 0x14 };
	static const uint8_t b[16] = { 0x3E, 0x3C, 0x38, 0x3A, 0x30, 0x32, 0x36, 0x34,
				       0x20, 0x22, 0x26, 0x24, 0x2E, 0x2C, 0x28, 0x2A };
	static const uint8_t bi[16] = { 31, 30, 28, 29, 24, 25, 27, 26, 16, 17, 19, 18, 23, 22, 20, 21 };
	for (int k = 0; k < 16; k++) {
		v[bi[k]] = ld(st, in + b[k]);
		v[k] = ld(st, in + a[k]) + sign * v[bi[k]];
	}
}

static void inner_loop(struct aud_state *st, uint32_t outPtr, uint32_t inPtr, uint32_t t6, uint32_t t5,
		       uint32_t t4)
{
	/* 0, 1, 3, 2, 7, 6, 4, 5, 7, 6, 4, 5, 0, 1, 3, 2 */
	static const uint16_t LUT6[16] = {
		0xFFB2, 0xFD3A, 0xF10A, 0xF854,
		0xBDAE, 0xCDA0, 0xE76C, 0xDB94,
		0x1920, 0x4B20, 0xAC7C, 0x7C68,
		0xABEC, 0x9880, 0xDAE8, 0x839C
	};
	int i, x;
	uint32_t t0, t1, t2, t3, offset, addptr, tmp;
	int32_t v2, v4, v6, v8, mult6, mult4, hi0, hi1, vt;
	int32_t v[32];

	/* Parte 1: sumas de las muestras simétricas. */
	load_pairs(st, v, inPtr, 1);

	mp3ab0(v);

	/* Parte 5: mariposas de 1 y escritura en el anillo de la ventana. */
	t0 = t6 + 0x100;
	t1 = t6 + 0x200;
	t2 = t5 + 0x100;
	t3 = t5 + 0x200;

	v[1] = 0;
	v[11] = mulhi(v[16] - v[17], 0xB504);
	v[16] = -v[16] - v[17];
	v[2] = v[18] + v[19];
	st16(st, t6, v[11]);
	v[11] = -v[11];
	st16(st, t3, v[16]);
	st16(st, t5, v[11]);
	v[2] = -v[2];
	st16(st, t2, v[2]);
	v[3] = mulhi(v[18] - v[19], 0x16A09) + v[2];
	st16(st, t0, v[3]);
	v[4] = -v[20] - v[21];
	v[6] = v[22] + v[23];
	v[5] = mulhi(v[20] - v[21], 0x16A09);
	st16(st, t3 - 0x80, v[4]);
	v[7] = mulhi(v[22] - v[23], 0x2D413);
	v[5] = v[5] - v[4];
	v[7] = v[7] - v[5];
	v[6] = v[6] + v[6];
	v[5] = v[5] - v[6];
	v[4] = -v[4] - v[6];
	st16(st, t1 - 0x80, v[7]);
	st16(st, t2 - 0x80, v[4]);
	st16(st, t0 - 0x80, v[5]);
	v[8] = v[24] + v[25];

	v[9] = mulhi(v[24] - v[25], 0x16A09);
	v[2] = v[8] + v[9];
	v[11] = mulhi(v[26] - v[27], 0x2D413);
	v[13] = mulhi(v[28] - v[29], 0x2D413);

	v[10] = v[26] + v[27];
	v[10] = v[10] + v[10];
	v[12] = v[28] + v[29];
	v[12] = v[12] + v[12];
	v[14] = v[30] + v[31];
	v[3] = v[8] + v[10];
	v[14] = v[14] + v[14];
	v[13] = (v[13] - v[2]) + v[12];
	v[15] = mulhi(v[30] - v[31], 0x5A827) - (v[11] + v[2]);
	v[14] = -(v[14] + v[14]) + v[3];
	v[17] = v[13] - v[10];
	v[9] = v[9] + v[14];
	st16(st, t6 + 0x40, v[9]);
	v[11] = v[11] - v[13];
	st16(st, t0 - 0x40, v[17]);
	v[12] = v[8] - v[12];
	st16(st, t0 + 0x40, v[11]);
	v[8] = -v[8];
	st16(st, t1 - 0x40, v[15]);
	v[10] = -v[10] - v[12];
	st16(st, t2 + 0x40, v[12]);
	st16(st, t3 - 0x40, v[8]);
	st16(st, t5 + 0x40, v[14]);
	st16(st, t2 - 0x40, v[10]);

	/* Parte 6: diferencias de las muestras simétricas. */
	load_pairs(st, v, inPtr, -1);

	for (i = 0; i < 16; i++)
		v[0 + i] = mulhi(v[0 + i], LUT6[i]);
	v[0] = v[0] + v[0];
	v[1] = v[1] + v[1];
	v[2] = v[2] + v[2];
	v[3] = v[3] + v[3];
	v[4] = v[4] + v[4];
	v[5] = v[5] + v[5];
	v[6] = v[6] + v[6];
	v[7] = v[7] + v[7];
	v[12] = v[12] + v[12];
	v[13] = v[13] + v[13];
	v[15] = v[15] + v[15];

	mp3ab0(v);

	/* Parte 7. */
	v[0] = (v[17] + v[16]) >> 1;
	v[1] = (int32_t)((uint32_t)v[17] * (uint32_t)(-0x5A82 * 2) + (uint32_t)v[16] * 0xB504u) >> 16;
	v[2] = -v[18] - v[19];
	v[3] = mulhi(v[18] - v[19], 0x16A09);
	v[4] = v[20] + v[21] + v[0];
	v[5] = mulhi(v[20] - v[21], 0x16A09) + v[1];
	v[6] = (((v[22] + v[23]) * 2) + v[0]) - v[2];
	v[7] = mulhi(v[22] - v[23], 0x2D413) + v[0] + v[1] + v[3];
	st16(st, t3 - 0x20, -v[0]);
	v[8] = v[24] + v[25];
	v[9] = mulhi(v[24] - v[25], 0x16A09);
	v[10] = ((v[26] + v[27]) * 2) + v[8];
	v[11] = mulhi(v[26] - v[27], 0x2D413) + v[8] + v[9];
	v[12] = v[4] - ((v[28] + v[29]) * 2);
	st16(st, t2 + 0x20, v[12]);
	v[13] = mulhi(v[28] - v[29], 0x2D413) - v[12] - v[5];
	v[14] = v[30] + v[31];
	v[14] = v[14] + v[14];
	v[14] = v[14] + v[14];
	v[14] = v[6] - v[14];
	v[15] = mulhi(v[30] - v[31], 0x5A827) - v[7];
	st16(st, t5 + 0x20, v[14]);
	v[14] = v[14] + v[1];
	st16(st, t6 + 0x20, v[14]);
	st16(st, t1 - 0x20, v[15]);
	v[9] = v[9] + v[10];
	v[1] = v[1] + v[6];
	v[6] = v[10] - v[6];
	v[1] = v[9] - v[1];
	st16(st, t5 + 0x60, v[6]);
	v[10] = v[10] + v[2];
	v[10] = v[4] - v[10];
	st16(st, t2 - 0x60, v[10]);
	v[12] = v[2] - v[12];
	st16(st, t2 - 0x20, v[12]);
	v[5] = v[4] + v[5];
	v[4] = v[8] - v[4];
	st16(st, t2 + 0x60, v[4]);
	v[0] = v[0] - v[8];
	st16(st, t3 - 0x60, v[0]);
	v[7] = v[7] - v[11];
	st16(st, t1 - 0x60, v[7]);
	v[11] = v[11] - v[3];
	st16(st, t6 + 0x60, v[1]);
	v[11] = v[11] - v[5];
	st16(st, t0 + 0x60, v[11]);
	v[3] = v[3] - v[13];
	st16(st, t0 + 0x20, v[3]);
	v[13] = v[13] + v[2];
	st16(st, t0 - 0x20, v[13]);
	v[2] = (v[5] - v[2]) - v[9];
	st16(st, t0 - 0x60, v[2]);

	/* Parte 8: ventana. */
	addptr = t6 & 0xFFE0;

	offset = 0x10 - (t4 >> 1);
	for (x = 0; x < 8; x++) {
		v2 = v4 = v6 = v8 = 0;
		for (i = 7; i >= 0; i--) {
			v2 += win(st, addptr + 0x00, offset + 0x00);
			v4 += win(st, addptr + 0x10, offset + 0x08);
			v6 += win(st, addptr + 0x20, offset + 0x20);
			v8 += win(st, addptr + 0x30, offset + 0x28);
			addptr += 2;
			offset++;
		}
		st16(st, outPtr, v2 + v4);
		st16(st, outPtr + 2, v6 + v8);
		outPtr += 4;
		addptr += 0x30;
		offset += 0x38;
	}

	offset = 0x10 - (t4 >> 1) + 8 * 0x40;
	v2 = v4 = 0;
	for (i = 0; i < 4; i++) {
		v2 += win(st, addptr + 0x00, offset + 0x00);
		v2 += win(st, addptr + 0x10, offset + 0x08);
		addptr += 2;
		offset++;
		v4 += win(st, addptr + 0x00, offset + 0x00);
		v4 += win(st, addptr + 0x10, offset + 0x08);
		addptr += 2;
		offset++;
	}
	uint32_t gain = salsa64_rd32(mp(st, 0xCE8));
	mult6 = (int32_t)gain;
	mult4 = (int32_t)salsa64_rd32(mp(st, 0xCEC));
	if (t4 & 0x2) {
		st16(st, outPtr, (int32_t)(((uint32_t)v2 * gain) >> 16));
	} else {
		st16(st, outPtr, (int32_t)(((uint32_t)v4 * gain) >> 16));
		mult4 = (int32_t)gain;
	}
	addptr -= 0x50;

	for (x = 0; x < 8; x++) {
		v2 = v4 = v6 = v8 = 0;
		offset = (0x22F - (t4 >> 1) + x * 0x40);
		for (i = 0; i < 4; i++) {
			v2 += win(st, addptr + 0x20, offset + 0x00);
			v2 -= win(st, addptr + 0x22, offset + 0x01);
			v4 += win(st, addptr + 0x30, offset + 0x08);
			v4 -= win(st, addptr + 0x32, offset + 0x09);
			v6 += win(st, addptr + 0x00, offset + 0x20);
			v6 -= win(st, addptr + 0x02, offset + 0x21);
			v8 += win(st, addptr + 0x10, offset + 0x28);
			v8 -= win(st, addptr + 0x12, offset + 0x29);
			addptr += 4;
			offset += 2;
		}
		st16(st, outPtr + 2, v2 + v4);
		st16(st, outPtr + 4, v6 + v8);
		outPtr += 4;
		addptr -= 0x50;
	}

	/* Ganancias: la parte alta de cada palabra, saturando. */
	tmp = outPtr;
	hi0 = mult6 >> 16;
	hi1 = mult4 >> 16;
	for (i = 0; i < 8; i++) {
		vt = ld(st, tmp - 0x40) * hi0;
		st16(st, tmp - 0x40, aud_clamp16(vt));
		vt = ld(st, tmp - 0x30) * hi0;
		st16(st, tmp - 0x30, aud_clamp16(vt));
		vt = ld(st, tmp - 0x1E) * hi1;
		st16(st, tmp - 0x1E, aud_clamp16(vt));
		vt = ld(st, tmp - 0xE) * hi1;
		st16(st, tmp - 0xE, aud_clamp16(vt));
		tmp += 2;
	}
}

void aud_mp3(struct aud_state *st, unsigned index, uint32_t address)
{
	uint32_t t6 = 0x08A0, t5 = 0x0AC0, t4 = index;
	uint32_t readPtr = address, writePtr = address;

	aud_load(st, mp(st, 0xCE8), readPtr, 8);
	readPtr += 8;
	for (int cnt = 0; cnt < 0x480; cnt += 0x180) {
		aud_load(st, mp(st, 0xCF0), readPtr, 0x180);
		uint32_t inPtr = 0xCF0, outPtr = 0xE70;
		for (int cnt2 = 0; cnt2 < 0x180; cnt2 += 0x40) {
			t6 = (t6 & 0xFFE0) | t4;
			t5 = (t5 & 0xFFE0) | t4;
			inner_loop(st, outPtr, inPtr, t6, t5, t4);
			t4 = (t4 - 2) & 0x1E;
			uint32_t t = t6;
			t6 = t5;
			t5 = t;
			inPtr += 0x40;
			outPtr += 0x40;
		}
		aud_store(st, writePtr, mp(st, 0xE70), 0x180);
		writePtr += 0x180;
		readPtr += 0x180;
	}
}
