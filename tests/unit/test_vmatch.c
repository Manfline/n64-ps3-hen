/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas del emparejamiento de vértices entre cuadros
 * (src/rdp/vmatch.c) con registros GFX_OP_VGROUP armados a mano, como los
 * deja la HLE con GFX_JOB_INTERP.
 */
#include <math.h>
#include <string.h>

#include "rdp/vmatch.h"
#include "test.h"

#define GCAP 64
#define CCAP 1024

struct frame {
	struct vmatch_group g[GCAP];
	float clip[CCAP][4];
	uint32_t index[4 * GCAP];
	struct vmatch_table t;
	uint8_t cmd[16384];
	uint32_t len;
};

static uint8_t ram[0x10000];  /* RDRAM de prueba (direcciones bajas) */

static void frame_init(struct frame *f)
{
	memset(f, 0, sizeof *f);
	vmatch_table_init(&f->t, f->g, GCAP, f->clip, CCAP, f->index, 4 * GCAP);
}

/* Un G_VTX de n vértices desde `addr` con viewport 160 × 120 (el de la HLE
 * por omisión) y las coordenadas de recorte de `clip`. */
static void add_group(struct frame *f, uint32_t addr, uint32_t v0, uint32_t n, const float (*clip)[4])
{
	struct gfx_vgroup r;
	memset(&r, 0, sizeof r);
	r.h.op = GFX_OP_VGROUP;
	r.h.size = (uint16_t)(sizeof r + 16u * n);
	r.h.a = addr;
	r.h.b = n | v0 << 8 | 16u << 16;
	r.vp[0] = 160.0f, r.vp[1] = 120.0f, r.vp[2] = 160.0f, r.vp[3] = 120.0f;
	memcpy(f->cmd + f->len, &r, sizeof r);
	memcpy(f->cmd + f->len + sizeof r, clip, 16u * n);
	f->len += r.h.size;
}

static void finish(struct frame *f)
{
	struct gfx_cmd c = { GFX_OP_END, sizeof c, 0, 0, 0 };
	memcpy(f->cmd + f->len, &c, sizeof c);
	f->len += sizeof c;
	vmatch_build(&f->t, f->cmd, f->len, ram, sizeof ram);
}

/* El vértice de salida de la coordenada de recorte c (viewport 160 × 120,
 * z/w = 0,5), como gfx_vtx_out. */
static struct gfx_vertex out_of(const float c[4], uint32_t pad)
{
	struct gfx_vertex v;
	memset(&v, 0, sizeof v);
	v.x = 160.0f * c[3] + 160.0f * c[0];
	v.y = 120.0f * c[3] - 120.0f * c[1];
	v.z = 0.5f * c[3];
	v.w = c[3];
	v.pad = pad;
	return v;
}

static bool near(float a, float b)
{
	return fabsf(a - b) < 1e-3f;
}

static struct frame fa, fb;

/* El mismo modelo dos veces (se distinguen por orden), uno que aparece, uno
 * que se movió de más y la posición anterior de cada vértice de salida,
 * también la de uno del recorte cercano. */
static void vmatch_pairs_and_positions(void)
{
	memset(ram, 0, sizeof ram);
	static const float m0[2][4] = { { 0.0f, 0.0f, 1.0f, 2.0f }, { 0.5f, 0.0f, 1.0f, 2.0f } };
	static const float m1[2][4] = { { -1.0f, 0.5f, 1.0f, 2.0f }, { -0.5f, 0.5f, 1.0f, 2.0f } };
	static const float far0[1][4] = { { 0.0f, 0.0f, 1.0f, 1.0f } };
	frame_init(&fa);
	add_group(&fa, 0x1000, 0, 2, m0);  /* árbol 1 */
	add_group(&fa, 0x1000, 0, 2, m1);  /* árbol 2: misma lista */
	add_group(&fa, 0x2000, 4, 1, far0);
	finish(&fa);
	CHECK_EQ(fa.t.ngroups, 3);
	CHECK_EQ(fa.g[0].occ, 0);
	CHECK_EQ(fa.g[1].occ, 1);

	static const float n0[2][4] = { { 0.1f, 0.0f, 1.0f, 2.0f }, { 0.6f, 0.0f, 1.0f, 2.0f } };
	static const float n1[2][4] = { { -0.9f, 0.5f, 1.0f, 2.0f }, { -0.4f, 0.5f, 1.0f, 2.0f } };
	static const float far1[1][4] = { { 0.9f, 0.0f, 1.0f, 1.0f } };  /* 144 px en x */
	static const float new0[1][4] = { { 0.0f, 0.0f, 1.0f, 1.0f } };
	frame_init(&fb);
	add_group(&fb, 0x1000, 0, 2, n0);
	add_group(&fb, 0x1000, 0, 2, n1);
	add_group(&fb, 0x2000, 4, 1, far1);
	add_group(&fb, 0x3000, 8, 1, new0);
	finish(&fb);
	CHECK_EQ(fb.t.ngroups, 4);
	struct vmatch_stats st;
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.g[0].pair, 0);
	CHECK_EQ(fb.g[1].pair, 1);  /* el segundo árbol con el segundo */
	CHECK_EQ(fb.g[2].pair, -1); /* se movió 144 px: corte */
	CHECK_EQ(fb.g[3].pair, -1); /* nuevo */
	CHECK_EQ(st.found, 3);
	CHECK_EQ(st.moved, 1);
	CHECK_EQ(st.paired, 2);
	CHECK(!st.cut);

	struct gfx_vertex v[5] = {
		out_of(n0[1], GFX_VGID(1u, 1u)), out_of(n1[0], GFX_VGID(2u, 0u)), out_of(far1[0], GFX_VGID(3u, 0u)),
		out_of(new0[0], 0), out_of(n0[0], GFX_VCLIP_ID(1u, 0u, 1u)),
	};
	float prev[5][4];
	CHECK_EQ(vmatch_prev(&fa.t, &fb.t, &st, v, 5, prev), 2);
	struct gfx_vertex want = out_of(m0[1], 0);
	CHECK(near(prev[0][0], want.x) && near(prev[0][1], want.y) && near(prev[0][3], want.w));
	CHECK(near(prev[0][2], 0.5f * want.w));  /* z/w de ahora */
	want = out_of(m1[0], 0);
	CHECK(near(prev[1][0], want.x) && near(prev[1][1], want.y));
	CHECK(near(prev[2][0], v[2].x) && near(prev[3][0], v[3].x));  /* sin pareja: donde está */
	/* Recorte cercano entre los lugares 0 y 1 del grupo 1: t sale de z + w de
	 * ahora (n0: 3 y 3 → iguales: sin t). Con otro grupo de prueba abajo. */
	CHECK(near(prev[4][0], v[4].x));
	static const float q0[2][4] = { { 0, 0, -1.5f, 1.0f }, { 1, 0, 1.0f, 3.0f } };  /* z + w: −0,5 y 4 */
	static const float q1[2][4] = { { 0.2f, 0, -1.5f, 1.0f }, { 0.8f, 0, 1.0f, 3.0f } };
	frame_init(&fa);
	add_group(&fa, 0x6000, 0, 2, q0);
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x6000, 0, 2, q1);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	float t = -0.5f / (-0.5f - 4.0f), cq[4];
	for (int q = 0; q < 4; q++)
		cq[q] = q1[0][q] + (q1[1][q] - q1[0][q]) * t;
	struct gfx_vertex g = out_of(cq, GFX_VCLIP_ID(1u, 0u, 1u));
	CHECK_EQ(vmatch_prev(&fa.t, &fb.t, &st, &g, 1, prev), 1);
	for (int q = 0; q < 4; q++)
		cq[q] = q0[0][q] + (q0[1][q] - q0[0][q]) * t;
	want = out_of(cq, 0);
	CHECK(near(prev[0][0], want.x) && near(prev[0][1], want.y) && near(prev[0][3], want.w));
}

/* Perfect Dark copia los vértices a otra dirección en cada cuadro: se
 * encuentran por el contenido en la RDRAM. Y un cuadro con casi todo nuevo
 * es un corte. */
static void vmatch_content_and_cut(void)
{
	static const float a[3][4] = { { 0, 0, 1, 2 }, { 0.2f, 0, 1, 2 }, { 0, 0.2f, 1, 2 } };
	memset(ram, 0, sizeof ram);
	for (int i = 0; i < 48; i++)
		ram[0x4000 + i] = ram[0x4800 + i] = (uint8_t)(i * 7 + 1);  /* la misma lista en los dos búferes */
	frame_init(&fa);
	add_group(&fa, 0x4000, 0, 3, a);
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x4800, 0, 3, a);
	finish(&fb);
	struct vmatch_stats st;
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.g[0].pair, 0);
	CHECK_EQ(st.by_content, 1);
	CHECK(!st.cut);

	frame_init(&fb);
	add_group(&fb, 0x4000, 0, 3, a);
	add_group(&fb, 0x5000, 3, 3, a);
	add_group(&fb, 0x5100, 6, 3, a);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK(st.cut);
	struct gfx_vertex v = out_of(a[1], GFX_VGID(1u, 1u));
	float prev[1][4];
	CHECK_EQ(vmatch_prev(&fa.t, &fb.t, &st, &v, 1, prev), 0);

	vmatch_pair(NULL, &fb.t, 120.0f, &st);
	CHECK_EQ(st.found, 0);
	CHECK_EQ(vmatch_prev(NULL, &fb.t, &st, &v, 1, prev), 0);
	CHECK(near(prev[0][0], v.x));
}

void t_suite_vmatch(void)
{
	RUN(vmatch_pairs_and_positions);
	RUN(vmatch_content_and_cut);
}
