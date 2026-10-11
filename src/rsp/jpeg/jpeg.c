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
 * docs/THIRD_PARTY.md); código reescrito para Salsa64 (2026-10-11): RDRAM
 * en el orden de bytes de la N64, IDCT separable con tabla de cosenos. Se
 * compara contra el microcódigo real con el LLE (salsa64-run
 * --lle-check-jpeg): ver docs/estado.md.
 *
 * Corre en el PPE dentro del arranque de la tarea (core/rsp.c): 75 tareas
 * de 4 macrobloques por sala, una vez al cargarla.
 */
#include <math.h>
#include <string.h>

#include "core/endian.h"
#include "core/rom.h"
#include "rsp/jpeg/jpeg.h"

#define SB 64  /* coeficientes por bloque */

/* Microcódigo de Zelda OoT (4 KB, sin texto de identificación): CRC-32. */
#define OOT_UCODE_CRC  0x761D8504u
#define OOT_UCODE_SIZE 0x1000u

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

/* idct[n][k] = α(k)·cos((2n + 1)·k·π/16), α(0) = √(1/8), α(k) = 1/2. */
static float idct[8][8];
static bool idct_ready;

static void idct_init(void)
{
	for (int n = 0; n < 8; n++)
		for (int k = 0; k < 8; k++)
			idct[n][k] = (float)((k ? 0.5 : sqrt(0.125)) * cos((2 * n + 1) * k * 3.14159265358979323846 / 16.0));
	idct_ready = true;
}

enum salsa64_jpeg_kind salsa64_jpeg_detect(const uint8_t *ucode, uint32_t size)
{
	if (size == OOT_UCODE_SIZE && salsa64_crc32(ucode, size) == OOT_UCODE_CRC)
		return SALSA64_JPEG_OOT;
	return SALSA64_JPEG_NONE;
}

static int16_t sat16(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Un bloque: coeficientes en zigzag (en la RDRAM) → valores ×16 en orden
 * natural. */
static void decode_block(const uint8_t *src, const int16_t q[SB], int16_t out[SB])
{
	float f[SB], tmp[SB];
	for (int i = 0; i < SB; i++) {
		int z = zigzag[i];
		/* × cuantización saturado y ×16 en 16 bits (desborda como el
		 * registro: no pasa con coeficientes de un JPEG real). */
		int16_t c = (int16_t)(uint16_t)((uint16_t)sat16((int16_t)salsa64_rd16(src + 2 * z) * q[z]) << 4);
		f[i] = (float)c;
	}
	/* Filas y luego columnas. */
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			float s = 0.0f;
			for (int k = 0; k < 8; k++)
				s += idct[x][k] * f[y * 8 + k];
			tmp[y * 8 + x] = s;
		}
	for (int x = 0; x < 8; x++)
		for (int y = 0; y < 8; y++) {
			float s = 0.0f;
			for (int k = 0; k < 8; k++)
				s += idct[y][k] * tmp[k * 8 + x];
			out[y * 8 + x] = sat16((int32_t)lrintf(s));
		}
}

/* Canal en dieciseisavos → sus 5 bits altos (0xF80 del valor de 12 bits). */
static uint32_t chan(float v)
{
	int32_t x = (int32_t)v;  /* trunca hacia cero */
	x = x > 0xFF0 ? 0xFF0 : x < 0 ? 0 : x;
	return (uint32_t)x & 0xF80;
}

static uint16_t rgba5551(int16_t y, int16_t u, int16_t v)
{
	float fy = (float)y + 2048.0f, fu = (float)u, fv = (float)v;
	/* G: cada producto truncado por separado, como el punto fijo del RSP
	 * (con las constantes en flotante quedaba una unidad por debajo en el
	 * 1 % de los píxeles; ver docs/estado.md). */
	uint32_t r = chan(fy + 1.4025f * fv);
	uint32_t g = chan(fy - floorf(0.3443f * fu) - floorf(0.7144f * fv));
	uint32_t b = chan(fy + 1.7729f * fu);
	return (uint16_t)(r << 4 | g >> 1 | b >> 6 | 1);
}

/* Fila `row` de 16 píxeles: y = 8 de un bloque y 8 del siguiente, u y v la
 * fila de croma (un par de valores por cada dos píxeles). */
static void emit_row(const int16_t *y, const int16_t *u, const int16_t *v, uint8_t *dst)
{
	for (int x = 0; x < 16; x++) {
		int16_t yy = x < 8 ? y[x] : y[SB + x - 8];
		salsa64_wr16(dst + 2 * x, rgba5551(yy, u[x >> 1], v[x >> 1]));
	}
}

void salsa64_jpeg_run(enum salsa64_jpeg_kind kind, uint8_t *rdram, uint32_t rdram_size, uint32_t data_ptr,
		      struct salsa64_jpeg_result *res)
{
	memset(res, 0, sizeof *res);
	data_ptr &= 0x1FFFFFFF;
	if (kind != SALSA64_JPEG_OOT || data_ptr > rdram_size || rdram_size - data_ptr < 24) {
		res->bad = true;
		return;
	}
	if (!idct_ready)
		idct_init();
	const uint8_t *d = rdram + data_ptr;
	uint32_t addr = salsa64_rd32(d) & 0x1FFFFFFF, count = salsa64_rd32(d + 4), mode = salsa64_rd32(d + 8);
	if (mode != 0 && mode != 2) {
		res->bad = true;
		return;
	}
	int16_t q[3][SB];
	for (int t = 0; t < 3; t++) {
		uint32_t qa = salsa64_rd32(d + 12 + 4 * t) & 0x1FFFFFFF;
		if (qa > rdram_size || rdram_size - qa < 2 * SB) {
			res->bad = true;
			return;
		}
		for (int i = 0; i < SB; i++)
			q[t][i] = (int16_t)salsa64_rd16(rdram + qa + 2 * i);
	}
	unsigned blocks = mode + 4;  /* los dos últimos son U y V */
	uint32_t mb_bytes = blocks * SB * 2;
	res->out_addr = addr;
	res->mb_stride = mb_bytes;
	res->mb_out = mode == 0 ? 256u : 512u;
	for (uint32_t m = 0; m < count; m++, addr += mb_bytes) {
		if (addr > rdram_size || rdram_size - addr < mb_bytes) {
			res->bad = true;
			return;
		}
		int16_t px[6][SB];
		for (unsigned b = 0; b < blocks; b++)
			decode_block(rdram + addr + b * SB * 2, q[b + 2 < blocks ? 0 : b + 2 == blocks ? 1 : 2], px[b]);
		const int16_t *u = px[blocks - 2], *v = px[blocks - 1];
		uint8_t *out = rdram + addr;
		if (mode == 0) {
			for (int r = 0; r < 8; r++)
				emit_row(&px[0][8 * r], &u[8 * r], &v[8 * r], out + 32 * r);
		} else {
			/* Filas 0–7 de Y0/Y1, 8–15 de Y2/Y3; la croma, una fila por
			 * cada dos. */
			for (int r = 0; r < 16; r++)
				emit_row(&px[r < 8 ? 0 : 2][8 * (r & 7)], &u[8 * (r >> 1)], &v[8 * (r >> 1)], out + 32 * r);
		}
		res->macroblocks++;
		res->out_bytes = addr + res->mb_out - res->out_addr;
	}
}
