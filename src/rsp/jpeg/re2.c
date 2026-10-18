/* SPDX-License-Identifier: GPL-2.0-or-later */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - re2.c                                           *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2016 Gilles Siberlin                                    *
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
 * Salsa64 — tareas de video de Resident Evil 2. Ver re2.h.
 *
 * Qué hace cada tarea, qué parámetros lee y cómo se reconocen los tres
 * microcódigos, según resize_bilinear_task, decode_video_frame_task y
 * fill_video_double_buffer_task de src/re2.c y try_normal_task_detection de
 * src/hle.c de mupen64plus-rsp-hle (commit 8a7a472a; docs/THIRD_PARTY.md);
 * código reescrito para Salsa64.
 *
 * Aritmética (2026-10-17), la del microcódigo real: se midió con el LLE
 * (salsa64-run --lle-check-jpeg, SALSA64_RE2_DUMP y un script que lee los
 * volcados; docs/estado.md) y no se copió nada de él. La de mupen64plus
 * (dobles y truncado, bilineal en 64 bits sin difuminado, mezcla con el cuarto
 * byte en cero) daba el 46 % de los bytes distintos en RESIZE y el 1,6 % y el
 * 1,5 % en DECODE y FILL. Lo que se encontró:
 *   RESIZE  pesos de 15 bits (⌊x_frac / 2⌋ y ⌊(y_frac / 2) mod 2^15⌋), los
 *           cuatro productos ⌊wx · wy / 2^16⌋ de a dos en 14 bits, ×4; cada
 *           canal es la suma de los cuatro ⌊byte · peso / 2^16⌋ (cada
 *           producto con su piso), más un redondeo de 0 a 7, tope 255 y los 3
 *           bits bajos en cero (5 bits por canal). El redondeo es 4 salvo con
 *           el difuminado (umbral > 0 en el segmento de datos: Resident Evil 2
 *           lo usa en las franjas de 596 de ancho): cada carril de un grupo de
 *           8 píxeles acumula su suma con el redondeo anterior; la acumulada
 *           del carril de al lado (la del último carril del grupo anterior
 *           para el 0) se compara con la suma del píxel en los tres canales y,
 *           si la diferencia es menor que el umbral en los tres, el redondeo
 *           es esa acumulada & 7; si no, 4. Arranca con ceros y con las
 *           palabras dram_stack y dram_stack_size de la tarea (lo que hay en
 *           0xFE0–0xFE5 de la DMEM). El origen en x y en y
 *           lleva su parte fraccionaria (la fila de origen es la parte entera
 *           de y0 + n · y_ratio, no solo la de src_offset). El microcódigo
 *           trae dos filas de origen a la DMEM (1920 bytes), calcula 8
 *           píxeles por pasada y escribe la fila de salida desde 0x780: para
 *           el último píxel de una fila de 436 (x en 319) el vecino de abajo y
 *           a la derecha cae en esa fila de salida, y se lee tal como está.
 *   DECODE  cada canal: ⌊(Y · 128 · K4) / 2^16⌋ más o menos ⌊(croma · 128 − 0x4000)
 *           · K / 2^16⌋ con piso por producto, recortado a 0..0x7F80 y >> 7;
 *           las constantes K salen del segmento de datos de la tarea. El cuarto
 *           byte (el microcódigo no lo escribe) queda con lo que hay en la
 *           DMEM de salida: ceros, salvo en las primeras 28 posiciones de la
 *           primera fila de cada par, donde el DMA de la croma Cr (que trae el
 *           ancho completo desde 0x280 y pasa de los 0x300 de esa fila) deja
 *           sus bytes 131, 135, … ; la segunda fila queda en cero.
 *   FILL    (s + d) >> 1 en los cuatro bytes, también el cuarto.
 */
#include <string.h>

#ifdef __SPU__
#include <spu_intrinsics.h>
#endif

#include "core/endian.h"
#include "rsp/jpeg/re2.h"

/* Suma de los 256 primeros bytes del código de cada microcódigo
 * (mupen64plus-rsp-hle, hle.c: try_normal_task_detection). */
#define SUM_RESIZE 0x450fu
#define SUM_DECODE 0x3b44u
#define SUM_FILL   0x3d84u

enum salsa64_jpeg_kind salsa64_re2_detect(const uint8_t *ucode, uint32_t size)
{
	if (size < 256)
		return SALSA64_JPEG_NONE;
	uint32_t sum = 0;
	for (unsigned i = 0; i < 256; i++)
		sum += ucode[i];
	switch (sum) {
	case SUM_RESIZE: return SALSA64_JPEG_RE2_RESIZE;
	case SUM_DECODE: return SALSA64_JPEG_RE2_DECODE;
	case SUM_FILL: return SALSA64_JPEG_RE2_FILL;
	}
	return SALSA64_JPEG_NONE;
}

#define PHYS 0x1FFFFFFFu

/* VADD y VSUB saturan a 16 bits con signo. */
static inline int32_t sat16(int32_t v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static inline uint32_t dw(const uint8_t *d, unsigned o)
{
	return salsa64_rd32(d + o);
}

static inline uint32_t dh(const uint8_t *d, unsigned o)
{
	return salsa64_rd16(d + o);
}

/* ---- RESIZE ---- */

/* Las constantes del segmento de datos con las que el núcleo está medido
 * (las cargan los vectores v29, v30 y v31 de la tarea): máscaras de 15 bits,
 * 3 bytes por píxel, pesos ×4, alfa 1, máscara de 5 bits 0xF8, el redondeo
 * de +4 (máscara 7) con tope 255 y los 960 bytes de una fila de origen; el
 * umbral del difuminado (v31[1]) es un parámetro. */
static bool resize_consts(const uint8_t *d)
{
	static const uint16_t v29[8] = { 0xFFFF, 0x7FFF, 3, 4, 1, 0xF8, 0x4000, 8 };
	for (unsigned i = 0; i < 8; i++)
		if (dh(d, 0x40 + 2 * i) != v29[i])
			return false;
	return dh(d, 0x50) == 0x80 && dh(d, 0x52) == 7 && dh(d, 0x54) == 0x8000 && dh(d, 0x56) == 0xFF &&
	       dh(d, 0x60) == SALSA64_RE2_SRC_ROW;
}

bool salsa64_re2_resize_read(const uint8_t *d, struct salsa64_re2_resize *t)
{
	t->src = dw(d, 0x00) & PHYS;
	t->dst = dw(d, 0x04) & PHYS;
	t->width = dw(d, 0x08);
	t->rows = dw(d, 0x0C);
	t->x_ratio = dw(d, 0x10);
	t->y_ratio = dw(d, 0x14);
	t->stride = dw(d, 0x18);
	t->x0 = dw(d, 0x20);
	t->y0 = dw(d, 0x24);
	t->thresh = (int16_t)dh(d, 0x62);
	/* El ancho múltiplo de 4 (filas de salida de 8 bytes múltiplos, para el DMA del SPE). */
	if (!resize_consts(d) || t->width < 4 || t->width > 1024 || (t->width & 3) || t->rows < 1 || t->rows > 4096 ||
	    t->stride < 2 * t->width || t->stride > 65536 || (t->stride & 7) || (t->src | t->dst) & 7)
		return false;
	/* El paso por grupo de 8 píxeles, en el segmento de datos, es 8 · x_ratio. */
	if (((dh(d, 0x58) << 16) | dh(d, 0x5A)) != 8 * t->x_ratio)
		return false;
	/* Los píxeles de un grupo: 8 · ⌈ancho / 8⌉; el vecino de abajo a la derecha
	 * (3 · x + 963 + 2) tiene que quedar antes de 0xF80 (la memoria de trabajo
	 * del microcódigo) y x no puede dar la vuelta. */
	uint64_t px = (uint64_t)((t->width + 7) >> 3) * 8;
	uint64_t xmax = t->x0 + (px - 1) * (uint64_t)t->x_ratio;
	if (xmax >= (1ull << 32) || 3 * (xmax >> 16) + 965 >= 0xF80u)
		return false;
	return true;
}

void salsa64_re2_resize_begin(struct salsa64_re2_dither *st, uint32_t dram_stack, uint32_t dram_stack_size)
{
	memset(st->v, 0, sizeof st->v);
	st->last[0] = (int16_t)(dram_stack >> 16);
	st->last[1] = (int16_t)dram_stack;
	st->last[2] = (int16_t)(dram_stack_size >> 16);
}

/* VABS: valor absoluto con -32768 → 32767. */
static inline int32_t absat(int32_t d)
{
	return d < 0 ? (d == -32768 ? 32767 : -d) : d;
}

void salsa64_re2_resize_row(uint8_t *dm, const struct salsa64_re2_resize *t, struct salsa64_re2_dither *st,
			    uint32_t n)
{
	const uint32_t gp = t->y0 + n * t->y_ratio;
	const uint32_t wy = (gp >> 1) & 0x7FFF, wyc = 0x7FFF - wy;
	const unsigned groups = (t->width + 7) >> 3;
	uint32_t x = t->x0;
	for (unsigned g = 0; g < groups; g++) {
		int32_t s[3][8];
		for (unsigned i = 0; i < 8; i++, x += t->x_ratio) {
			const uint32_t wx = (x & 0xFFFF) >> 1, wxc = 0x7FFF - wx;
			const uint32_t wa = ((wxc * wyc) >> 16) << 2, wb = ((wx * wyc) >> 16) << 2;
			const uint32_t wc = ((wxc * wy) >> 16) << 2, wd = ((wx * wy) >> 16) << 2;
			const uint8_t *a = dm + 3 * (x >> 16), *b = a + 3, *c = a + SALSA64_RE2_SRC_ROW, *e = c + 3;
			for (unsigned k = 0; k < 3; k++)
				s[k][i] = (int32_t)(((a[k] * wa) >> 16) + ((b[k] * wb) >> 16) + ((c[k] * wc) >> 16) +
						    ((e[k] * wd) >> 16));
		}
		/* El difuminado: por canal, lo acumulado por carril (VADD con lo del grupo anterior),
		 * rotado un carril (el 0 recibe el último del grupo anterior), y qué carriles quedan
		 * a menos del umbral de su suma en los tres canales (VCC). */
		int32_t rot[3][8];
		bool near[8];
		for (unsigned k = 0; k < 3; k++) {
			int32_t acc[8];
			for (unsigned i = 0; i < 8; i++)
				acc[i] = sat16(st->v[k][i] + s[k][i]);
			rot[k][0] = st->last[k];
			for (unsigned i = 1; i < 8; i++)
				rot[k][i] = acc[i - 1];
			st->last[k] = acc[7];
		}
		for (unsigned i = 0; i < 8; i++)
			near[i] = absat(sat16(s[0][i] - rot[0][i])) < t->thresh &&
				  absat(sat16(s[1][i] - rot[1][i])) < t->thresh &&
				  absat(sat16(s[2][i] - rot[2][i])) < t->thresh;
		uint8_t grp[16];
		for (unsigned i = 0; i < 8; i++) {
			uint32_t ch[3];
			for (unsigned k = 0; k < 3; k++) {
				st->v[k][i] = near[i] ? (rot[k][i] & 7) : 4;
				int32_t o = s[k][i] + st->v[k][i];
				ch[k] = (uint32_t)(o > 255 ? 255 : o) & 0xF8;
			}
			/* byte 0 → azul, 1 → verde, 2 → rojo; RGBA5551 con el alfa en 1 */
			uint32_t p = (ch[2] << 8) | (ch[1] << 3) | (ch[0] >> 2) | 1;
			grp[2 * i] = (uint8_t)(p >> 8);
			grp[2 * i + 1] = (uint8_t)p;
		}
		memcpy(dm + SALSA64_RE2_OUT_BASE + 16 * g, grp, sizeof grp);
	}
}

/* ---- DECODE ---- */

bool salsa64_re2_decode_read(const uint8_t *d, struct salsa64_re2_decode *t)
{
	t->y = dw(d, 0x00) & PHYS;
	t->cb = dw(d, 0x04) & PHYS;
	t->cr = dw(d, 0x08) & PHYS;
	t->dst = dw(d, 0x0C) & PHYS;
	t->width = dw(d, 0x10);
	t->pairs = dw(d, 0x1C);
	t->step = dw(d, 0x24);
	for (unsigned i = 0; i < 5; i++)
		t->k[i] = dh(d, 0x40 + 2 * i);
	t->zero = (int16_t)dh(d, 0x50);
	t->max = (int16_t)dh(d, 0x56);
	const uint32_t height = dw(d, 0x14), passes = dw(d, 0x18), dma = dw(d, 0x20);
	/* El microcódigo trae con un DMA los planos de un par de filas (Y a 0, Cb a
	 * 0x200, Cr a 0x280) y escribe las dos filas de salida a partir de 0x300 y
	 * 0x300 + 4 · ancho; el alfa de las dos zonas queda en cero (se limpian
	 * hasta 0xAC0) salvo lo que deja el DMA de Cr: ancho ≤ 240 y múltiplo de 16
	 * (cada pasada hace 16 píxeles). La salida es un DMA de 2 filas de 4 · ancho
	 * bytes con el salto de la palabra [0x20]; el paso entre pares de filas es
	 * el de las dos. */
	if (t->width < 16 || t->width > 240 || (t->width & 15) || t->pairs < 1 || t->pairs > 4096 ||
	    height != 2 * t->pairs || passes < 1 || passes > 2 || ((t->y | t->cb | t->cr | t->dst) & 7))
		return false;
	const uint32_t len = (dma & 0xFFF) + 1, count = (dma >> 12) & 0xFF, skip = (dma >> 20) & 0xFFF;
	if (len != 4 * t->width || count != 1 || (skip & 7) || t->step != 2 * (len + skip))
		return false;
	return true;
}

/* Un canal: ⌊Y'⌋ ± términos, con VADD/VSUB saturados a 16 bits, VGE contra 0 y VLT contra el
 * tope. */
static inline uint8_t chan(int32_t v, int32_t max)
{
	v = sat16(v);
	v = v < 0 ? 0 : v;
	v = v > max ? max : v;
	return (uint8_t)(v >> 7);
}

void salsa64_re2_decode_pair(const struct salsa64_re2_decode *t, const uint8_t *y, const uint8_t *cb,
			     const uint8_t *cr, uint8_t *row0, uint8_t *row1)
{
	const unsigned w = t->width;
	const int32_t k0 = (int32_t)t->k[0], k1 = (int32_t)t->k[1], k2 = (int32_t)t->k[2], k3 = (int32_t)t->k[3];
	const int32_t k4 = (int32_t)t->k[4], max = t->max;
	for (unsigned c = 0; c < w / 2; c++) {
		/* VSUB: croma · 128 (LUV) menos el cero, saturado; VMUDM: producto con signo por la
		 * constante sin signo, la mitad alta (piso). */
		const int32_t dcr = sat16((int32_t)cr[c] * 128 - t->zero), dcb = sat16((int32_t)cb[c] * 128 - t->zero);
		const int32_t tr = (dcr * k0) >> 16, tg1 = (dcr * k1) >> 16, tg2 = (dcb * k2) >> 16, tb = (dcb * k3) >> 16;
		for (unsigned i = 0; i < 2; i++) {
			const unsigned x = 2 * c + i;
			const int32_t y0 = ((int32_t)y[x] * 128 * k4) >> 16, y1 = ((int32_t)y[w + x] * 128 * k4) >> 16;
			uint8_t *p0 = row0 + 4 * x, *p1 = row1 + 4 * x;
			p0[0] = chan(y0 + tr, max);
			p0[1] = chan(sat16(y0 - tg1) - tg2, max);
			p0[2] = chan(y0 + tb, max);
			p1[0] = chan(y1 + tr, max);
			p1[1] = chan(sat16(y1 - tg1) - tg2, max);
			p1[2] = chan(y1 + tb, max);
			/* El cuarto byte: ver la cabecera. */
			unsigned a = 0x80 + 4 * x + 3;
			p0[3] = a < w ? cr[a] : 0;
			p1[3] = 0;
		}
	}
}

/* ---- FILL ---- */

bool salsa64_re2_fill_read(const uint8_t *d, struct salsa64_re2_fill *t)
{
	t->src = dw(d, 0x00) & PHYS;
	t->dst = dw(d, 0x04) & PHYS;
	const uint32_t bytes = dw(d, 0x08), dma = dw(d, 0x18);
	t->rows = dw(d, 0x0C);
	t->iters = dw(d, 0x10);
	t->advance = dw(d, 0x1C);
	t->len = (dma & 0xFFF) + 1;
	t->skip = (dma >> 20) & 0xFFF;
	/* El microcódigo trae t->rows filas de cada cuadro (DMA con el salto de la palabra, el
	 * número de filas lo pone el segmento de datos) a 0 y a 0x800, promedia `bytes` bytes y
	 * escribe las filas del segundo. */
	if (((dma >> 12) & 0xFF) != 0 || t->rows < 1 || t->rows * t->len > 2048 || bytes != t->rows * t->len ||
	    (t->len & 7) || (t->skip & 7) || t->iters < 1 || t->iters > 65536 || t->advance != t->rows * (t->len + t->skip) ||
	    ((t->src | t->dst) & 7))
		return false;
	/* La constante de redondeo y la del producto (v10[3] y v10[4]): (s + d) >> 1. */
	return dh(d, 0x26) == 1 && dh(d, 0x28) == 0x8000;
}

void salsa64_re2_fill_row(const uint8_t *src, uint8_t *dst, uint32_t n)
{
#ifdef __SPU__
	/* 16 bytes por instrucción: ⌊(a + b) / 2⌋ = (a & b) + ((a ^ b) >> 1) en cada byte (la
	 * suma no pasa de 255, así que no hay acarreo entre bytes). */
	if (!(((uintptr_t)src | (uintptr_t)dst | n) & 15)) {
		const vec_uchar16 *s = (const vec_uchar16 *)src;
		vec_uchar16 *d = (vec_uchar16 *)dst;
		const vec_uint4 m7f = spu_splats(0x7F7F7F7Fu);
		for (uint32_t i = 0; i < n / 16; i++) {
			vec_uint4 a = (vec_uint4)s[i], b = (vec_uint4)d[i];
			d[i] = (vec_uchar16)spu_add(spu_and(a, b), spu_and(spu_rlmask(spu_xor(a, b), -1), m7f));
		}
		return;
	}
#endif
	uint32_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t a, b;
		memcpy(&a, src + i, 8);
		memcpy(&b, dst + i, 8);
		a = (a & b) + (((a ^ b) >> 1) & 0x7F7F7F7F7F7F7F7Full);
		memcpy(dst + i, &a, 8);
	}
	for (; i < n; i++)
		dst[i] = (uint8_t)((src[i] + dst[i]) >> 1);
}

/* ---- Tarea entera sobre la RDRAM ---- */

static inline bool fits(uint32_t rdram_size, uint64_t addr, uint64_t n)
{
	return addr <= rdram_size && n <= rdram_size - addr;
}

bool salsa64_re2_supported(enum salsa64_jpeg_kind kind, const uint8_t *rdram, uint32_t rdram_size, uint32_t data)
{
	union {
		struct salsa64_re2_resize r;
		struct salsa64_re2_decode d;
		struct salsa64_re2_fill f;
	} t;
	data &= PHYS;
	if (!fits(rdram_size, data, SALSA64_RE2_DATA_BYTES))
		return false;
	switch (kind) {
	case SALSA64_JPEG_RE2_RESIZE: return salsa64_re2_resize_read(rdram + data, &t.r);
	case SALSA64_JPEG_RE2_DECODE: return salsa64_re2_decode_read(rdram + data, &t.d);
	case SALSA64_JPEG_RE2_FILL: return salsa64_re2_fill_read(rdram + data, &t.f);
	default: return false;
	}
}

static void run_resize(uint8_t *rdram, uint32_t size, const struct salsa64_re2_resize *t, uint32_t dram_stack,
		       uint32_t dram_stack_size, struct salsa64_jpeg_result *res)
{
	uint8_t dm[SALSA64_RE2_DMEM];
	struct salsa64_re2_dither st;
	const uint32_t row = 2 * t->width;
	memset(dm, 0, sizeof dm);
	salsa64_re2_resize_begin(&st, dram_stack, dram_stack_size);
	res->out_addr = t->dst;
	res->mb_stride = t->stride;
	res->mb_out = row;
	uint32_t n = 0;
	for (; n < t->rows; n++) {
		uint64_t a = salsa64_re2_resize_src(t, n), o = (uint64_t)t->dst + (uint64_t)n * t->stride;
		if (!fits(size, a, 2 * SALSA64_RE2_SRC_ROW) || !fits(size, o, row))
			break;
		memcpy(dm, rdram + a, 2 * SALSA64_RE2_SRC_ROW);
		salsa64_re2_resize_row(dm, t, &st, n);
		memcpy(rdram + o, dm + SALSA64_RE2_OUT_BASE, row);
	}
	res->bad = n < t->rows;
	res->macroblocks = n;
	res->out_bytes = n ? (n - 1) * t->stride + row : 0;
}

static void run_decode(uint8_t *rdram, uint32_t size, const struct salsa64_re2_decode *t, struct salsa64_jpeg_result *res)
{
	const uint32_t w = t->width, half = t->step / 2;
	res->out_addr = t->dst;
	res->mb_stride = t->step;
	res->mb_out = half + 4 * w;
	uint32_t n = 0;
	for (; n < t->pairs; n++) {
		uint64_t yy = (uint64_t)t->y + (uint64_t)n * 2 * w, cc = (uint64_t)n * (w / 2);
		uint64_t o = (uint64_t)t->dst + (uint64_t)n * t->step;
		if (!fits(size, yy, 2 * w) || !fits(size, t->cb + cc, w) || !fits(size, t->cr + cc, w) ||
		    !fits(size, o, 4 * w) || !fits(size, o + half, 4 * w))
			break;
		salsa64_re2_decode_pair(t, rdram + yy, rdram + t->cb + cc, rdram + t->cr + cc, rdram + o, rdram + o + half);
	}
	res->bad = n < t->pairs;
	res->macroblocks = n;
	res->out_bytes = n ? (n - 1) * t->step + half + 4 * w : 0;
}

static void run_fill(uint8_t *rdram, uint32_t size, const struct salsa64_re2_fill *t, struct salsa64_jpeg_result *res)
{
	const uint32_t pitch = t->len + t->skip;
	res->out_addr = t->dst;
	res->mb_stride = t->advance;
	res->mb_out = (t->rows - 1) * pitch + t->len;
	uint32_t n = 0;
	/* Una pasada es todo o nada: si no caben sus filas en la RDRAM no se hace ninguna. */
	for (; n < t->iters; n++) {
		const uint64_t s0 = (uint64_t)t->src + (uint64_t)n * t->advance, d0 = (uint64_t)t->dst + (uint64_t)n * t->advance;
		if (!fits(size, s0, res->mb_out) || !fits(size, d0, res->mb_out))
			break;
		for (uint32_t r = 0; r < t->rows; r++)
			salsa64_re2_fill_row(rdram + s0 + (uint64_t)r * pitch, rdram + d0 + (uint64_t)r * pitch, t->len);
	}
	res->bad = n < t->iters;
	res->macroblocks = n;
	res->out_bytes = n ? (n - 1) * t->advance + res->mb_out : 0;
}

void salsa64_re2_run(enum salsa64_jpeg_kind kind, uint8_t *rdram, uint32_t rdram_size, uint32_t data,
		     uint32_t dram_stack, uint32_t dram_stack_size, struct salsa64_jpeg_result *res)
{
	union {
		struct salsa64_re2_resize r;
		struct salsa64_re2_decode d;
		struct salsa64_re2_fill f;
	} t;
	memset(res, 0, sizeof *res);
	data &= PHYS;
	if (!fits(rdram_size, data, SALSA64_RE2_DATA_BYTES)) {
		res->bad = true;
		return;
	}
	switch (kind) {
	case SALSA64_JPEG_RE2_RESIZE:
		if (salsa64_re2_resize_read(rdram + data, &t.r))
			run_resize(rdram, rdram_size, &t.r, dram_stack, dram_stack_size, res);
		else
			res->bad = true;
		break;
	case SALSA64_JPEG_RE2_DECODE:
		if (salsa64_re2_decode_read(rdram + data, &t.d))
			run_decode(rdram, rdram_size, &t.d, res);
		else
			res->bad = true;
		break;
	case SALSA64_JPEG_RE2_FILL:
		if (salsa64_re2_fill_read(rdram + data, &t.f))
			run_fill(rdram, rdram_size, &t.f, res);
		else
			res->bad = true;
		break;
	default:
		res->bad = true;
		break;
	}
}
