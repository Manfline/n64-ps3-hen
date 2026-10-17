/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — la tarea JPEG en 4 carriles de 32 bits (lo que corre en el SPE).
 * Ver jpeg_v.h. Mismas operaciones que jpeg.c: productos de dos en dos con
 * un solo redondeo, sumas saturadas a 16 bits y el color con el piso de
 * cada producto; las sumas de dos productos caben en 32 bits (|valor| ≤
 * 32768 y constantes ≤ 32138), así que el resultado es el mismo byte a byte.
 *   descuantizar  4 coeficientes por v4, en el orden en que llegan
 *                 (zigzag): producto de 16 × 16, recorte a 16 bits y ×16
 *                 con vuelta en 16 (<< 20 y >> 16 aritmético)
 *   filas         ocho transformadas a la vez en dos v4 (los carriles son
 *                 las filas del bloque): el v4 i de la entrada tiene la
 *                 columna i, reunida del zigzag con un shufb por valor
 *   columnas      igual, con los carriles las columnas: la salida de las
 *                 filas se reordena (otro shufb por valor)
 *   color         4 píxeles por v4: la croma de cada par de píxeles
 *                 repetida (0 0 1 1, 2 2 3 3); los productos de la croma,
 *                 una vez por fila de croma
 */
#include "rsp/jpeg/jpeg_v.h"

#include <stdbool.h>

#include "rsp/jpeg/jpeg.h"

static v4pat ins[4][4];   /* ins[j][s]: lleva el carril s del segundo v4 al carril j del primero */
static uint8_t src_v[64]; /* coeficiente natural i: en d[src_v[i]], carril src_l[i] */
static uint8_t src_l[64];

void salsa64_jpeg_v_init(void)
{
	for (unsigned j = 0; j < 4; j++)
		for (unsigned s = 0; s < 4; s++) {
			unsigned l[4] = { 0, 1, 2, 3 };
			l[j] = 4 + s;
			ins[j][s] = v4_mkpat(l[0], l[1], l[2], l[3]);
		}
	for (int i = 0; i < 64; i++) {
		src_v[i] = salsa64_jpeg_zigzag[i] / 4;
		src_l[i] = salsa64_jpeg_zigzag[i] % 4;
	}
}

void salsa64_jpeg_qv_init(struct salsa64_jpeg_qv *qv, const int16_t q[3][64])
{
	for (int t = 0; t < 3; t++)
		for (int j = 0; j < 16; j++)
			qv->q[t][j] = v4_set(q[t][4 * j], q[t][4 * j + 1], q[t][4 * j + 2], q[t][4 * j + 3]);
}

static inline v4 clamp16(v4 x)
{
	return v4_min(v4_max(x, v4_splat(-32768)), v4_splat(32767));
}

/* ⌊(a·ka + b·kb + 2^14) / 2^15⌋ recortado a 16 bits: VMULF + VMACF. */
static inline v4 mac2(v4 a, int ka, v4 b, int kb)
{
	v4 acc = v4_madd16(b, v4_splat(kb), v4_mul16(a, v4_splat(ka)));
	return clamp16(v4_sra(v4_add(acc, v4_splat(0x4000)), 15));
}

static inline v4 sadd(v4 a, v4 b) { return clamp16(v4_add(a, b)); }
static inline v4 ssub(v4 a, v4 b) { return clamp16(v4_sub(a, b)); }

/* La IDCT unidimensional de jpeg.c (idct1), en cuatro carriles. */
static void idct1_v(const v4 x[8], v4 o[8], bool last)
{
	v4 v10 = mac2(x[3], 27246, x[5], 18205);
	v4 v11 = mac2(x[7], 6393, x[1], 32138);
	v4 v8 = mac2(x[1], 6393, x[7], -32138);
	v4 v9 = mac2(x[5], 27246, x[3], -18205);
	v4 v6 = mac2(x[0], 23170, x[4], -23170);
	v4 v5 = ssub(v11, v10);
	v4 v4_ = ssub(v8, v9);
	v4 v12 = sadd(v8, v9);
	v4 v15 = sadd(v11, v10);
	v4 v13 = mac2(v5, 23170, v4_, -23170);
	v4 v14 = mac2(v5, 23170, v4_, 23170);
	v4 w4 = mac2(x[0], 23170, x[4], 23170);
	v4 w5 = mac2(x[6], 12540, x[2], 30274);
	v4 v7 = mac2(x[2], 12540, x[6], -30274);
	v4 a0 = sadd(w4, w5), a3 = ssub(w4, w5), a1 = sadd(v6, v7), a2 = ssub(v6, v7);
	static const uint8_t lo[4] = { 0, 3, 1, 2 }, hi[4] = { 7, 4, 6, 5 };
	const v4 p[4] = { a0, a3, a1, a2 }, q[4] = { v15, v12, v14, v13 };
	for (int j = 0; j < 4; j++) {
		if (!last) {
			o[lo[j]] = sadd(p[j], q[j]);
			o[hi[j]] = ssub(p[j], q[j]);
		} else {
			o[lo[j]] = v4_sra(v4_add(v4_add(p[j], q[j]), v4_splat(2)), 2);
			o[hi[j]] = v4_sra(v4_add(v4_sub(p[j], q[j]), v4_splat(2)), 2);
		}
	}
}

/* Un bloque: 128 bytes de coeficientes (zigzag) → out[2y], out[2y + 1]: la
 * fila y de valores ×16 (x 0–3 y 4–7). */
static void block_v(const uint8_t *src, const v4 q[16], v4 out[16])
{
	v4 d[16], x[8][2], o[8][2], y[8][2], z[8][2];
	for (int j = 0; j < 8; j++) {
		v4 c[2];
		v4_load16x8(src + 16 * j, &c[0], &c[1]);
		for (int h = 0; h < 2; h++) {
			v4 p = clamp16(v4_mul16(c[h], q[2 * j + h]));
			d[2 * j + h] = v4_sra(v4_sl(p, 20), 16);
		}
	}
	/* x[i][g] = columna i de las filas 4g–4g+3 (carril j: fila 4g + j). */
	for (int i = 0; i < 8; i++)
		for (int g = 0; g < 2; g++) {
			v4 a = v4_splat(0);
			for (int j = 0; j < 4; j++) {
				int n = 8 * (4 * g + j) + i;
				a = v4_perm(a, d[src_v[n]], ins[j][src_l[n]]);
			}
			x[i][g] = a;
		}
	for (int g = 0; g < 2; g++) {
		v4 xi[8], oi[8];
		for (int i = 0; i < 8; i++)
			xi[i] = x[i][g];
		idct1_v(xi, oi, false);
		for (int k = 0; k < 8; k++)
			o[k][g] = oi[k];
	}
	/* y[i][h] = la fila i de la salida de las filas, columnas 4h–4h+3 (carril j: columna 4h + j). */
	for (int i = 0; i < 8; i++)
		for (int h = 0; h < 2; h++) {
			v4 a = v4_splat(0);
			for (int j = 0; j < 4; j++)
				a = v4_perm(a, o[4 * h + j][i >> 2], ins[j][i & 3]);
			y[i][h] = a;
		}
	for (int h = 0; h < 2; h++) {
		v4 yi[8], zi[8];
		for (int i = 0; i < 8; i++)
			yi[i] = y[i][h];
		idct1_v(yi, zi, true);
		for (int m = 0; m < 8; m++)
			z[m][h] = zi[m];
	}
	for (int m = 0; m < 8; m++) {
		out[2 * m] = z[m][0];
		out[2 * m + 1] = z[m][1];
	}
}

/* Canal en dieciseisavos → sus 5 bits altos, en su lugar (0xF80). */
static inline v4 chan(v4 x)
{
	return v4_and(v4_min(v4_max(x, v4_splat(0)), v4_splat(0xFF0)), v4_splat(0xF80));
}

/* Fila de 16 píxeles: ya, yb las filas de los dos bloques Y (x 0–7 y 8–15,
 * dos v4 cada una); u, v la fila de croma (8 valores, uno por cada dos
 * píxeles). */
static void row_v(const v4 ya[2], const v4 yb[2], const v4 u[2], const v4 v[2], uint8_t *dst)
{
	v4 tr[2], tg[2], tb[2], px[4];
	for (int h = 0; h < 2; h++) {
		/* ⌊x · 46819 / 65536⌋ = x + ⌊−x · 18717 / 65536⌋ y ⌊x · 50659 / 65536⌋ = x + ⌊−x · 14877 / 65536⌋
		 * (las constantes sin signo no caben en un producto con signo de 16 bits). */
		v4 fr = clamp16(v4_sra(v4_mul16(v[h], v4_splat(SALSA64_JPEG_CR_R)), 16));
		v4 gu = clamp16(v4_sra(v4_mul16(u[h], v4_splat(SALSA64_JPEG_CB_G)), 16));
		v4 gv = clamp16(v4_add(v[h], v4_sra(v4_mul16(v[h], v4_splat(-(65536 - SALSA64_JPEG_CR_G))), 16)));
		v4 fb = clamp16(v4_add(u[h], v4_sra(v4_mul16(u[h], v4_splat(-(65536 - SALSA64_JPEG_CB_B))), 16)));
		tr[h] = clamp16(v4_add(v[h], fr));
		tg[h] = clamp16(v4_add(gu, gv));
		tb[h] = clamp16(v4_add(u[h], fb));
	}
	for (int p = 0; p < 4; p++) {
		int h = p >> 1;
		static const unsigned dl[2][4] = { { 0, 0, 1, 1 }, { 2, 2, 3, 3 } };
		const unsigned *dp = dl[p & 1];
		v4pat dpat = v4_mkpat(dp[0], dp[1], dp[2], dp[3]);
		v4 yy = clamp16(v4_add(p < 2 ? ya[p] : yb[p - 2], v4_splat(2048)));
		v4 r = chan(clamp16(v4_add(v4_perm(tr[h], tr[h], dpat), yy)));
		v4 g = chan(clamp16(v4_sub(yy, v4_perm(tg[h], tg[h], dpat))));
		v4 b = chan(clamp16(v4_add(v4_perm(tb[h], tb[h], dpat), yy)));
		px[p] = v4_or(v4_or(v4_sl(r, 4), v4_sra(g, 1)), v4_or(v4_sra(b, 6), v4_splat(1)));
	}
	v4_store16x8(dst, px[0], px[1]);
	v4_store16x8(dst + 16, px[2], px[3]);
}

void salsa64_jpeg_mb_v(uint32_t mode, const struct salsa64_jpeg_qv *qv, const uint8_t *in, uint8_t *out)
{
	unsigned blocks = mode + 4;  /* los dos últimos son U y V */
	v4 px[6][16];
	for (unsigned b = 0; b < blocks; b++)
		block_v(in + b * 128, qv->q[b + 2 < blocks ? 0 : b + 2 == blocks ? 1 : 2], px[b]);
	const v4 *u = px[blocks - 2], *v = px[blocks - 1];
	if (mode == 0) {
		for (int r = 0; r < 8; r++)
			row_v(&px[0][2 * r], &px[1][2 * r], &u[2 * r], &v[2 * r], out + 32 * r);
	} else {
		/* Filas 0–7 de Y0/Y1, 8–15 de Y2/Y3; la croma, una fila por cada
		 * dos. */
		for (int r = 0; r < 16; r++) {
			int blk = r < 8 ? 0 : 2, row = r & 7, c = r >> 1;
			row_v(&px[blk][2 * row], &px[blk + 1][2 * row], &u[2 * c], &v[2 * c], out + 32 * r);
		}
	}
}
