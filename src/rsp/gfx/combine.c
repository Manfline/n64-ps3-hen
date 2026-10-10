/*
 * Salsa64 — combiner de color de la N64 → combinación lineal para el RSX.
 *
 * El combiner del RDP calcula, por ciclo y por canal, (A − B) × C + D con
 * entradas que son texels (TEXEL0/1), el color de sombreado (SHADE),
 * constantes (PRIM, ENV, 1, 0…) y, en el segundo ciclo, el resultado del
 * primero (COMBINED). Aquí se evalúa de forma simbólica: cada valor es un
 * polinomio por canal sobre los monomios
 *
 *     {1, T0, T0a, T1, T1a} × {1, S, Sa}
 *
 * (T0a = alfa de TEXEL0, S = SHADE del mismo canal, Sa = alfa de SHADE), con
 * las constantes ya como números. El resultado se deja en gfx_draw.k, que
 * el RSX evalúa así: el vertex program arma cada término X_k = k0 + k1·C +
 * k2·C.aaaa con el color del vértice y el fragment program suma T0·X0 +
 * T0.aaaa·X1 + T1·X2 + T1.aaaa·X3 + X4. Es exacto mientras no se
 * multipliquen dos texturas ni el sombreado por sí mismo; esos productos se
 * aproximan quedándose con el primer factor y se cuentan.
 *
 * En el canal alfa, "el mismo canal" es el alfa: ahí T0 y S se guardan
 * siempre como T0a y Sa, así que COMBINED_ALPHA se puede llevar tal cual a
 * los canales de color.
 *
 * Codificación de G_SETCOMBINE y entradas de cada slot: n64brew.dev
 * (Reality Display Processor/Commands, "Set Combine Mode"). En un ciclo se
 * usa la segunda mitad del combiner.
 */
#include <string.h>

#include "rsp/gfx/hle.h"

enum { TX_ONE, TX_T0, TX_T0A, TX_T1, TX_T1A, TX_N };
enum { SH_ONE, SH_S, SH_SA, SH_N };

typedef struct {
	float c[TX_N][SH_N];
} poly;

struct cc_ctx {
	float prim[4], env[4];
	float prim_lod, lod, noise, k4, k5;
	bool approx;
	bool swap;  /* segundo ciclo de 2CYCLE: TEXEL0 lee el texel de la tile+1 */
};

static poly p_const(float v)
{
	poly p;
	memset(&p, 0, sizeof p);
	p.c[TX_ONE][SH_ONE] = v;
	return p;
}

static poly p_tex(int tx, const struct cc_ctx *x)
{
	static const int swapped[TX_N] = { TX_ONE, TX_T1, TX_T1A, TX_T0, TX_T0A };
	poly p;
	memset(&p, 0, sizeof p);
	p.c[x->swap ? swapped[tx] : tx][SH_ONE] = 1.0f;
	return p;
}

static poly p_mono(int tx, int sh)
{
	poly p = p_const(0.0f);
	p.c[tx][sh] = 1.0f;
	return p;
}

static poly p_sub(poly a, const poly *b)
{
	for (int t = 0; t < TX_N; t++)
		for (int s = 0; s < SH_N; s++)
			a.c[t][s] -= b->c[t][s];
	return a;
}

static poly p_add(poly a, const poly *b)
{
	for (int t = 0; t < TX_N; t++)
		for (int s = 0; s < SH_N; s++)
			a.c[t][s] += b->c[t][s];
	return a;
}

static poly p_mul(const poly *a, const poly *b, struct cc_ctx *x)
{
	poly r = p_const(0.0f);
	for (int ta = 0; ta < TX_N; ta++)
		for (int sa = 0; sa < SH_N; sa++) {
			float va = a->c[ta][sa];
			if (va == 0.0f)
				continue;
			for (int tb = 0; tb < TX_N; tb++)
				for (int sb = 0; sb < SH_N; sb++) {
					float vb = b->c[tb][sb];
					if (vb == 0.0f)
						continue;
					int t = ta, s = sa;
					if (tb != TX_ONE) {
						if (ta != TX_ONE)
							x->approx = true;  /* textura × textura */
						else
							t = tb;
					}
					if (sb != SH_ONE) {
						if (sa != SH_ONE)
							x->approx = true;  /* sombreado × sombreado */
						else
							s = sb;
					}
					r.c[t][s] += va * vb;
				}
		}
	return r;
}

/* Entradas de un canal de color (lane 0–2). comb = COMBINED de ese canal y
 * comb_a = COMBINED del canal alfa. */
static poly in_rgb(int slot, unsigned v, int lane, const poly *comb, const poly *comb_a, struct cc_ctx *x)
{
	switch (v) {
	case 0: return *comb;
	case 1: return p_tex(TX_T0, x);
	case 2: return p_tex(TX_T1, x);
	case 3: return p_const(x->prim[lane]);
	case 4: return p_mono(TX_ONE, SH_S);
	case 5: return p_const(x->env[lane]);
	default: break;
	}
	switch (slot) {
	case 'a':  /* 6 ONE, 7 NOISE, 8–15 cero */
		return p_const(v == 6 ? 1.0f : v == 7 ? x->noise : 0.0f);
	case 'b':  /* 6 CENTER, 7 K4, 8–15 cero */
		return p_const(v == 7 ? x->k4 : 0.0f);
	case 'c':
		switch (v) {
		case 6: return p_const(0.0f);                 /* SCALE (clave) */
		case 7: return *comb_a;                       /* COMBINED_ALPHA */
		case 8: return p_tex(TX_T0A, x);
		case 9: return p_tex(TX_T1A, x);
		case 10: return p_const(x->prim[3]);
		case 11: return p_mono(TX_ONE, SH_SA);
		case 12: return p_const(x->env[3]);
		case 13: return p_const(x->lod);              /* LOD_FRACTION */
		case 14: return p_const(x->prim_lod);         /* PRIM_LOD_FRAC */
		case 15: return p_const(x->k5);
		default: return p_const(0.0f);
		}
	default:   /* 'd': 6 ONE, 7 cero */
		return p_const(v == 6 ? 1.0f : 0.0f);
	}
}

static poly in_alpha(int slot, unsigned v, const poly *comb_a, struct cc_ctx *x)
{
	if (slot == 'c') {
		switch (v) {
		case 0: return p_const(x->lod);
		case 6: return p_const(x->prim_lod);
		case 7: return p_const(0.0f);
		default: break;
		}
	} else if (v == 0) {
		return *comb_a;
	}
	switch (v) {
	case 1: return p_tex(TX_T0A, x);
	case 2: return p_tex(TX_T1A, x);
	case 3: return p_const(x->prim[3]);
	case 4: return p_mono(TX_ONE, SH_SA);
	case 5: return p_const(x->env[3]);
	case 6: return p_const(1.0f);
	default: return p_const(0.0f);
	}
}

struct cc_cycle {
	unsigned a, b, c, d;      /* RGB */
	unsigned aa, ab, ac, ad;  /* alfa */
};

/* Campos de G_SETCOMBINE (n64brew.dev). */
static void unpack(uint32_t hi, uint32_t lo, struct cc_cycle cyc[2])
{
	cyc[0].a = (hi >> 20) & 0xF;
	cyc[0].c = (hi >> 15) & 0x1F;
	cyc[0].aa = (hi >> 12) & 0x7;
	cyc[0].ac = (hi >> 9) & 0x7;
	cyc[1].a = (hi >> 5) & 0xF;
	cyc[1].c = hi & 0x1F;
	cyc[0].b = (lo >> 28) & 0xF;
	cyc[1].b = (lo >> 24) & 0xF;
	cyc[1].aa = (lo >> 21) & 0x7;
	cyc[1].ac = (lo >> 18) & 0x7;
	cyc[0].d = (lo >> 15) & 0x7;
	cyc[0].ab = (lo >> 12) & 0x7;
	cyc[0].ad = (lo >> 9) & 0x7;
	cyc[1].d = (lo >> 6) & 0x7;
	cyc[1].ab = (lo >> 3) & 0x7;
	cyc[1].ad = lo & 0x7;
}

static void run_cycle(const struct cc_cycle *cy, poly out[4], const poly comb[4], struct cc_ctx *x)
{
	for (int lane = 0; lane < 3; lane++) {
		poly a = in_rgb('a', cy->a, lane, &comb[lane], &comb[3], x);
		poly b = in_rgb('b', cy->b, lane, &comb[lane], &comb[3], x);
		poly c = in_rgb('c', cy->c, lane, &comb[lane], &comb[3], x);
		poly d = in_rgb('d', cy->d, lane, &comb[lane], &comb[3], x);
		a = p_sub(a, &b);
		a = p_mul(&a, &c, x);
		out[lane] = p_add(a, &d);
	}
	poly a = in_alpha('a', cy->aa, &comb[3], x);
	poly b = in_alpha('b', cy->ab, &comb[3], x);
	poly c = in_alpha('c', cy->ac, &comb[3], x);
	poly d = in_alpha('d', cy->ad, &comb[3], x);
	a = p_sub(a, &b);
	a = p_mul(&a, &c, x);
	out[3] = p_add(a, &d);
}

static void rgba(uint32_t c, float out[4])
{
	out[0] = (float)(c >> 24) / 255.0f;
	out[1] = (float)((c >> 16) & 0xFF) / 255.0f;
	out[2] = (float)((c >> 8) & 0xFF) / 255.0f;
	out[3] = (float)(c & 0xFF) / 255.0f;
}

/* Monomio de textura → término de gfx_draw.k. */
static const int tx_term[TX_N] = { GFX_K_ONE, GFX_K_T0, GFX_K_T0A, GFX_K_T1, GFX_K_T1A };

bool gfx_combine(const struct gfx_state *g, struct gfx_draw *d)
{
	struct cc_ctx x;
	memset(&x, 0, sizeof x);
	rgba(g->prim, x.prim);
	rgba(g->env, x.env);
	x.prim_lod = g->prim_lod / 255.0f;
	x.noise = 0.5f;

	struct cc_cycle cyc[2];
	unpack(g->cc_hi, g->cc_lo, cyc);

	poly zero = p_const(0.0f);
	poly comb[4] = { zero, zero, zero, zero }, out[4];
	unsigned cycle = (g->oh >> G_MDSFT_CYCLETYPE) & 3;
	if (cycle == G_CYC_2CYCLE) {
		run_cycle(&cyc[0], comb, comb, &x);
		/* Rareza del pipeline del RDP (n64brew.dev): en el segundo ciclo
		 * TEXEL0 da el texel de la tile siguiente y TEXEL1 el del píxel
		 * siguiente (≈ TEXEL0). */
		x.swap = true;
		run_cycle(&cyc[1], out, comb, &x);
	} else {
		run_cycle(&cyc[1], out, comb, &x);
	}

	memset(d->k, 0, sizeof d->k);
	bool t0 = false, t1 = false;
	for (int lane = 0; lane < 4; lane++)
		for (int t = 0; t < TX_N; t++)
			for (int s = 0; s < SH_N; s++) {
				float v = out[lane].c[t][s];
				d->k[tx_term[t]][s][lane] = v;
				if (v != 0.0f) {
					t0 |= t == TX_T0 || t == TX_T0A;
					t1 |= t == TX_T1 || t == TX_T1A;
				}
			}
	d->ntex = t1 ? 2 : t0 ? 1 : 0;
	return x.approx;
}
