/* SPDX-License-Identifier: GPL-2.0-or-later */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - jpeg.c                                          *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 Bobby Smiles                                       *
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
 * Salsa64 — tarea JPEG del RSP de Zelda OoT (fondos de las salas
 * prerrenderizadas). El CPU de la N64 ya hizo el Huffman: cada macrobloque
 * llega como 4 (modo 0, 4:2:2: Y0 Y1 U V) o 6 (modo 2, 4:2:0: Y0 Y1 Y2 Y3
 * U V) bloques de 64 coeficientes de 16 bits en orden zigzag. Por bloque:
 * coeficiente × cuantización (saturado a 16 bits) × 16, al orden natural,
 * IDCT 8×8; los valores quedan en dieciseisavos, centrados en 0. Luego
 * YCbCr → RGBA5551 (Y + 2048, constantes de JFIF, truncado a 5 bits, alfa
 * 1) y el macrobloque se escribe en su mismo lugar: 16 × 8 (modo 0) o
 * 16 × 16 (modo 2) píxeles, fila por fila.
 *
 * Algoritmo, orden de los bloques y constantes según jpeg_decode_PS de
 * src/jpeg.c de mupen64plus-rsp-hle (rama master consultada el 2026-10-11;
 * docs/THIRD_PARTY.md); código reescrito para Salsa64.
 *
 * Aritmética entera (2026-10-12), para que el host, el PPE y el SPE den lo
 * mismo byte a byte (el SPE redondea los flotantes de simple precisión
 * hacia cero) y sin las conversiones a entero que en el PPE pasan por
 * memoria:
 *   IDCT    constantes α(k)·cos((2n + 1)kπ/16) en Q15 (tabla fija); filas y
 *           luego columnas, productos de 16 × 16 bits sumados en 32. Las
 *           filas quedan con 2 bits de fracción, redondeadas y recortadas
 *           a 16 bits; las columnas, redondeadas al dieciseisavo. En los
 *           fondos de OoT las sumas no pasan de 2^28,2 (margen ×7 contra el
 *           desborde) ni las filas de 23 400 (recorte en 32 767).
 *   color   productos con constantes en Q14, cada uno truncado (como el
 *           punto fijo del RSP: con flotantes G quedaba una unidad por
 *           debajo en el 1 % de los píxeles).
 * Contra el microcódigo real (salsa64-run --lle-check-jpeg, casa de Link
 * con partida nueva, 150 tareas): 99,06 % de los píxeles iguales y el
 * resto a ±1 en un canal de 5 bits (la versión con flotantes, 99,03 %);
 * contra una IDCT exacta en doble precisión, 0,19 % de los píxeles a ±1.
 * Los desplazamientos a la derecha de negativos son aritméticos (gcc y
 * clang en todas las arquitecturas) y las sumas van en uint32_t: un
 * desborde (solo con coeficientes absurdos) da la vuelta igual en todas.
 *
 * Corre en el PPE dentro del arranque de la tarea (core/rsp.c) o, en la
 * PS3, en un SPE (src/spu/jpeg): 75 tareas de 4 macrobloques por sala, una
 * vez al cargarla.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/jpeg/jpeg.h"

#define SB 64  /* coeficientes por bloque */

/* Posición en el orden zigzag de cada coeficiente en orden natural. */
static const uint8_t zigzag[SB] = {
	0,  1,  5,  6,  14, 15, 27, 28,
	2,  4,  7,  13, 16, 26, 29, 42,
	3,  8,  12, 17, 25, 30, 41, 43,
	9,  11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63,
};

/* idct[n][k] = α(k)·cos((2n + 1)·k·π/16) · 2^15, α(0) = √(1/8), α(k) = 1/2,
 * redondeado. */
static const int16_t idct[8][8] = {
	{ 11585,  16069,  15137,  13623,  11585,   9102,   6270,   3196 },
	{ 11585,  13623,   6270,  -3196, -11585, -16069, -15137,  -9102 },
	{ 11585,   9102,  -6270, -16069, -11585,   3196,  15137,  13623 },
	{ 11585,   3196, -15137,  -9102,  11585,  13623,  -6270, -16069 },
	{ 11585,  -3196, -15137,   9102,  11585, -13623,  -6270,  16069 },
	{ 11585,  -9102,  -6270,  16069, -11585,  -3196,  15137, -13623 },
	{ 11585, -13623,   6270,   3196, -11585,  16069, -15137,   9102 },
	{ 11585, -16069,  15137, -13623,  11585,  -9102,   6270,  -3196 },
};

#define ROW_SHIFT 13  /* Q15 → 2 bits de fracción */
#define COL_SHIFT 17  /* Q15 + 2 bits → dieciseisavos */

/* YCbCr → RGB en Q14: 1,4025, 0,3443, 0,7144, 1,7729. */
#define CR_R 22979
#define CB_G 5641
#define CR_G 11705
#define CB_B 29047

static int16_t sat16(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Suma de 8 productos de 16 × 16 bits más el redondeo de `shift`, con
 * vuelta en 32 bits, desplazada. */
static int32_t dot8(const int16_t k[8], const int16_t *x, unsigned stride, unsigned shift)
{
	uint32_t s = 1u << (shift - 1);
	for (int i = 0; i < 8; i++)
		s += (uint32_t)((int32_t)k[i] * x[i * stride]);
	return (int32_t)s >> shift;
}

/* Un bloque: coeficientes en zigzag (orden de la N64) → valores ×16 en
 * orden natural. */
static void decode_block(const uint8_t *src, const int16_t q[SB], int16_t out[SB])
{
	int16_t c[SB], r[SB];
	for (int i = 0; i < SB; i++) {
		int z = zigzag[i];
		/* × cuantización saturado y ×16 en 16 bits (desborda como el
		 * registro: no pasa con coeficientes de un JPEG real). */
		c[i] = (int16_t)(uint16_t)((uint16_t)sat16((int16_t)salsa64_rd16(src + 2 * z) * q[z]) << 4);
	}
	/* Filas: r[y][x] = Σk idct[x][k]·c[y][k]. */
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			r[y * 8 + x] = sat16(dot8(idct[x], &c[y * 8], 1, ROW_SHIFT));
	/* Columnas: out[y][x] = Σk idct[y][k]·r[k][x]. */
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			out[y * 8 + x] = sat16(dot8(idct[y], &r[x], 8, COL_SHIFT));
}

/* Canal en dieciseisavos → sus 5 bits altos (0xF80 del valor de 12 bits). */
static uint32_t chan(int32_t x)
{
	x = x > 0xFF0 ? 0xFF0 : x < 0 ? 0 : x;
	return (uint32_t)x & 0xF80;
}

static uint16_t rgba5551(int16_t y, int16_t u, int16_t v)
{
	int32_t yy = y + 2048;
	uint32_t r = chan(yy + ((v * CR_R) >> 14));
	uint32_t g = chan(yy - ((u * CB_G) >> 14) - ((v * CR_G) >> 14));
	uint32_t b = chan(yy + ((u * CB_B) >> 14));
	return (uint16_t)(r << 4 | g >> 1 | b >> 6 | 1);
}

/* Fila de 16 píxeles: y = 8 de un bloque y 8 del siguiente, u y v la fila
 * de croma (un par de valores por cada dos píxeles). */
static void emit_row(const int16_t *y, const int16_t *u, const int16_t *v, uint8_t *dst)
{
	for (int x = 0; x < 16; x++) {
		int16_t yy = x < 8 ? y[x] : y[SB + x - 8];
		salsa64_wr16(dst + 2 * x, rgba5551(yy, u[x >> 1], v[x >> 1]));
	}
}

bool salsa64_jpeg_task_read(const uint8_t *d, struct salsa64_jpeg_task *t)
{
	t->addr = salsa64_rd32(d) & 0x1FFFFFFF;
	t->count = salsa64_rd32(d + 4);
	t->mode = salsa64_rd32(d + 8);
	for (int i = 0; i < 3; i++)
		t->qaddr[i] = salsa64_rd32(d + 12 + 4 * i) & 0x1FFFFFFF;
	return t->mode == 0 || t->mode == 2;
}

void salsa64_jpeg_qtable(const uint8_t *src, int16_t q[SB])
{
	for (int i = 0; i < SB; i++)
		q[i] = (int16_t)salsa64_rd16(src + 2 * i);
}

void salsa64_jpeg_mb(uint32_t mode, const int16_t q[3][SB], const uint8_t *in, uint8_t *out)
{
	unsigned blocks = mode + 4;  /* los dos últimos son U y V */
	int16_t px[6][SB];
	for (unsigned b = 0; b < blocks; b++)
		decode_block(in + b * SB * 2, q[b + 2 < blocks ? 0 : b + 2 == blocks ? 1 : 2], px[b]);
	const int16_t *u = px[blocks - 2], *v = px[blocks - 1];
	if (mode == 0) {
		for (int r = 0; r < 8; r++)
			emit_row(&px[0][8 * r], &u[8 * r], &v[8 * r], out + 32 * r);
	} else {
		/* Filas 0–7 de Y0/Y1, 8–15 de Y2/Y3; la croma, una fila por cada
		 * dos. */
		for (int r = 0; r < 16; r++)
			emit_row(&px[r < 8 ? 0 : 2][8 * (r & 7)], &u[8 * (r >> 1)], &v[8 * (r >> 1)], out + 32 * r);
	}
}

void salsa64_jpeg_run(enum salsa64_jpeg_kind kind, uint8_t *rdram, uint32_t rdram_size, uint32_t data_ptr,
		      struct salsa64_jpeg_result *res)
{
	struct salsa64_jpeg_task t;
	memset(res, 0, sizeof *res);
	data_ptr &= 0x1FFFFFFF;
	if (kind != SALSA64_JPEG_OOT || data_ptr > rdram_size || rdram_size - data_ptr < SALSA64_JPEG_TASK_BYTES ||
	    !salsa64_jpeg_task_read(rdram + data_ptr, &t)) {
		res->bad = true;
		return;
	}
	int16_t q[3][SB];
	for (int i = 0; i < 3; i++) {
		if (t.qaddr[i] > rdram_size || rdram_size - t.qaddr[i] < 2 * SB) {
			res->bad = true;
			return;
		}
		salsa64_jpeg_qtable(rdram + t.qaddr[i], q[i]);
	}
	uint32_t addr = t.addr, mb_bytes = SALSA64_JPEG_MB_IN(t.mode);
	res->out_addr = addr;
	res->mb_stride = mb_bytes;
	res->mb_out = SALSA64_JPEG_MB_OUT(t.mode);
	for (uint32_t m = 0; m < t.count; m++, addr += mb_bytes) {
		if (addr > rdram_size || rdram_size - addr < mb_bytes) {
			res->bad = true;
			return;
		}
		salsa64_jpeg_mb(t.mode, q, rdram + addr, rdram + addr);
		res->macroblocks++;
		res->out_bytes = addr + res->mb_out - res->out_addr;
	}
}
