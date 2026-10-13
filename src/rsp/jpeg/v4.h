/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — cuatro carriles de 32 bits para la IDCT vectorizada de la tarea
 * JPEG (jpeg_v.c). En el SPU son los intrínsecos (mpya, shufb, rotmai…);
 * en el host, lo mismo carril por carril, para que la prueba unitaria
 * compare la versión vectorial con la escalar (jpeg.c). Solo lo que usa
 * jpeg_v.c, con la semántica exacta del SPU:
 *   v4_mul16 / v4_madd16  producto con signo de los 16 bits bajos de cada
 *                         carril (mpy / mpya), suma con vuelta en 32 bits
 *   v4_sra                desplazamiento aritmético a la derecha
 *   v4_perm               carriles 0–7 de (a, b) a elección (shufb)
 */
#ifndef SALSA64_RSP_JPEG_V4_H
#define SALSA64_RSP_JPEG_V4_H

#include <stdint.h>

#ifdef __SPU__
#include <spu_intrinsics.h>

typedef vec_int4 v4;
typedef vec_uchar16 v4pat;

static inline v4 v4_set(int32_t a, int32_t b, int32_t c, int32_t d)
{
	return (v4){ a, b, c, d };
}
static inline v4 v4_splat(int32_t x) { return spu_splats(x); }
static inline v4 v4_add(v4 a, v4 b) { return spu_add(a, b); }
static inline v4 v4_sub(v4 a, v4 b) { return spu_sub(a, b); }
static inline v4 v4_and(v4 a, v4 b) { return spu_and(a, b); }
static inline v4 v4_or(v4 a, v4 b) { return spu_or(a, b); }
static inline v4 v4_mul16(v4 a, v4 b) { return spu_mulo((vec_short8)a, (vec_short8)b); }
static inline v4 v4_madd16(v4 a, v4 b, v4 c) { return spu_madd((vec_short8)a, (vec_short8)b, c); }
static inline v4 v4_sra(v4 a, int n) { return spu_rlmaska(a, -n); }
static inline v4 v4_sl(v4 a, unsigned n) { return (v4)spu_sl((vec_uint4)a, n); }
static inline v4 v4_min(v4 a, v4 b) { return spu_sel(a, b, spu_cmpgt(a, b)); }
static inline v4 v4_max(v4 a, v4 b) { return spu_sel(b, a, spu_cmpgt(a, b)); }
static inline v4 v4_perm(v4 a, v4 b, v4pat p) { return spu_shuffle(a, b, p); }

/* Patrón de v4_perm: el carril j del resultado es el carril l[j] de (a, b). */
static inline v4pat v4_mkpat(unsigned l0, unsigned l1, unsigned l2, unsigned l3)
{
	const unsigned l[4] = { l0, l1, l2, l3 };
	v4pat p = spu_splats((unsigned char)0);
	for (int j = 0; j < 4; j++)
		for (int b = 0; b < 4; b++)
			p = spu_insert((unsigned char)(4 * l[j] + b), p, 4 * j + b);
	return p;
}

/* 16 bytes alineados a 16 → los 8 valores de 16 bits con signo (orden de
 * la N64, como el SPU), en dos v4: los 4 primeros y los 4 siguientes. */
static inline void v4_load16x8(const uint8_t *p, v4 *lo, v4 *hi)
{
	static const vec_uchar16 plo = { 0, 1, 0x80, 0x80, 2, 3, 0x80, 0x80, 4, 5, 0x80, 0x80, 6, 7, 0x80, 0x80 };
	static const vec_uchar16 phi = { 8, 9, 0x80, 0x80, 10, 11, 0x80, 0x80, 12, 13, 0x80, 0x80, 14, 15, 0x80, 0x80 };
	vec_uchar16 q = *(const vec_uchar16 *)p;
	*lo = spu_rlmaska((v4)spu_shuffle(q, q, plo), -16);
	*hi = spu_rlmaska((v4)spu_shuffle(q, q, phi), -16);
}

/* Los 16 bits bajos de los 8 carriles de (a, b), en orden, a 16 bytes
 * alineados. */
static inline void v4_store16x8(uint8_t *p, v4 a, v4 b)
{
	static const vec_uchar16 pk = { 2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31 };
	*(vec_uchar16 *)p = spu_shuffle((vec_uchar16)a, (vec_uchar16)b, pk);
}

#else

typedef struct {
	int32_t l[4];
} v4;
typedef struct {
	uint8_t l[4];
} v4pat;

static inline v4 v4_set(int32_t a, int32_t b, int32_t c, int32_t d)
{
	return (v4){ { a, b, c, d } };
}
static inline v4 v4_splat(int32_t x) { return v4_set(x, x, x, x); }

#define V4_MAP2(name, expr)                       \
	static inline v4 name(v4 a, v4 b)         \
	{                                         \
		v4 r;                             \
		for (int i = 0; i < 4; i++)       \
			r.l[i] = (expr);          \
		return r;                         \
	}
V4_MAP2(v4_add, (int32_t)((uint32_t)a.l[i] + (uint32_t)b.l[i]))
V4_MAP2(v4_sub, (int32_t)((uint32_t)a.l[i] - (uint32_t)b.l[i]))
V4_MAP2(v4_and, a.l[i] & b.l[i])
V4_MAP2(v4_or, a.l[i] | b.l[i])
V4_MAP2(v4_mul16, (int32_t)(int16_t)a.l[i] * (int16_t)b.l[i])
V4_MAP2(v4_min, a.l[i] < b.l[i] ? a.l[i] : b.l[i])
V4_MAP2(v4_max, a.l[i] > b.l[i] ? a.l[i] : b.l[i])
#undef V4_MAP2

static inline v4 v4_madd16(v4 a, v4 b, v4 c)
{
	v4 r;
	for (int i = 0; i < 4; i++)
		r.l[i] = (int32_t)((uint32_t)((int32_t)(int16_t)a.l[i] * (int16_t)b.l[i]) + (uint32_t)c.l[i]);
	return r;
}

static inline v4 v4_sra(v4 a, int n)
{
	for (int i = 0; i < 4; i++)
		a.l[i] >>= n;
	return a;
}

static inline v4 v4_sl(v4 a, unsigned n)
{
	for (int i = 0; i < 4; i++)
		a.l[i] = (int32_t)((uint32_t)a.l[i] << n);
	return a;
}

static inline v4 v4_perm(v4 a, v4 b, v4pat p)
{
	v4 r;
	for (int j = 0; j < 4; j++)
		r.l[j] = p.l[j] < 4 ? a.l[p.l[j]] : b.l[p.l[j] - 4];
	return r;
}

static inline v4pat v4_mkpat(unsigned l0, unsigned l1, unsigned l2, unsigned l3)
{
	return (v4pat){ { (uint8_t)l0, (uint8_t)l1, (uint8_t)l2, (uint8_t)l3 } };
}

static inline void v4_load16x8(const uint8_t *p, v4 *lo, v4 *hi)
{
	for (int i = 0; i < 4; i++) {
		lo->l[i] = (int16_t)(p[2 * i] << 8 | p[2 * i + 1]);
		hi->l[i] = (int16_t)(p[8 + 2 * i] << 8 | p[8 + 2 * i + 1]);
	}
}

static inline void v4_store16x8(uint8_t *p, v4 a, v4 b)
{
	for (int i = 0; i < 4; i++) {
		p[2 * i] = (uint8_t)(a.l[i] >> 8);
		p[2 * i + 1] = (uint8_t)a.l[i];
		p[8 + 2 * i] = (uint8_t)(b.l[i] >> 8);
		p[8 + 2 * i + 1] = (uint8_t)b.l[i];
	}
}

#endif

#endif
