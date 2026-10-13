/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — la tarea JPEG en 4 carriles de 32 bits (lo que corre en el SPE).
 * Ver jpeg_v.h. Mismas operaciones que jpeg.c, en el mismo orden donde
 * importa (sumas con vuelta en 32 bits: el orden no cambia el resultado):
 *   descuantizar  4 coeficientes por v4, en el orden en que llegan
 *                 (zigzag): producto de 16 × 16, recorte a 16 bits y ×16
 *                 con vuelta en 16 (<< 20 y >> 16 aritmético)
 *   filas         por fila y, los 8 valores de salida en dos v4 (x 0–3 y
 *                 4–7): Σk c[y][k]·idct[x][k] con mpya; c[y][k] se toma del
 *                 v4 descuantizado que lo tiene (zigzag) repetido en los 4
 *                 carriles, así el zigzag no cuesta una pasada aparte
 *   columnas      por fila de salida y, Σk idct[y][k]·r[k][x] con la
 *                 constante repetida en los 4 carriles
 *   color         4 píxeles por v4: la croma de cada par de píxeles
 *                 repetida (0 0 1 1, 2 2 3 3); los productos de la croma,
 *                 una vez por fila de croma
 * Unas 700 operaciones por bloque contra unas 10 000 del código escalar en
 * el SPU (cargas y escrituras de 16 bits dentro de un quadword).
 */
#include "rsp/jpeg/jpeg_v.h"

#include "rsp/jpeg/jpeg.h"

static v4 kc[8][2];       /* filas: idct[x][k] para x 0–3 y 4–7 */
static v4 ks[8][8];       /* columnas: idct[y][k] en los 4 carriles */
static v4pat splat[4];    /* el carril l en los 4 */
static v4pat dup[2];      /* 0 0 1 1 y 2 2 3 3 */
static uint8_t src_v[64]; /* coeficiente natural i: en d[src_v[i]], carril src_l[i] */
static uint8_t src_l[64];

void salsa64_jpeg_v_init(void)
{
	for (int k = 0; k < 8; k++) {
		for (int h = 0; h < 2; h++)
			kc[k][h] = v4_set(salsa64_jpeg_idct[4 * h][k], salsa64_jpeg_idct[4 * h + 1][k],
					  salsa64_jpeg_idct[4 * h + 2][k], salsa64_jpeg_idct[4 * h + 3][k]);
		for (int y = 0; y < 8; y++)
			ks[y][k] = v4_splat(salsa64_jpeg_idct[y][k]);
	}
	for (unsigned l = 0; l < 4; l++)
		splat[l] = v4_mkpat(l, l, l, l);
	dup[0] = v4_mkpat(0, 0, 1, 1);
	dup[1] = v4_mkpat(2, 2, 3, 3);
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

/* Un bloque: 128 bytes de coeficientes (zigzag) → out[2y], out[2y + 1]: la
 * fila y de valores ×16 (x 0–3 y 4–7). */
static void block_v(const uint8_t *src, const v4 q[16], v4 out[16])
{
	v4 d[16], r[16];
	for (int j = 0; j < 8; j++) {
		v4 c[2];
		v4_load16x8(src + 16 * j, &c[0], &c[1]);
		for (int h = 0; h < 2; h++) {
			v4 p = clamp16(v4_mul16(c[h], q[2 * j + h]));
			d[2 * j + h] = v4_sra(v4_sl(p, 20), 16);
		}
	}
	for (int y = 0; y < 8; y++) {
		v4 a = v4_splat(1 << (SALSA64_JPEG_ROW_SHIFT - 1)), b = a;
		for (int k = 0; k < 8; k++) {
			int i = 8 * y + k;
			v4 s = v4_perm(d[src_v[i]], d[src_v[i]], splat[src_l[i]]);
			a = v4_madd16(s, kc[k][0], a);
			b = v4_madd16(s, kc[k][1], b);
		}
		r[2 * y] = clamp16(v4_sra(a, SALSA64_JPEG_ROW_SHIFT));
		r[2 * y + 1] = clamp16(v4_sra(b, SALSA64_JPEG_ROW_SHIFT));
	}
	for (int y = 0; y < 8; y++) {
		v4 a = v4_splat(1 << (SALSA64_JPEG_COL_SHIFT - 1)), b = a;
		for (int k = 0; k < 8; k++) {
			a = v4_madd16(r[2 * k], ks[y][k], a);
			b = v4_madd16(r[2 * k + 1], ks[y][k], b);
		}
		out[2 * y] = clamp16(v4_sra(a, SALSA64_JPEG_COL_SHIFT));
		out[2 * y + 1] = clamp16(v4_sra(b, SALSA64_JPEG_COL_SHIFT));
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
		tr[h] = v4_sra(v4_mul16(v[h], v4_splat(SALSA64_JPEG_CR_R)), 14);
		tg[h] = v4_add(v4_sra(v4_mul16(u[h], v4_splat(SALSA64_JPEG_CB_G)), 14),
			       v4_sra(v4_mul16(v[h], v4_splat(SALSA64_JPEG_CR_G)), 14));
		tb[h] = v4_sra(v4_mul16(u[h], v4_splat(SALSA64_JPEG_CB_B)), 14);
	}
	for (int p = 0; p < 4; p++) {
		int h = p >> 1;
		v4pat dp = dup[p & 1];
		v4 yy = v4_add(p < 2 ? ya[p] : yb[p - 2], v4_splat(2048));
		v4 r = chan(v4_add(yy, v4_perm(tr[h], tr[h], dp)));
		v4 g = chan(v4_sub(yy, v4_perm(tg[h], tg[h], dp)));
		v4 b = chan(v4_add(yy, v4_perm(tb[h], tb[h], dp)));
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
