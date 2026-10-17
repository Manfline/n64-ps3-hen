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
 * Aritmética (2026-10-17), la del microcódigo real: se midió con el LLE
 * (salsa64-run --lle-check-jpeg y un banco de pruebas que corre el
 * microcódigo con macrobloques sintéticos; docs/audio-censo.md) y no se
 * copió nada de él.
 *   IDCT    la mitad par y la impar con las constantes round(32768 ·
 *           cos(kπ/16)), productos de dos en dos con un solo redondeo (como
 *           VMULF + VMACF del RSP) y sumas saturadas a 16 bits; primero las
 *           filas y luego las columnas, que salen (a ± b + 2) >> 2.
 *   color   Y + 2048; la parte fraccionaria de cada constante (Q16) multiplica
 *           con el piso del producto y la parte entera se suma aparte:
 *           R = Y + V + ⌊V · 0,4025⌋, G = Y − ⌊U · 0,3443⌋ − ⌊V · 0,7144⌋,
 *           B = Y + U + ⌊U · 0,7729⌋; todo saturado a 16 bits.
 * Contra el microcódigo real: 0 píxeles distintos en 400 000 de macrobloques
 * sintéticos (continuos, un coeficiente suelto, aleatorios y con saturación)
 * y en los de OoT y Pokémon Stadium 1 y 2. Antes (IDCT separable propia con
 * constantes Q15 y color en Q14): 99,0 % de los píxeles iguales y el resto a
 * ±1 en un canal de 5 bits.
 * Los desplazamientos a la derecha de negativos son aritméticos (gcc y
 * clang en todas las arquitecturas). Las sumas de dos productos caben en 32
 * bits (|valor| ≤ 32768 y constantes ≤ 32138), así que la versión de 4
 * carriles (jpeg_v.c) da lo mismo con enteros de 32 bits.
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
const uint8_t salsa64_jpeg_zigzag[SB] = {
	0,  1,  5,  6,  14, 15, 27, 28,
	2,  4,  7,  13, 16, 26, 29, 42,
	3,  8,  12, 17, 25, 30, 41, 43,
	9,  11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63,
};

#define zigzag    salsa64_jpeg_zigzag
#define CR_R      SALSA64_JPEG_CR_R
#define CB_G      SALSA64_JPEG_CB_G
#define CR_G      SALSA64_JPEG_CR_G
#define CB_B      SALSA64_JPEG_CB_B

static int16_t sat16(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Las operaciones de 16 bits del RSP que usa la IDCT: VMULF + VMACF (dos
 * productos de 16 × 16 acumulados en 48 bits con un solo redondeo, el
 * resultado recortado a 16 bits) y VADD/VSUB con saturación. */
static int16_t mac2(int a, int b, int c, int d)
{
	int64_t acc = 2 * (int64_t)a * b + 2 * (int64_t)c * d + 0x8000;
	return sat16((int32_t)(acc >> 16));
}

static int16_t sadd(int a, int b)
{
	return sat16(a + b);
}

static int16_t ssub(int a, int b)
{
	return sat16(a - b);
}

/* IDCT unidimensional del microcódigo de OoT, medida con el LLE (docs/
 * audio-censo.md): mitades par e impar con las constantes round(32768 ·
 * cos(kπ/16)) —6393, 27246, 32138 y 18205 en la impar, 23170, 12540 y 30274
 * en la par—, productos de dos en dos con VMULF + VMACF (un redondeo) y
 * sumas saturadas de 16 bits. `last` (la pasada de las columnas): las
 * salidas salen (a ± b) / 4 con un redondeo, (a ± b + 2) >> 2. */
static void idct1(const int16_t x[8], int16_t o[8], bool last)
{
	int16_t v10 = mac2(x[3], 27246, x[5], 18205);
	int16_t v11 = mac2(x[7], 6393, x[1], 32138);
	int16_t v8 = mac2(x[1], 6393, x[7], -32138);
	int16_t v9 = mac2(x[5], 27246, x[3], -18205);
	int16_t v6 = mac2(x[0], 23170, x[4], -23170);
	int16_t v5 = ssub(v11, v10);
	int16_t v4 = ssub(v8, v9);
	int16_t v12 = sadd(v8, v9);
	int16_t v15 = sadd(v11, v10);
	int16_t v13 = mac2(v5, 23170, v4, -23170);
	int16_t v14 = mac2(v5, 23170, v4, 23170);
	int16_t w4 = mac2(x[0], 23170, x[4], 23170);
	int16_t w5 = mac2(x[6], 12540, x[2], 30274);
	int16_t v7 = mac2(x[2], 12540, x[6], -30274);
	int16_t a0 = sadd(w4, w5), a3 = ssub(w4, w5), a1 = sadd(v6, v7), a2 = ssub(v6, v7);
	if (!last) {
		o[0] = sadd(a0, v15); o[7] = ssub(a0, v15);
		o[3] = sadd(a3, v12); o[4] = ssub(a3, v12);
		o[1] = sadd(a1, v14); o[6] = ssub(a1, v14);
		o[2] = sadd(a2, v13); o[5] = ssub(a2, v13);
	} else {
		static const uint8_t lo[4] = { 0, 3, 1, 2 }, hi[4] = { 7, 4, 6, 5 };
		const int16_t p[4] = { a0, a3, a1, a2 }, q[4] = { v15, v12, v14, v13 };
		for (int j = 0; j < 4; j++) {
			o[lo[j]] = (int16_t)((p[j] + q[j] + 2) >> 2);
			o[hi[j]] = (int16_t)((p[j] - q[j] + 2) >> 2);
		}
	}
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
	/* Filas, y luego columnas (cada una con la salida k = 0–7 en su lugar). */
	for (int y = 0; y < 8; y++)
		idct1(&c[8 * y], &r[8 * y], false);
	for (int x = 0; x < 8; x++) {
		int16_t col[8], o[8];
		for (int k = 0; k < 8; k++)
			col[k] = r[8 * k + x];
		idct1(col, o, true);
		for (int k = 0; k < 8; k++)
			out[8 * k + x] = o[k];
	}
}

/* Canal en dieciseisavos → sus 5 bits altos (0xF80 del valor de 12 bits). */
static uint32_t chan(int32_t x)
{
	x = x > 0xFF0 ? 0xFF0 : x < 0 ? 0 : x;
	return (uint32_t)x & 0xF80;
}

/* Como el microcódigo: la parte fraccionaria de cada constante (Q16, sin
 * signo) multiplica con VMUDM (el piso del producto entre 65536) y se suma
 * la parte entera aparte; todo en 16 bits saturados.
 *   R = Y + V + ⌊V · 0,4025⌋        G = Y − ⌊U · 0,3443⌋ − ⌊V · 0,7144⌋
 *   B = Y + U + ⌊U · 0,7729⌋ */
static uint16_t rgba5551(int16_t y, int16_t u, int16_t v)
{
	int32_t yy = sat16(y + 2048);
	int32_t rt = sat16(v + sat16((v * CR_R) >> 16));
	int32_t gt = sat16(sat16((u * CB_G) >> 16) + sat16((v * CR_G) >> 16));
	int32_t bt = sat16(u + sat16((u * CB_B) >> 16));
	uint32_t r = chan(sat16(rt + yy));
	uint32_t g = chan(sat16(yy - gt));
	uint32_t b = chan(sat16(bt + yy));
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
