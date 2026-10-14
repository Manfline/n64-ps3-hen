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
	_Alignas(16) uint8_t mem[64 << 10];
	struct vmatch_table t;
	uint8_t cmd[16384];
	uint32_t len;
};

static uint8_t ram[0x10000];  /* RDRAM de prueba (direcciones bajas) */

static void frame_init(struct frame *f)
{
	memset(f, 0, sizeof *f);
	CHECK(vmatch_table_bytes(GCAP, CCAP) <= sizeof f->mem);
	vmatch_table_init(&f->t, f->mem, GCAP, CCAP);
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
	vmatch_build(&f->t, NULL, f->cmd, f->len, ram, sizeof ram);
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
	ram[0x3000] = 1;  /* el grupo nuevo no tiene el contenido de otro */
	static const float m0[2][4] = { { 0.0f, 0.0f, 1.0f, 2.0f }, { 0.5f, 0.0f, 1.0f, 2.0f } };
	static const float m1[2][4] = { { -1.0f, 0.5f, 1.0f, 2.0f }, { -0.5f, 0.5f, 1.0f, 2.0f } };
	static const float far0[1][4] = { { 0.0f, 0.0f, 1.0f, 1.0f } };
	frame_init(&fa);
	add_group(&fa, 0x1000, 0, 2, m0);  /* árbol 1 */
	add_group(&fa, 0x1000, 0, 2, m1);  /* árbol 2: misma lista */
	add_group(&fa, 0x2000, 4, 1, far0);
	finish(&fa);
	CHECK_EQ(fa.t.ngroups, 3);
	CHECK_EQ(fa.t.g[0].occ, 0);
	CHECK_EQ(fa.t.g[1].occ, 1);

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
	CHECK_EQ(fb.t.g[0].pair, 0);
	CHECK_EQ(fb.t.g[1].pair, 1);  /* el segundo árbol con el segundo */
	CHECK_EQ(fb.t.g[2].pair, -1); /* se movió 144 px: corte */
	CHECK_EQ(fb.t.g[3].pair, -1); /* nuevo */
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
	CHECK(near(prev[0][2], v[0].z));  /* z, la de ahora (la profundidad la pone quien dibuja) */
	want = out_of(m1[0], 0);
	CHECK(near(prev[1][0], want.x) && near(prev[1][1], want.y));
	CHECK(near(prev[2][0], v[2].x) && near(prev[3][0], v[3].x));  /* sin pareja: donde está */
	/* Recorte cercano entre los lugares 0 y 1 del grupo 1: t sale de z + w de
	 * ahora (n0: 3 y 3 → iguales: sin t). Con otro grupo de prueba abajo. */
	CHECK(near(prev[4][0], v[4].x));
	static const float q0[2][4] = { { 0, 0, -1.5f, 1.0f }, { 1, 0, 1.0f, 3.0f } };  /* z + w: −0,5 y 4 */
	static const float q1[2][4] = { { 0.1f, 0, -1.5f, 1.0f }, { 1.1f, 0, 1.0f, 3.0f } };
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
	CHECK_EQ(fb.t.g[0].pair, 0);
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

	/* Con el anterior a mano solo se mira el contenido de lo que cambió de
	 * dirección: el grupo en 0x4000, que ya estaba, queda sin hash. */
	vmatch_build(&fb.t, &fa.t, fb.cmd, fb.len, ram, sizeof ram);
	CHECK_EQ(fb.t.g[0].hash, 0);
	CHECK(fb.t.g[1].hash != 0);
	vmatch_pair(NULL, &fb.t, 120.0f, &st);
	CHECK_EQ(st.found, 0);
	CHECK_EQ(vmatch_prev(NULL, &fb.t, &st, &v, 1, prev), 0);
	CHECK(near(prev[0][0], v.x));
}

/* Un cuadrilátero chico (8 px) centrado en (px, py) píxeles, con w = 1. */
static void quad_at(float c[4][4], float px, float py)
{
	static const float off[4][2] = { { -4, -4 }, { 4, -4 }, { -4, 4 }, { 4, 4 } };
	for (int k = 0; k < 4; k++) {
		c[k][0] = (px + off[k][0]) / 160.0f;
		c[k][1] = (py + off[k][1]) / 120.0f;
		c[k][2] = 0.5f;
		c[k][3] = 1.0f;
	}
}

/* Instancias del mismo modelo que cambian de orden: cada una con la más
 * cercana de antes. Una partícula que renace lejos y una estela en anillo
 * (un vértice salta del final al principio) quedan sin pareja. */
static void vmatch_identity_rules(void)
{
	memset(ram, 0, sizeof ram);
	float q[4][4];
	/* Cinco instancias que avanzan 2 px; la segunda y la cuarta cambian de
	 * lugar en el orden. */
	static const float xa[5] = { -200, -100, 0, 100, 200 }, xb[5] = { -198, 102, 2, -98, 202 };
	frame_init(&fa);
	for (int i = 0; i < 5; i++) {
		quad_at(q, xa[i], 0);
		add_group(&fa, 0x7000, 0, 4, (const float(*)[4])q);
	}
	finish(&fa);
	frame_init(&fb);
	for (int i = 0; i < 5; i++) {
		quad_at(q, xb[i], 0);
		add_group(&fb, 0x7000, 0, 4, (const float(*)[4])q);
	}
	finish(&fb);
	struct vmatch_stats st;
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, 0);
	CHECK_EQ(fb.t.g[1].pair, 3);
	CHECK_EQ(fb.t.g[2].pair, 2);
	CHECK_EQ(fb.t.g[3].pair, 1);
	CHECK_EQ(fb.t.g[4].pair, 4);
	CHECK_EQ(st.reordered, 2);
	CHECK_EQ(st.paired, 5);

	/* Una partícula de cuatro que renace 200 px más allá (sus hermanas
	 * quietas): sin pareja. Un modelo de una sola instancia puede moverse
	 * eso (la cámara). */
	static const float pa[4] = { -100, -50, 0, 50 }, pb[4] = { 100, -50, 0, 50 };
	frame_init(&fa);
	for (int i = 0; i < 4; i++) {
		quad_at(q, pa[i], 0);
		add_group(&fa, 0x7100, 0, 4, (const float(*)[4])q);
	}
	quad_at(q, -60, 30);
	add_group(&fa, 0x7180, 0, 4, (const float(*)[4])q);
	finish(&fa);
	frame_init(&fb);
	for (int i = 0; i < 4; i++) {
		quad_at(q, pb[i], 0);
		add_group(&fb, 0x7100, 0, 4, (const float(*)[4])q);
	}
	quad_at(q, 40, 30);
	add_group(&fb, 0x7180, 0, 4, (const float(*)[4])q);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, -1);
	CHECK_EQ(fb.t.g[1].pair, 1);
	CHECK_EQ(fb.t.g[4].pair, 4);
	CHECK_EQ(st.jumped, 1);

	static const float ta[4][4] = { { 0, 0, 0.5f, 1 }, { 20 / 160.0f, 0, 0.5f, 1 }, { 40 / 160.0f, 0, 0.5f, 1 },
					{ 60 / 160.0f, 0, 0.5f, 1 } };
	static const float tb[4][4] = { { 80 / 160.0f, 0, 0.5f, 1 }, { 20 / 160.0f, 0, 0.5f, 1 },
					{ 40 / 160.0f, 0, 0.5f, 1 }, { 60 / 160.0f, 0, 0.5f, 1 } };
	frame_init(&fa);
	add_group(&fa, 0x7200, 0, 4, ta);
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x7200, 0, 4, tb);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, -1);
	CHECK_EQ(st.deformed, 1);
	/* La misma estela que solo avanza (todos 20 px): con pareja. */
	static const float tc[4][4] = { { 20 / 160.0f, 0, 0.5f, 1 }, { 40 / 160.0f, 0, 0.5f, 1 },
					{ 60 / 160.0f, 0, 0.5f, 1 }, { 80 / 160.0f, 0, 0.5f, 1 } };
	frame_init(&fb);
	add_group(&fb, 0x7200, 0, 4, tc);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, 0);
}

void t_suite_vmatch(void)
{
	RUN(vmatch_pairs_and_positions);
	RUN(vmatch_content_and_cut);
	RUN(vmatch_identity_rules);
}
