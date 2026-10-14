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


static void frame_init(struct frame *f)
{
	memset(f, 0, sizeof *f);
	CHECK(vmatch_table_bytes(GCAP, CCAP) <= sizeof f->mem);
	vmatch_table_init(&f->t, f->mem, GCAP, CCAP);
}

/* Un G_VTX de n vértices desde `addr` con viewport 160 × 120 (el de la HLE
 * por omisión), las coordenadas de recorte de `clip` y el hash de contenido
 * `hash` (add_group: uno por dirección, como una lista que no cambia). */
static void add_group_h(struct frame *f, uint32_t addr, uint32_t v0, uint32_t n, const float (*clip)[4], uint32_t hash)
{
	struct gfx_vgroup r;
	memset(&r, 0, sizeof r);
	r.h.op = GFX_OP_VGROUP;
	r.h.size = (uint16_t)(sizeof r + 16u * n);
	r.h.a = addr;
	r.h.b = n | v0 << 8 | 16u << 16;
	r.h.c = hash;
	r.vp[0] = 160.0f, r.vp[1] = 120.0f, r.vp[2] = 160.0f, r.vp[3] = 120.0f;
	memcpy(f->cmd + f->len, &r, sizeof r);
	memcpy(f->cmd + f->len + sizeof r, clip, 16u * n);
	f->len += r.h.size;
}

/* Con la vista en z del registro: z de salida = vz1·w + vz0·z. */
static void add_group_vz(struct frame *f, uint32_t addr, uint32_t n, const float (*clip)[4], float vz0, float vz1)
{
	add_group_h(f, addr, 0, n, clip, addr * 2654435761u | 1u);
	struct gfx_vgroup *r = (struct gfx_vgroup *)(f->cmd + f->len - sizeof *r - 16u * n);
	r->vz[0] = vz0, r->vz[1] = vz1;
}

static void add_group(struct frame *f, uint32_t addr, uint32_t v0, uint32_t n, const float (*clip)[4])
{
	add_group_h(f, addr, v0, n, clip, addr * 2654435761u | 1u);
}

static void finish(struct frame *f)
{
	struct gfx_cmd c = { GFX_OP_END, sizeof c, 0, 0, 0 };
	memcpy(f->cmd + f->len, &c, sizeof c);
	f->len += sizeof c;
	vmatch_build(&f->t, f->cmd, f->len);
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

/* Grupos quietos de un vértice (el resto del escenario): con muchos, uno
 * que se mueve de más no es un giro de la cámara. */
static void add_still(struct frame *f, int count)
{
	for (int i = 0; i < count; i++) {
		const float c[1][4] = { { -0.8f + 0.05f * (float)i, -0.6f, 1.0f, 2.0f } };
		add_group(f, 0x40000 + 0x100u * (uint32_t)i, 0, 1, c);
	}
}

/* El mismo modelo dos veces (se distinguen por orden), uno que aparece, uno
 * que se movió de más y la posición anterior de cada vértice de salida,
 * también la de uno del recorte cercano. */
static void vmatch_pairs_and_positions(void)
{
	static const float m0[2][4] = { { 0.0f, 0.0f, 1.0f, 2.0f }, { 0.5f, 0.0f, 1.0f, 2.0f } };
	static const float m1[2][4] = { { -1.0f, 0.5f, 1.0f, 2.0f }, { -0.5f, 0.5f, 1.0f, 2.0f } };
	static const float far0[1][4] = { { 0.0f, 0.0f, 1.0f, 1.5f } };
	frame_init(&fa);
	add_group(&fa, 0x1000, 0, 2, m0);  /* árbol 1 */
	add_group(&fa, 0x1000, 0, 2, m1);  /* árbol 2: misma lista */
	add_group(&fa, 0x2000, 4, 1, far0);
	add_still(&fa, 20);
	finish(&fa);
	CHECK_EQ(fa.t.ngroups, 23);
	CHECK_EQ(fa.t.g[0].occ, 0);
	CHECK_EQ(fa.t.g[1].occ, 1);

	static const float n0[2][4] = { { 0.1f, 0.0f, 1.0f, 2.0f }, { 0.6f, 0.0f, 1.0f, 2.0f } };
	static const float n1[2][4] = { { -0.9f, 0.5f, 1.0f, 2.0f }, { -0.4f, 0.5f, 1.0f, 2.0f } };
	static const float far1[1][4] = { { 1.35f, 0.0f, 1.0f, 1.5f } };  /* 144 px en x */
	static const float new0[1][4] = { { 0.0f, 0.0f, 1.0f, 1.5f } };
	frame_init(&fb);
	add_group(&fb, 0x1000, 0, 2, n0);
	add_group(&fb, 0x1000, 0, 2, n1);
	add_group(&fb, 0x2000, 4, 1, far1);
	add_group(&fb, 0x3000, 8, 1, new0);
	add_still(&fb, 20);
	finish(&fb);
	CHECK_EQ(fb.t.ngroups, 24);
	struct vmatch_stats st;
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, 0);
	CHECK_EQ(fb.t.g[1].pair, 1);  /* el segundo árbol con el segundo */
	CHECK_EQ(fb.t.g[2].pair, -1); /* se movió 144 px: corte */
	CHECK_EQ(fb.t.g[3].pair, -1); /* nuevo */
	CHECK_EQ(st.found, 23);
	CHECK_EQ(st.moved, 1);
	CHECK_EQ(st.paired, 22);
	CHECK(!st.cut);

	struct gfx_vertex v[5] = {
		out_of(n0[1], GFX_VGID(1u, 1u)), out_of(n1[0], GFX_VGID(2u, 0u)), out_of(far1[0], GFX_VGID(3u, 0u)),
		out_of(new0[0], 0), out_of(n0[0], GFX_VCLIP_ID(1u, 0u, 1u)),
	};
	float prev[5][4];
	CHECK_EQ(vmatch_prev(&fa.t, &fb.t, &st, v, 5, prev), 2);
	struct gfx_vertex want = out_of(m0[1], 0);
	CHECK(near(prev[0][0], want.x) && near(prev[0][1], want.y) && near(prev[0][3], want.w));
	CHECK(near(prev[0][2], v[0].z));  /* sin la vista en z (vz = 0), la misma z/w que ahora: w igual */
	want = out_of(m1[0], 0);
	CHECK(near(prev[1][0], want.x) && near(prev[1][1], want.y));
	CHECK(near(prev[2][0], v[2].x) && near(prev[3][0], v[3].x));  /* sin pareja: donde está */
	/* Recorte cercano entre los lugares 0 y 1 del grupo 1: t sale de z + w de
	 * ahora (n0: 3 y 3 → iguales: sin t). Con otro grupo de prueba abajo. */
	CHECK(near(prev[4][0], v[4].x));
	static const float q0[2][4] = { { 0, 0, -1.5f, 1.1f }, { 1, 0, 1.0f, 3.0f } };  /* z + w: −0,4 y 4 */
	static const float q1[2][4] = { { 0.1f, 0, -1.5f, 1.1f }, { 1.1f, 0, 1.0f, 3.0f } };
	frame_init(&fa);
	add_group(&fa, 0x6000, 0, 2, q0);
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x6000, 0, 2, q1);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	float t = -0.4f / (-0.4f - 4.0f), cq[4];
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
 * encuentran por el contenido (el hash que deja la HLE). Y un cuadro con
 * casi todo nuevo es un corte. */
static void vmatch_content_and_cut(void)
{
	static const float a[3][4] = { { 0, 0, 1, 2 }, { 0.2f, 0, 1, 2 }, { 0, 0.2f, 1, 2 } };
	frame_init(&fa);
	add_group_h(&fa, 0x4000, 0, 3, a, 0x12345679u);  /* la misma lista en los dos búferes */
	finish(&fa);
	frame_init(&fb);
	add_group_h(&fb, 0x4800, 0, 3, a, 0x12345679u);
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

	/* Por el contenido pero cinco veces más grande: no es el mismo. */
	static const float big[3][4] = { { 0, 0, 1, 2 }, { 1.0f, 0, 1, 2 }, { 0, 1.0f, 1, 2 } };
	frame_init(&fb);
	add_group_h(&fb, 0x4900, 0, 3, big, 0x12345679u);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, -1);
	CHECK_EQ(st.deformed, 1);

	vmatch_pair(NULL, &fb.t, 120.0f, &st);
	CHECK_EQ(st.found, 0);
	CHECK_EQ(vmatch_prev(NULL, &fb.t, &st, &v, 1, prev), 0);
	CHECK(near(prev[0][0], v.x));
}

/* Un cuadrilátero chico (8 px) centrado en (px, py) píxeles, con w = 2
 * (perspectiva: w = 1 en todos es el HUD, que no se interpola). */
static void quad_at(float c[4][4], float px, float py)
{
	static const float off[4][2] = { { -4, -4 }, { 4, -4 }, { -4, 4 }, { 4, 4 } };
	for (int k = 0; k < 4; k++) {
		c[k][0] = 2.0f * (px + off[k][0]) / 160.0f;
		c[k][1] = 2.0f * (py + off[k][1]) / 120.0f;
		c[k][2] = 1.0f;
		c[k][3] = 2.0f;
	}
}

/* Instancias del mismo modelo que cambian de orden: cada una con la más
 * cercana de antes. Una partícula que renace lejos y una estela en anillo
 * (un vértice salta del final al principio) quedan sin pareja. */
static void vmatch_identity_rules(void)
{
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

	static const float ta[4][4] = { { 0, 0, 1, 2 }, { 40 / 160.0f, 0, 1, 2 }, { 80 / 160.0f, 0, 1, 2 },
					{ 120 / 160.0f, 0, 1, 2 } };
	static const float tb[4][4] = { { 160 / 160.0f, 0, 1, 2 }, { 40 / 160.0f, 0, 1, 2 },
					{ 80 / 160.0f, 0, 1, 2 }, { 120 / 160.0f, 0, 1, 2 } };
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
	static const float tc[4][4] = { { 40 / 160.0f, 0, 1, 2 }, { 80 / 160.0f, 0, 1, 2 },
					{ 120 / 160.0f, 0, 1, 2 }, { 160 / 160.0f, 0, 1, 2 } };
	frame_init(&fb);
	add_group(&fb, 0x7200, 0, 4, tc);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, 0);

	/* Un vértice que pasa de detrás a delante de la cámara no se interpola
	 * (en algún intermedio se proyectaría al infinito); los demás del grupo
	 * (grande: 80 px), sí. En una instancia chica de un modelo repetido,
	 * ninguno (una chispa). */
	static const float s0[3][4] = { { 0, 0, 1, 2 }, { 0.2f, 0, 1, 2 }, { 0.1f, 0.1f, -0.5f, -0.3f } };
	static const float s1[3][4] = { { 0.02f, 0, 1, 2 }, { 0.22f, 0, 1, 2 }, { 0.1f, 0.1f, 0.4f, 0.6f } };
	static const float sq[3][4] = { { 0.5f, 0.5f, 1, 2 }, { 0.55f, 0.5f, 1, 2 }, { 0.5f, 0.55f, 1, 2 } };
	frame_init(&fa);
	add_group(&fa, 0x7500, 0, 3, s0);
	add_group(&fa, 0x7500, 0, 3, sq);  /* otra instancia (partículas) */
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x7500, 0, 3, s1);
	add_group(&fb, 0x7500, 0, 3, sq);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].why, VMATCH_NEAR);
	CHECK_EQ(fb.t.g[1].pair, 1);
	CHECK_EQ(st.near, 1);
	static const float c0[3][4] = { { 0, 0, 1, 2 }, { 1.0f, 0, 1, 2 }, { 0.1f, 0.1f, -0.5f, -0.3f } };
	static const float c1[3][4] = { { 0.02f, 0, 1, 2 }, { 1.02f, 0, 1, 2 }, { 0.1f, 0.1f, 0.4f, 0.6f } };
	frame_init(&fa);
	add_group(&fa, 0x7400, 0, 3, c0);
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x7400, 0, 3, c1);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, 0);
	struct gfx_vertex cv[3] = { out_of(c1[0], GFX_VGID(1u, 0u)), out_of(c1[1], GFX_VGID(1u, 1u)),
				    out_of(c1[2], GFX_VGID(1u, 2u)) };
	float cp[3][4];
	CHECK_EQ(vmatch_prev(&fa.t, &fb.t, &st, cv, 3, cp), 2);
	CHECK(near(cp[2][0], cv[2].x) && near(cp[2][3], cv[2].w));  /* el que cruza, donde está */
	CHECK(near(cp[0][3], 2.0f) && !near(cp[0][0], cv[0].x));

	/* El HUD 3D (proyección ortográfica: w = 1 en todos) no se interpola ni
	 * cuenta para decidir un corte. */
	static const float h0[3][4] = { { 0.5f, 0.5f, 0, 1 }, { 0.6f, 0.5f, 0, 1 }, { 0.5f, 0.6f, 0, 1 } };
	static const float h1[3][4] = { { 0.5f, 0.5f, 0, 1 }, { 0.6f, 0.55f, 0, 1 }, { 0.5f, 0.65f, 0, 1 } };
	frame_init(&fa);
	add_group(&fa, 0x7300, 0, 3, h0);
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x7300, 0, 3, h1);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].pair, -1);
	CHECK_EQ(st.flat, 1);
	CHECK_EQ(st.vertices, 0);
	CHECK(!st.cut);
}

/* Grupos 1 y 2 en una malla (un triángulo con vértices de los dos), el 3
 * aparte (otro triángulo) a dx_c píxeles del 1. have_b: el 2 estaba en el
 * cuadro anterior; b_behind: el 2 está detrás de la cámara; a_deform: un
 * vértice del 1 salta 40 px (se deforma). */
static void atomic_case(bool have_b, bool b_behind, bool a_deform, float dx_c, struct vmatch_stats *st,
			uint32_t *paired)
{
	float a0[4][4], a1[4][4], b0[4][4], b1[4][4], c0[4][4], c1[4][4];
	quad_at(a0, 0, 0), quad_at(a1, 2, 0);
	quad_at(b0, 10, 0), quad_at(b1, 12, 0);
	quad_at(c0, dx_c, 0), quad_at(c1, dx_c + 2, 0);
	if (a_deform)
		a1[3][0] += 2.0f * 40.0f / 160.0f;
	if (b_behind)
		for (int k = 0; k < 4; k++)
			b1[k][3] = -1.0f;
	frame_init(&fa);
	add_group(&fa, 0x8000, 0, 4, (const float(*)[4])a0);
	if (have_b)
		add_group(&fa, 0x8100, 4, 4, (const float(*)[4])b0);
	add_group(&fa, 0x8200, 0, 4, (const float(*)[4])c0);
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x8000, 0, 4, (const float(*)[4])a1);
	add_group(&fb, 0x8100, 4, 4, (const float(*)[4])b1);
	add_group(&fb, 0x8200, 0, 4, (const float(*)[4])c1);
	finish(&fb);
	struct gfx_vertex v[6] = {
		out_of(a1[0], GFX_VGID(1u, 0u)), out_of(a1[1], GFX_VGID(1u, 1u)), out_of(b1[0], GFX_VGID(2u, 0u)),
		out_of(c1[0], GFX_VGID(3u, 0u)), out_of(c1[1], GFX_VGID(3u, 1u)), out_of(c1[2], GFX_VGID(3u, 2u)),
	};
	vmatch_pair(&fa.t, &fb.t, 120.0f, st);
	vmatch_atomic(&fb.t, v, 6, st);
	float prev[6][4];
	*paired = vmatch_prev(&fa.t, &fb.t, st, v, 6, prev);
	if (fb.t.g[0].pair < 0)
		CHECK(near(prev[0][0], v[0].x));
}

/* Nunca un objeto a medias (vmatch_atomic), y un giro grande no se
 * interpola. */
static void vmatch_atomic_mesh(void)
{
	struct vmatch_stats st;
	uint32_t paired;
	atomic_case(true, false, false, 80, &st, &paired);  /* todos con pareja */
	CHECK_EQ(st.atomic, 0);
	CHECK_EQ(paired, 6);
	/* El grupo 2 es nuevo: su malla entera sin interpolar; el 3, sí. */
	atomic_case(false, false, false, 80, &st, &paired);
	CHECK_EQ(fb.t.g[1].why, VMATCH_NEW);
	CHECK_EQ(fb.t.g[0].pair, -1);
	CHECK_EQ(fb.t.g[0].why, VMATCH_COMPONENT);
	CHECK_EQ(fb.t.g[2].pair, 1);
	CHECK_EQ(st.atomic, 1);
	CHECK_EQ(st.paired, 1);
	CHECK_EQ(paired, 3);
	/* Nuevo pero detrás de la cámara: no cuenta. */
	atomic_case(false, true, false, 80, &st, &paired);
	CHECK_EQ(fb.t.g[0].pair, 0);
	CHECK_EQ(st.atomic, 0);
	/* Nuevo y el 3 pegado: un grupo nuevo no arrastra su objeto. */
	atomic_case(false, false, false, 20, &st, &paired);
	CHECK_EQ(fb.t.g[0].why, VMATCH_COMPONENT);
	CHECK_EQ(fb.t.g[2].pair, 1);
	/* El 1 se deforma: con el 2 (pegado) es un objeto con la mitad de sus
	 * vértices rechazados, entero sin interpolar; el 3, lejos, sí. */
	atomic_case(true, false, true, 80, &st, &paired);
	CHECK_EQ(fb.t.g[0].why, VMATCH_DEFORMED);
	CHECK_EQ(fb.t.g[1].why, VMATCH_OBJECT);
	CHECK_EQ(fb.t.g[2].pair, 2);
	/* Con el 3 pegado el deformado es un tercio del objeto: se interpola
	 * con los demás (todos con pareja; una pieza en el cuadro de ahora se
	 * despegaría de las otras). */
	atomic_case(true, false, true, 20, &st, &paired);
	CHECK_EQ(fb.t.g[0].pair, 0);
	CHECK_EQ(fb.t.g[0].why, VMATCH_OK);
	CHECK_EQ(fb.t.g[1].pair, 1);
	CHECK_EQ(fb.t.g[2].pair, 2);
	CHECK_EQ(st.kept, 1);
	CHECK_EQ(st.deformed, 0);
	CHECK_EQ(paired, 6);
	/* Con el 2 detrás de la cámara (no cuenta), la mitad: el objeto entero. */
	atomic_case(true, true, true, 20, &st, &paired);
	CHECK_EQ(fb.t.g[2].why, VMATCH_OBJECT);
	CHECK_EQ(paired, 0);

	/* Dos piezas a 20 px: del mismo modelo (direcciones cercanas), un solo
	 * objeto (la mitad rechazada: entero en el cuadro de ahora); de modelos
	 * lejanos, dos (la deformada sola, la otra interpolada). */
	for (int far_model = 0; far_model < 2; far_model++) {
		float d0[4][4], d1[4][4], e0[4][4], e1[4][4];
		quad_at(d0, 0, 0), quad_at(d1, 2, 0), quad_at(e0, 0, 28), quad_at(e1, 2, 28);
		d1[3][0] += 2.0f * 40.0f / 160.0f;  /* se deforma */
		uint32_t ea = far_model ? 0x90000 : 0x8800;
		frame_init(&fa);
		add_group(&fa, 0x8600, 0, 4, (const float(*)[4])d0);
		add_group(&fa, ea, 0, 4, (const float(*)[4])e0);
		finish(&fa);
		frame_init(&fb);
		add_group(&fb, 0x8600, 0, 4, (const float(*)[4])d1);
		add_group(&fb, ea, 0, 4, (const float(*)[4])e1);
		finish(&fb);
		struct gfx_vertex dv[3] = { out_of(d1[0], GFX_VGID(1u, 0u)), out_of(d1[1], GFX_VGID(1u, 1u)),
					    out_of(d1[2], GFX_VGID(1u, 2u)) };
		vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
		vmatch_atomic(&fb.t, dv, 3, &st);
		CHECK_EQ(fb.t.g[0].why, VMATCH_DEFORMED);
		CHECK_EQ(fb.t.g[1].why, far_model ? VMATCH_OK : VMATCH_OBJECT);
	}

	/* Un cuadrilátero que gira media vuelta (se aplastaría a media
	 * interpolación) y uno que gira un poco. */
	float r0[4][4], r1[4][4], s0[4][4], s1[4][4];
	quad_at(r0, -60, 0), quad_at(s0, 60, 0), quad_at(s1, 60, 0);
	quad_at(r1, -60, 0);
	for (int k = 0; k < 4; k++)  /* media vuelta: cada vértice, al lado opuesto del centro */
		r1[k][0] = 2.0f * (2.0f * -60.0f / 160.0f) - r0[k][0], r1[k][1] = -r0[k][1];
	s1[3][0] += 2.0f * 2.0f / 160.0f;  /* un vértice 2 px */
	frame_init(&fa);
	add_group(&fa, 0x8300, 0, 4, (const float(*)[4])r0);
	add_group(&fa, 0x8400, 0, 4, (const float(*)[4])s0);
	finish(&fa);
	frame_init(&fb);
	add_group(&fb, 0x8300, 0, 4, (const float(*)[4])r1);
	add_group(&fb, 0x8400, 0, 4, (const float(*)[4])s1);
	finish(&fb);
	vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
	CHECK_EQ(fb.t.g[0].why, VMATCH_COLLAPSE);
	CHECK_EQ(fb.t.g[1].pair, 1);
	CHECK_EQ(st.collapsed, 1);
}

/* La z de allá con la vista del grupo; si la de ahora no sale con esa vista
 * (la z de primitiva), la de allá con la misma z/w que ahora. */
static void vmatch_depth(void)
{
	static const float a0[3][4] = { { 0, 0, 0.2f, 2 }, { 0.2f, 0, 0.4f, 2 }, { 0, 0.2f, 0.6f, 3 } };
	static const float a1[3][4] = { { 0.02f, 0, 0.3f, 2.5f }, { 0.22f, 0, 0.5f, 2.5f }, { 0.02f, 0.2f, 0.7f, 3.5f } };
	for (int prim = 0; prim < 2; prim++) {
		frame_init(&fa);
		add_group_vz(&fa, 0x9000, 3, a0, 0.5f, 0.5f);
		finish(&fa);
		frame_init(&fb);
		add_group_vz(&fb, 0x9000, 3, a1, 0.5f, 0.5f);
		finish(&fb);
		struct gfx_vertex v[3];
		for (int k = 0; k < 3; k++) {
			v[k] = out_of(a1[k], GFX_VGID(1u, (uint32_t)k));
			/* como gfx_vtx_out con la vista; con la z de primitiva, 0,25·w */
			v[k].z = prim ? 0.25f * a1[k][3] : 0.5f * a1[k][3] + 0.5f * a1[k][2];
		}
		struct vmatch_stats st;
		vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
		float prev[3][4];
		CHECK_EQ(vmatch_prev(&fa.t, &fb.t, &st, v, 3, prev), 3);
		for (int k = 0; k < 3; k++) {
			float want = prim ? 0.25f * a0[k][3] : 0.5f * a0[k][3] + 0.5f * a0[k][2];
			CHECK(near(prev[k][2], want));
			CHECK(near(prev[k][3], a0[k][3]));
		}
	}
}

/* Un giro de golpe de la cámara (Z en Zelda): si más del 5 % de lo que se
 * ve se movió más que max_disp, el cuadro entero es un corte. */
static void vmatch_camera_cut(void)
{
	static const float far0[1][4] = { { 0.0f, 0.0f, 1.0f, 1.5f } }, far1[1][4] = { { 1.35f, 0.0f, 1.0f, 1.5f } };
	for (int many = 0; many < 2; many++) {
		frame_init(&fa);
		frame_init(&fb);
		for (int i = 0; i < (many ? 3 : 1); i++) {
			add_group(&fa, 0x2000 + 0x100u * (uint32_t)i, 0, 1, far0);
			add_group(&fb, 0x2000 + 0x100u * (uint32_t)i, 0, 1, far1);
		}
		add_still(&fa, 30);
		add_still(&fb, 30);
		finish(&fa);
		finish(&fb);
		struct vmatch_stats st;
		vmatch_pair(&fa.t, &fb.t, 120.0f, &st);
		CHECK_EQ(st.cut, many != 0);
		CHECK_EQ(st.camera, many != 0);
	}
}

void t_suite_vmatch(void)
{
	RUN(vmatch_pairs_and_positions);
	RUN(vmatch_content_and_cut);
	RUN(vmatch_identity_rules);
	RUN(vmatch_atomic_mesh);
	RUN(vmatch_depth);
	RUN(vmatch_camera_cut);
}
