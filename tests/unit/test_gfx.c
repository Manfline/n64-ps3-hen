/*
 * Salsa64 — pruebas de la HLE gráfica (src/rsp/gfx): display lists F3DEX2
 * armadas a mano en la RDRAM, ejecutadas con gfx_run y revisadas en su
 * salida (vértices, registros de dibujo y texturas).
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/backend.h"
#include "rsp/gfx/hle.h"

#define DL_PA   0x10000u
#define DATA_PA 0x20000u
#define TEX_PA  0x30000u

static struct gfx_state gs;
static struct gfx_vertex out_vtx[4096] __attribute__((aligned(16)));
static uint8_t out_cmd[64 * 1024] __attribute__((aligned(16)));
static uint8_t out_tex[256 * 1024] __attribute__((aligned(128)));
static uint8_t out_cache[64 * 1024] __attribute__((aligned(128)));
static struct gfx_job job;
static uint32_t cache_task;  /* tarea de la caché de texturas (0: sin caché) */
static struct gfx_result res;
static uint32_t dl_pc, data_pc;
static uint32_t job_ucode;   /* dirección física del microcódigo de la tarea */

/* ---- Armado de la display list ---- */

static void dl(uint32_t w0, uint32_t w1)
{
	t_wr32(dl_pc, w0);
	t_wr32(dl_pc + 4, w1);
	dl_pc += 8;
}

static uint32_t data_alloc(uint32_t bytes)
{
	uint32_t a = data_pc;
	data_pc += (bytes + 15) & ~15u;
	return a;
}

static void start(void)
{
	dl_pc = DL_PA;
	data_pc = DATA_PA;
	cache_task = 0;
	gfx_init(&gs);
}

/* Otra tarea con el mismo estado del RDP (y la misma caché). */
static void next_task(void)
{
	dl_pc = DL_PA;
	data_pc = DATA_PA;
	cache_task++;
}

static void run(void)
{
	dl(0xDF000000, 0);  /* G_ENDDL */
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	if (cache_task) {
		job.cache_ea = (uint64_t)(uintptr_t)out_cache;
		job.cache_cap = sizeof out_cache;
		job.task = cache_task;
		job.done_task = cache_task - 1;
	}
	job.gbi = SALSA64_GBI_F3DEX2;
	job.dl = DL_PA;
	job.ucode = job_ucode;
	gfx_run(&gs, &job, &res);
}

/* n-ésimo registro de un tipo en la salida (NULL si no hay). */
static const struct gfx_cmd *find_cmd(uint16_t op, int n)
{
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= res.cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(out_cmd + off);
		if (c->op == op && n-- == 0)
			return c;
		if (c->op == GFX_OP_END || c->size == 0)
			break;
		off += c->size;
	}
	return NULL;
}

static const struct gfx_draw *draw_n(int n)
{
	return (const struct gfx_draw *)find_cmd(GFX_OP_DRAW, n);
}

/* Mtx de libultra desde floats (fila por fila). */
static uint32_t put_matrix(const float m[16])
{
	uint32_t a = data_alloc(64);
	for (int i = 0; i < 16; i++) {
		int32_t v = (int32_t)lrintf(m[i] * 65536.0f);
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, (uint16_t)((uint32_t)v >> 16));
		salsa64_wr16(t_n64.bus.rdram + a + 32 + 2 * i, (uint16_t)v);
	}
	return a;
}

static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

struct vtx { int16_t x, y, z, s, t; uint8_t c[4]; };

static uint32_t put_vertices(const struct vtx *v, int n)
{
	uint32_t a = data_alloc(16 * (uint32_t)n);
	for (int i = 0; i < n; i++) {
		uint8_t *p = t_n64.bus.rdram + a + 16 * i;
		salsa64_wr16(p + 0, (uint16_t)v[i].x);
		salsa64_wr16(p + 2, (uint16_t)v[i].y);
		salsa64_wr16(p + 4, (uint16_t)v[i].z);
		salsa64_wr16(p + 6, 0);
		salsa64_wr16(p + 8, (uint16_t)v[i].s);
		salsa64_wr16(p + 10, (uint16_t)v[i].t);
		memcpy(p + 12, v[i].c, 4);
	}
	return a;
}

/* Viewport de 320×240 y z 0..0x3FE. */
static void viewport_320x240(void)
{
	uint32_t a = data_alloc(16);
	const int16_t vp[8] = { 640, 480, 511, 0, 640, 480, 511, 0 };
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, (uint16_t)vp[i]);
	dl(0xDC080008, a);  /* G_MOVEMEM viewport */
}

static void load_mtx(const float m[16], bool projection)
{
	/* G_MTX con LOAD (y NOPUSH: el bit 0 va invertido, así que queda 1). */
	dl(0xDA380000 | (projection ? 0x04 : 0x00) | 0x02 | 0x01, put_matrix(m));
}

#define CC(a, b, c, d) (a), (b), (c), (d)

/* Combiner igual en los dos ciclos: rgb (a, b, c, d) y alfa (a, b, c, d). */
static void set_combine(unsigned a, unsigned b, unsigned c, unsigned d, unsigned aa, unsigned ab, unsigned ac,
			unsigned ad)
{
	uint32_t hi = a << 20 | c << 15 | aa << 12 | ac << 9 | a << 5 | c;
	uint32_t lo = b << 28 | b << 24 | aa << 21 | ac << 18 | d << 15 | ab << 12 | ad << 9 | d << 6 | ab << 3 | ad;
	dl(0xFC000000 | hi, lo);
}

static void set_cycle(unsigned cycle)
{
	/* G_SETOTHERMODE_H, shift 20, len 2. */
	dl(0xE3000000 | (32 - 20 - 2) << 8 | 1, cycle << 20);
}

enum { CC_COMB = 0, CC_T0 = 1, CC_T1 = 2, CC_PRIM = 3, CC_SHADE = 4, CC_ENV = 5, CC_ONE = 6, CC_ZERO = 7 };
enum { CC_C_ZERO = 31, CC_C_T0A = 8, CC_C_SHADEA = 11 };
enum { CC_A_ZERO = 15 };

/* ---- Pruebas ---- */

static void gfx_combiner_forms(void)
{
	/* SHADE: todo en X_4, coeficiente del color del vértice. */
	start();
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
	gs.oh = 0;
	run();
	struct gfx_draw d;
	memset(&d, 0, sizeof d);
	gs.cc_hi = 0, gs.cc_lo = 0;
	/* Se evalúa directo sobre el estado. */
	gs.cc_hi = (15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u);
	gs.cc_lo = (7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 4u << 15 | 7u << 12 | 4u << 9 | 4u << 6 | 7u << 3 | 4u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.ntex, 0);
	for (int lane = 0; lane < 3; lane++)
		CHECK(d.k[GFX_K_ONE][1][lane] == 1.0f);
	CHECK(d.k[GFX_K_ONE][2][3] == 1.0f);

	/* TEXEL0 × SHADE (G_CC_MODULATERGBA): T0·S en X_0 con coeficiente S. */
	gs.cc_hi = (1u << 20 | 4u << 15 | 1u << 12 | 4u << 9 | 1u << 5 | 4u);
	gs.cc_lo = (15u << 28 | 15u << 24 | 1u << 21 | 4u << 18 | 7u << 15 | 7u << 12 | 7u << 9 | 7u << 6 | 7u << 3 | 7u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.ntex, 1);
	CHECK(d.k[GFX_K_T0][1][0] == 1.0f && d.k[GFX_K_T0][0][0] == 0.0f);
	CHECK(d.k[GFX_K_T0A][2][3] == 1.0f);  /* alfa: T0a · Sa */
	CHECK(d.k[GFX_K_ONE][0][0] == 0.0f);

	/* (PRIM − ENV) × TEXEL0 + ENV con prim = blanco y env = negro: T0. */
	gs.prim = 0xFFFFFFFF;
	gs.env = 0x00000000;
	gs.cc_hi = (3u << 20 | 1u << 15 | 7u << 12 | 7u << 9 | 3u << 5 | 1u);
	gs.cc_lo = (5u << 28 | 5u << 24 | 7u << 21 | 7u << 18 | 5u << 15 | 7u << 12 | 1u << 9 | 5u << 6 | 7u << 3 | 1u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK(fabsf(d.k[GFX_K_T0][0][1] - 1.0f) < 1e-6f);
	CHECK(fabsf(d.k[GFX_K_ONE][0][1]) < 1e-6f);
	CHECK(fabsf(d.k[GFX_K_T0A][0][3] - 1.0f) < 1e-6f);

	/* 2 ciclos: lerp(T0, T1, prim_lod) y luego × SHADE. En el segundo ciclo
	 * no hay texels: el intercambio TEXEL0/TEXEL1 no influye. */
	gs.oh = (uint32_t)G_CYC_2CYCLE << G_MDSFT_CYCLETYPE;
	gs.prim_lod = 0x80;
	gs.cc_hi = (2u << 20 | 14u << 15 | 7u << 12 | 7u << 9 | 0u << 5 | 4u);
	gs.cc_lo = (1u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 1u << 15 | 7u << 12 | 6u << 9 | 7u << 6 | 7u << 3 | 6u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.ntex, 2);
	float k = 0x80 / 255.0f;
	CHECK(fabsf(d.k[GFX_K_T0][1][0] - (1.0f - k)) < 1e-5f);
	CHECK(fabsf(d.k[GFX_K_T1][1][0] - k) < 1e-5f);

	/* TEXEL0 × TEXEL1: se aproxima. */
	gs.oh = 0;
	gs.cc_hi = (1u << 20 | 2u << 15 | 7u << 12 | 7u << 9 | 1u << 5 | 2u);
	gs.cc_lo = (15u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 7u << 15 | 7u << 12 | 6u << 9 | 7u << 6 | 7u << 3 | 6u);
	CHECK(gfx_combine(&gs, &d));
}

static void gfx_triangle_transform(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
	dl(0xD9000000, G_SHADING_SMOOTH | G_CULL_BACK);  /* geometry mode: solo estos */
	dl(0xFF100000 | 319, 0x00100000);                /* color image */
	const struct vtx v[3] = {
		{ 0, 0, 0, 0, 0, { 255, 0, 0, 255 } },
		{ 1, 0, 0, 0, 0, { 0, 255, 0, 255 } },
		{ 0, 1, 0, 0, 0, { 0, 0, 255, 255 } },
	};
	uint32_t va = put_vertices(v, 3);
	dl(0x01000000 | 3 << 12 | (0 + 3) << 1, va);  /* G_VTX: 3 desde 0 */
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0); /* G_TRI1 (índices ×2) */
	dl(0xE9000000, 0);                              /* G_RDPFULLSYNC */
	run();

	CHECK_EQ(res.triangles, 1);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	const struct gfx_cmd *t = find_cmd(GFX_OP_TARGET, 0);
	CHECK(t != NULL);
	if (t) {
		CHECK_EQ(t->a, 0x100000);
		CHECK_EQ(t->b, 320);
	}
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->h.a, 0);
	CHECK_EQ(d->h.b, 3);
	CHECK_EQ(d->cull, GFX_CULL_BACK);
	CHECK_EQ(d->ntex, 0);
	/* (0,0) al centro; (1,0) un viewport.x a la derecha; (0,1) arriba. */
	CHECK(out_vtx[0].x == 160.0f && out_vtx[0].y == 120.0f && out_vtx[0].w == 1.0f);
	CHECK(out_vtx[1].x == 320.0f && out_vtx[1].y == 120.0f);
	CHECK(out_vtx[2].x == 160.0f && out_vtx[2].y == 0.0f);
	CHECK(fabsf(out_vtx[0].z - 511.0f / 1023.0f) < 1e-6f);
	CHECK_EQ(out_vtx[1].g, 255);
	CHECK_EQ(out_vtx[2].b, 255);
}

static void gfx_matrix_stack(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	const float tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1 };
	const float sc[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1 };
	dl(0xDA380000 | 0x00, put_matrix(tr));  /* PUSH (bit 0 en 0) y MUL */
	dl(0xDA380000 | 0x00, put_matrix(sc));  /* PUSH y MUL: M = sc · tr */
	const struct vtx v[1] = { { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } } };
	uint32_t va = put_vertices(v, 1);
	dl(0x01000000 | 1 << 12 | 1 << 1, va);
	const struct vtx v1[1] = { { 1, 0, 0, 0, 0, { 0, 0, 0, 0 } } };
	uint32_t vb = put_vertices(v1, 1);
	dl(0x01000000 | 1 << 12 | 2 << 1, vb);  /* en el índice 1 */
	dl(0xD8380002, 64);                      /* POPMTX 1 */
	dl(0x01000000 | 1 << 12 | 3 << 1, vb);  /* índice 2, con solo tr */
	run();
	/* (0,0,0)·sc·tr = (0.5,0,0); (1,0,0)·sc·tr = (2.5,0,0); tras pop (1)·tr = 1.5. */
	CHECK(fabsf(gs.vtx[0].x - 0.5f) < 1e-4f);
	CHECK(fabsf(gs.vtx[1].x - 2.5f) < 1e-4f);
	CHECK(fabsf(gs.vtx[2].x - 1.5f) < 1e-4f);
	CHECK_EQ(gs.mv_top, 1);
}

static void gfx_lighting(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0xD9000000, G_LIGHTING | G_SHADING_SMOOTH);
	dl(0xDB020000, 1 * 24);  /* G_MW_NUMLIGHT: una luz */
	/* Luz 0: blanca a 200, dirección +z; ambiental (luz 1): 20. */
	uint32_t l0 = data_alloc(16), l1 = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram;
	p[l0 + 0] = p[l0 + 1] = p[l0 + 2] = 200;
	p[l0 + 4] = p[l0 + 5] = p[l0 + 6] = 200;
	p[l0 + 8] = 0, p[l0 + 9] = 0, p[l0 + 10] = 127;
	p[l1 + 0] = p[l1 + 1] = p[l1 + 2] = 20;
	dl(0xDC08000A | (2 * 24 / 8) << 8, l0);  /* G_MOVEMEM luz 0 (offset 48) */
	dl(0xDC08000A | (3 * 24 / 8) << 8, l1);  /* luz 1 = ambiental */
	const struct vtx v[2] = {
		{ 0, 0, 0, 0, 0, { 0, 0, 127, 0x40 } },   /* normal hacia la luz */
		{ 0, 0, 0, 0, 0, { 0, 0, (uint8_t)-127, 0x40 } },  /* de espaldas */
	};
	dl(0x01000000 | 2 << 12 | 2 << 1, put_vertices(v, 2));
	run();
	CHECK_EQ(gs.vtx[0].c[0], 220);
	CHECK_EQ(gs.vtx[0].c[3], 0x40);
	CHECK_EQ(gs.vtx[1].c[1], 20);
}

static void gfx_texgen(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0xD9000000, G_LIGHTING | G_TEXTURE_GEN);
	dl(0xD7000002 | 0 << 8, 0x07C007C0);  /* G_TEXTURE on, escala 0x07C0 */
	uint32_t lx = data_alloc(16), ly = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram;
	p[lx + 8] = 127;  /* lookat X = +x */
	p[ly + 9] = 127;  /* lookat Y = +y */
	dl(0xDC08000A | 0 << 8, lx);
	dl(0xDC08000A | (24 / 8) << 8, ly);
	const struct vtx v[2] = {
		{ 0, 0, 0, 0, 0, { 127, 0, 0, 255 } },  /* normal +x: s al máximo */
		{ 0, 0, 0, 0, 0, { 0, 0, 127, 255 } },  /* normal +z: centro */
	};
	dl(0x01000000 | 2 << 12 | 2 << 1, put_vertices(v, 2));
	run();
	/* (d + 1)·512 texels × 0x07C0/65536: 31 al máximo y 15,5 al centro. */
	CHECK(fabsf(gs.vtx[0].s - 31.0f) < 0.05f);
	CHECK(fabsf(gs.vtx[0].t - 15.5f) < 0.05f);
	CHECK(fabsf(gs.vtx[1].s - 15.5f) < 0.05f);
}

/* Textura RGBA16 de 4×2 cargada con LOADBLOCK (dxt de una fila de 8 bytes:
 * 2048, la segunda fila va intercambiada) y dibujada en un rectángulo. */
static void gfx_tmem_rgba16_block(void)
{
	start();
	uint16_t texels[8];
	for (int i = 0; i < 8; i++)
		texels[i] = (uint16_t)((i * 4) << 11 | 1);  /* rojo creciente, opaco */
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * i, texels[i]);
	dl(0xFD100000 | 0, TEX_PA);                         /* SETTIMG RGBA16, ancho 1 */
	dl(0xF5100000, 0x07000000);                         /* SETTILE 7 (de carga), tmem 0 */
	dl(0xF3000000, 0x07000000 | 7 << 12 | 2048);         /* LOADBLOCK 8 texels, dxt 1 línea */
	dl(0xF5100000 | 1 << 9, 0x00000000);                 /* SETTILE 0: line 1 palabra */
	dl(0xF2000000, 0x00000000 | (3 << 2) << 12 | (1 << 2)); /* tile 0: 4×2 */
	set_cycle(G_CYC_COPY);
	/* TEXRECT 0,0–3,1 (copia: inclusivo), s=t=0, dsdx = 4.0. */
	dl(0xE4000000 | (3 << 2) << 12 | (1 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	run();
	CHECK_EQ(res.rects, 1);
	CHECK_EQ(res.textures, 1);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->cycle, G_CYC_COPY);
	CHECK_EQ(d->ntex, 1);
	CHECK_EQ(d->tex[0].width, 4);
	CHECK_EQ(d->tex[0].height, 2);
	CHECK_EQ(d->h.b, 6);
	const uint8_t *tex = out_tex + d->tex[0].offset;
	for (int y = 0; y < 2; y++)
		for (int x = 0; x < 4; x++) {
			uint32_t r = (uint32_t)(y * 4 + x) * 4;
			CHECK_EQ(tex[y * d->tex[0].pitch + 4 * x], r << 3 | r >> 2);
			CHECK_EQ(tex[y * d->tex[0].pitch + 4 * x + 3], 0xFF);
		}
	/* Esquinas: 0..4 × 0..2 píxeles, s 0..4, t 0..2 texels. */
	CHECK(out_vtx[0].x == 0.0f && out_vtx[0].y == 0.0f && out_vtx[0].s == 0.0f);
	CHECK(out_vtx[1].x == 4.0f && fabsf(out_vtx[1].s - 4.0f) < 1e-5f);
	CHECK(out_vtx[2].y == 2.0f && fabsf(out_vtx[2].t - 2.0f) < 1e-5f);
	/* u = s·xf0 + xf2 normaliza a la textura. */
	CHECK(fabsf(d->tex[0].xf[0] - 0.25f) < 1e-6f);
}

/* (TEXEL0 − SHADE)·TEXEL0_ALPHA + SHADE con alfa ENV (G_SETCOMBINE de los
 * ojos de Mario en SM64): T0·T0a pide la textura premultiplicada y queda
 * exacto; con T0 solo además, se sigue aproximando. */
#define CC_EYES_HI 0x147E28u
#define CC_EYES_LO 0x44FE7B3Du

static void gfx_combiner_premul(void)
{
	start();
	struct gfx_draw d;
	memset(&d, 0, sizeof d);
	gs.oh = 0;
	gs.env = 0x000000C0;
	gs.cc_hi = CC_EYES_HI;
	gs.cc_lo = CC_EYES_LO;
	CHECK(!gfx_combine(&gs, &d));
	CHECK(d.tex[0].flags & GFX_TEX_PREMUL);
	CHECK_EQ(d.ntex, 1);
	for (int lane = 0; lane < 3; lane++) {
		CHECK(d.k[GFX_K_T0][0][lane] == 1.0f);    /* T0·T0a: la premultiplicada */
		CHECK(d.k[GFX_K_T0A][1][lane] == -1.0f);  /* −S·T0a */
		CHECK(d.k[GFX_K_ONE][1][lane] == 1.0f);   /* + S */
	}
	CHECK(fabsf(d.k[GFX_K_ONE][0][3] - 0xC0 / 255.0f) < 1e-6f);  /* alfa = ENV */

	/* (TEXEL0 − 0)·TEXEL0_ALPHA + TEXEL0: T0·T0a + T0. */
	gs.cc_lo = 15u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 1u << 15 | 7u << 12 | 5u << 9 | 1u << 6 | 7u << 3 | 5u;
	CHECK(gfx_combine(&gs, &d));
	CHECK(!(d.tex[0].flags & GFX_TEX_PREMUL));
}

/* La misma textura premultiplicada al decodificar: alfa 0 → negro. */
static void gfx_tmem_premul(void)
{
	start();
	for (int i = 0; i < 8; i++)  /* amarillo; alfa 0 en los pares, 1 en los impares */
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * i, (uint16_t)(31u << 11 | 31u << 6 | (unsigned)(i & 1)));
	dl(0xFD100000, TEX_PA);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 7 << 12 | 2048);
	dl(0xF5100000 | 1 << 9, 0x00000000);
	dl(0xF2000000, (3 << 2) << 12 | (1 << 2));
	set_cycle(G_CYC_1CYCLE);
	dl(0xFC000000 | CC_EYES_HI, CC_EYES_LO);
	dl(0xE4000000 | (4 << 2) << 12 | (2 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
	run();
	CHECK_EQ(res.approx_cc, 0);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK(d->tex[0].flags & GFX_TEX_PREMUL);
	const uint8_t *tex = out_tex + d->tex[0].offset;
	CHECK_EQ(tex[0], 0);  /* texel 0, alfa 0 */
	CHECK_EQ(tex[1], 0);
	CHECK_EQ(tex[3], 0);
	CHECK_EQ(tex[4], 0xFF);  /* texel 1, opaco: sin cambios */
	CHECK_EQ(tex[5], 0xFF);
	CHECK_EQ(tex[6], 0);
	CHECK_EQ(tex[7], 0xFF);
}

/* CI4 con paleta RGBA16 (LOADTLUT) cargada con LOADTILE. */
static void gfx_tmem_ci4_tlut(void)
{
	start();
	/* Paleta de 16 colores: entrada i = azul i·2, opaco. */
	for (int i = 0; i < 16; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 0x100 + 2 * i, (uint16_t)((i * 2) << 1 | 1));
	/* Textura 8×2 CI4: índices 0..15. */
	for (int i = 0; i < 8; i++)
		t_n64.bus.rdram[TEX_PA + i] = (uint8_t)((2 * i) << 4 | (2 * i + 1));
	/* TLUT: SETTIMG RGBA16, SETTILE 7 en tmem 256 (palabra 0x100), LOADTLUT 16. */
	dl(0xFD100000, TEX_PA + 0x100);
	dl(0xF5000100, 0x07000000);
	dl(0xF0000000, 0x07000000 | (15 << 2) << 12);
	/* Textura: SETTIMG CI8 de 4 bytes de ancho (8 texels de 4 bits). */
	dl(0xFD480000 | 3, TEX_PA);
	dl(0xF5480000 | 1 << 9, 0x07000000);
	dl(0xF4000000, 0x07000000 | (3 << 2) << 12 | (1 << 2));  /* LOADTILE 4×2 bytes */
	dl(0xF5400000 | 1 << 9, 0x00000000);                      /* tile 0: CI4, línea 8 bytes */
	dl(0xF2000000, (7 << 2) << 12 | (1 << 2));                /* 8×2 */
	dl(0xE3000000 | (32 - 14 - 2) << 8 | 1, 2u << 14);        /* TLUT RGBA16 */
	set_cycle(G_CYC_COPY);
	dl(0xE4000000 | (7 << 2) << 12 | (0 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->tex[0].width, 8);
	const uint8_t *tex = out_tex + d->tex[0].offset;
	for (int x = 0; x < 8; x++) {
		uint32_t b5 = (uint32_t)x * 2;
		CHECK_EQ(tex[4 * x + 2], b5 << 3 | b5 >> 2);
	}
	/* Segunda fila (impar): la carga la intercambió y la lectura lo deshace. */
	for (int x = 0; x < 8; x++) {
		uint32_t b5 = (uint32_t)(x + 8) * 2;
		CHECK_EQ(tex[d->tex[0].pitch + 4 * x + 2], b5 << 3 | b5 >> 2);
	}
}

static void gfx_fill_and_zclear(void)
{
	start();
	dl(0xFE000000, 0x00200000);           /* z image */
	dl(0xFF100000 | 319, 0x00200000);     /* color image = z image */
	set_cycle(G_CYC_FILL);
	dl(0xF7000000, 0xFFFCFFFC);
	dl(0xF6000000 | (319 << 2) << 12 | (239 << 2), 0);
	dl(0xFF100000 | 319, 0x00100000);     /* color image de verdad */
	dl(0xF7000000, 0xF801F801);           /* rojo en RGBA5551 */
	dl(0xF6000000 | (319 << 2) << 12 | (239 << 2), 0);
	run();
	const struct gfx_zclear *z = (const struct gfx_zclear *)find_cmd(GFX_OP_ZCLEAR, 0);
	CHECK(z != NULL);
	if (z) {
		CHECK_EQ(z->h.a, 0x200000);
		CHECK_EQ(z->h.b, 0xFFFC);
		CHECK_EQ(z->rect[2], 320);
		CHECK_EQ(z->rect[3], 240);
	}
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (d) {
		CHECK_EQ(d->cycle, G_CYC_FILL);
		CHECK(d->k[GFX_K_ONE][0][0] == 1.0f && d->k[GFX_K_ONE][0][1] == 0.0f);
		CHECK_EQ(d->h.b, 6);
	}
	CHECK(out_vtx[5].x == 320.0f && out_vtx[5].y == 240.0f);
	/* El TARGET de la color image real llega antes del dibujo. */
	CHECK(find_cmd(GFX_OP_TARGET, 0) != NULL);
}

/* FILLRECT en 1 ciclo: borde inferior derecho exclusivo, salvo cuando se
 * queda a un píxel del scissor (el fundido de Zelda manda 319, 239). */
static void gfx_fill_1cycle_edge(void)
{
	start();
	dl(0xFF100000 | 319, 0x00100000);
	dl(0xED000000, (320 << 2) << 12 | (240 << 2));  /* scissor 320×240 */
	set_cycle(G_CYC_1CYCLE);
	dl(0xF6000000 | (100 << 2) << 12 | (50 << 2), (10 << 2) << 12 | (10 << 2));
	dl(0xF6000000 | (319 << 2) << 12 | (239 << 2), 0);
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (d)
		CHECK_EQ(d->h.b, 12);
	/* Vértice 5: abajo a la derecha de cada rectángulo. */
	CHECK(out_vtx[5].x == 100.0f && out_vtx[5].y == 50.0f);
	CHECK(out_vtx[11].x == 320.0f && out_vtx[11].y == 240.0f);
}

/* Triángulos seguidos con el mismo estado comparten dibujo; un cambio de
 * color primitivo abre otro. */
static void gfx_draw_batching(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_PRIM, CC_ZERO, CC_ZERO, CC_ZERO, CC_PRIM);
	const struct vtx v[4] = {
		{ 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } }, { 1, 1, 0, 0, 0, { 0 } },
	};
	dl(0x01000000 | 4 << 12 | 4 << 1, put_vertices(v, 4));
	dl(0xFA000000, 0xFF0000FF);
	dl(0x06000000 | 0 << 16 | 1 << 9 | 2 << 1, 1 << 17 | 3 << 9 | 2 << 1);  /* TRI2 */
	dl(0xE7000000, 0);                                                     /* pipesync: nada */
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	dl(0xFA000000, 0x00FF00FF);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	run();
	CHECK_EQ(res.triangles, 4);
	CHECK_EQ(res.draws, 2);
	const struct gfx_draw *a = draw_n(0), *b = draw_n(1);
	CHECK(a && b);
	if (a && b) {
		CHECK_EQ(a->h.b, 9);
		CHECK_EQ(b->h.a, 9);
		CHECK_EQ(b->h.b, 3);
		CHECK(a->k[GFX_K_ONE][0][0] == 1.0f && b->k[GFX_K_ONE][0][1] == 1.0f);
	}
}

/* Display lists anidadas, segmentos, CULLDL y BRANCH_Z. */
static void gfx_dl_flow(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0xDB060000 | 3 * 4, 0x00010000);  /* segmento 3 → 0x10000 */
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	uint32_t va = put_vertices(v, 3);
	/* Subrutina en 0x18000 (segmento 3 + 0x8000): un triángulo. */
	uint32_t main_pc = dl_pc;
	dl_pc = DL_PA + 0x8000;
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	dl(0xDF000000, 0);
	dl_pc = main_pc;
	dl(0x01000000 | 3 << 12 | 3 << 1, va);
	dl(0xDE000000, 0x03008000);          /* G_DL (llamada) por segmento */
	dl(0xDE000000, 0x03008000);          /* otra vez */
	/* Vértices fuera por la derecha: CULLDL termina la lista. */
	const struct vtx far[3] = { { 5, 0, 0, 0, 0, { 0 } }, { 6, 0, 0, 0, 0, { 0 } }, { 7, 1, 0, 0, 0, { 0 } } };
	dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(far, 3));
	dl(0x03000000 | 0, 2 * 2);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* no se ejecuta */
	run();
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(res.dl_errors, 0);
}

/* G_LOAD_UCODE a otro microcódigo (S2DEX en Zelda OoT): sus comandos se
 * saltan, siguiendo G_DL y G_ENDDL, hasta que la lista vuelve a cargar el
 * microcódigo de la tarea; lo que sigue se ejecuta y el G_RDPFULLSYNC final
 * cuenta. Cargar el mismo microcódigo no cambia nada. */
static void gfx_load_ucode_skips_foreign(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	uint32_t va = put_vertices(v, 3);
	dl(0x01000000 | 3 << 12 | 3 << 1, va);
	/* Mismo microcódigo: sigue. */
	dl(0xE1000000, 0x80040000);
	dl(0xDD000FFF, 0x80030000);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* triángulo 1 */
	/* Otro microcódigo: un tramo con una subrutina, comandos que en F3DEX2
	 * serían triángulos (no deben dibujarse) y la vuelta al nuestro. */
	dl(0xE1000000, 0x80050000);
	dl(0xDD000FFF, 0x80031000);
	uint32_t main_pc = dl_pc;
	dl_pc = DL_PA + 0x8000;
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* no se ejecuta */
	dl(0x0A000000, 0x80060000);                       /* G_BG_COPY de S2DEX */
	dl(0xDF000000, 0);
	dl_pc = main_pc;
	dl(0xDE000000, 0x80000000 | (DL_PA + 0x8000));   /* llamada */
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* no se ejecuta */
	dl(0xE1000000, 0x80040000);
	dl(0xDD000FFF, 0x80030000);                       /* de vuelta */
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* triángulo 2 */
	dl(0xE9000000, 0);                                /* G_RDPFULLSYNC */
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(res.skipped, 7);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_LOAD_UCODE | GFX_RES_LOOP),
		 GFX_RES_FULLSYNC | GFX_RES_LOAD_UCODE);
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_FULLSYNC);

	/* Un tramo ajeno que nunca vuelve: la lista termina en su G_ENDDL y
	 * cuenta como cortada (no se sabe qué hizo; el juego espera al RDP). */
	next_task();
	dl(0xE1000000, 0x80050000);
	dl(0xDD000FFF, 0x80031000);
	dl(0x0A000000, 0x80060000);
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.triangles, 0);
	CHECK_EQ(res.skipped, 2);
	CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_FOREIGN_END), GFX_RES_FOREIGN_END);
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_CUT);

	/* El G_RDPFULLSYNC dentro del tramo ajeno (comando del RDP, común a la
	 * familia) sí cuenta: DP sin cortar. */
	next_task();
	dl(0xE1000000, 0x80050000);
	dl(0xDD000FFF, 0x80031000);
	dl(0x0A000000, 0x80060000);
	dl(0xE9000000, 0);
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.skipped, 3);
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_FULLSYNC | SALSA64_GFX_CUT);

	/* Sin tramo ajeno y sin FULLSYNC (G_ENDDL): ni DP ni corte. */
	next_task();
	run();
	CHECK_EQ(salsa64_gfx_task_flags(&res), 0);
	res.flags |= GFX_RES_LOOP;
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_CUT);
}

/* ---- Caché de texturas ---- */

/* RGBA16 4×2 desde TEX_PA con LOADBLOCK y un TEXRECT en copia. */
static void dl_rgba16_rect(void)
{
	dl(0xFD100000 | 0, TEX_PA);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 7 << 12 | 2048);
	dl(0xF5100000 | 1 << 9, 0x00000000);
	dl(0xF2000000, 0x00000000 | (3 << 2) << 12 | (1 << 2));
	set_cycle(G_CYC_COPY);
	dl(0xE4000000 | (3 << 2) << 12 | (1 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
}

static const uint8_t *tex_of(const struct gfx_tex *t)
{
	return (t->where == GFX_TEX_CACHE ? out_cache : out_tex) + t->offset;
}

/* La misma textura en otra tarea sale de la caché; si cambia un texel en la
 * RDRAM (el último del tramo cargado), se vuelve a decodificar. */
static void gfx_texcache_across_tasks(void)
{
	start();
	cache_task = 1;
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * i, (uint16_t)((i * 4) << 11 | 1));
	dl_rgba16_rect();
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(res.textures, 1);
	CHECK_EQ(res.tex_old, 0);
	CHECK_EQ(d->tex[0].where, GFX_TEX_CACHE);
	uint32_t first = d->tex[0].offset;
	CHECK_EQ(tex_of(&d->tex[0])[d->tex[0].pitch + 4 * 3], 28u << 3 | 28u >> 2);

	next_task();
	dl_rgba16_rect();
	run();
	d = draw_n(0);
	CHECK_EQ(res.textures, 0);
	CHECK_EQ(res.tex_old, 1);
	CHECK(d && d->tex[0].where == GFX_TEX_CACHE && d->tex[0].offset == first);

	/* Texel (3, 1): el último de los 16 bytes cargados. */
	salsa64_wr16(t_n64.bus.rdram + TEX_PA + 14, (uint16_t)(31u << 6 | 1));
	next_task();
	dl_rgba16_rect();
	run();
	d = draw_n(0);
	CHECK_EQ(res.textures, 1);
	if (!d)
		return;
	CHECK(d->tex[0].offset != first);
	const uint8_t *p = tex_of(&d->tex[0]) + d->tex[0].pitch + 4 * 3;
	CHECK_EQ(p[0], 0);
	CHECK_EQ(p[1], 0xFF);

	/* De vuelta al original: la primera sigue en la caché. */
	salsa64_wr16(t_n64.bus.rdram + TEX_PA + 14, (uint16_t)(28u << 11 | 1));
	next_task();
	dl_rgba16_rect();
	run();
	d = draw_n(0);
	CHECK_EQ(res.textures, 0);
	CHECK(d && d->tex[0].offset == first);

	/* Con GFX_JOB_NOCACHE, al arena. */
	next_task();
	dl_rgba16_rect();
	dl(0xDF000000, 0);
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	job.cache_ea = (uint64_t)(uintptr_t)out_cache;
	job.cache_cap = sizeof out_cache;
	job.task = cache_task;
	job.done_task = cache_task - 1;
	job.flags = GFX_JOB_NOCACHE;
	job.gbi = SALSA64_GBI_F3DEX2;
	job.dl = DL_PA;
	gfx_run(&gs, &job, &res);
	d = draw_n(0);
	CHECK_EQ(res.textures, 1);
	CHECK(d && d->tex[0].where == GFX_TEX_ARENA);
}

/* CI4 con paleta: si cambia una entrada de la paleta en la RDRAM, la
 * textura es otra. */
static void dl_ci4_rect(void)
{
	dl(0xFD100000, TEX_PA + 0x100);
	dl(0xF5000100, 0x07000000);
	dl(0xF0000000, 0x07000000 | (15 << 2) << 12);
	dl(0xFD480000 | 3, TEX_PA);
	dl(0xF5480000 | 1 << 9, 0x07000000);
	dl(0xF4000000, 0x07000000 | (3 << 2) << 12 | (1 << 2));
	dl(0xF5400000 | 1 << 9, 0x00000000);
	dl(0xF2000000, (7 << 2) << 12 | (1 << 2));
	dl(0xE3000000 | (32 - 14 - 2) << 8 | 1, 2u << 14);
	set_cycle(G_CYC_COPY);
	dl(0xE4000000 | (7 << 2) << 12 | (0 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
}

static void gfx_texcache_palette(void)
{
	start();
	cache_task = 1;
	for (int i = 0; i < 16; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 0x100 + 2 * i, (uint16_t)((i * 2) << 1 | 1));
	for (int i = 0; i < 8; i++)
		t_n64.bus.rdram[TEX_PA + i] = (uint8_t)((2 * i) << 4 | (2 * i + 1));
	dl_ci4_rect();
	run();
	CHECK_EQ(res.textures, 1);
	next_task();
	dl_ci4_rect();
	run();
	CHECK_EQ(res.textures, 0);
	/* Entrada 3: azul 6 → verde 31. */
	salsa64_wr16(t_n64.bus.rdram + TEX_PA + 0x100 + 6, (uint16_t)(31u << 6 | 1));
	next_task();
	dl_ci4_rect();
	run();
	CHECK_EQ(res.textures, 1);
	const struct gfx_draw *d = draw_n(0);
	if (!d)
		return;
	const uint8_t *p = tex_of(&d->tex[0]);
	CHECK_EQ(p[4 * 3 + 1], 0xFF);  /* texel 3: verde */
	CHECK_EQ(p[4 * 3 + 2], 0);
	CHECK_EQ(p[4 * 4 + 2], 8u << 3 | 8u >> 2);  /* texel 4: azul 8, igual */
}

/* El anillo de la caché: con el consumidor dos tareas atrás, nunca se
 * desaloja ni se pisa una textura que aún puede leer; las entradas no se
 * enciman y el índice las encuentra después de moverse. */
static uint32_t lcg(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

/* Tareas con `ops` usos de claves al azar (de `keys` distintas; tamaño
 * 128 o de 128 a 3200 bytes) sobre una región de `cap` bytes. */
static void ring_run(uint32_t cap, uint32_t keys, int ops, bool small, uint32_t *ins, uint32_t *hit, uint32_t *fail)
{
	static struct gfx_state g;
	memset(&g, 0, sizeof g);
	memset(&job, 0, sizeof job);
	memset(&res, 0, sizeof res);
	g.job = &job;
	g.res = &res;
	job.cache_ea = (uint64_t)(uintptr_t)out_cache;  /* el directorio no la toca */
	job.cache_cap = cap;
	enum { KEYS = 4000 };
	static uint32_t last[KEYS], off[KEYS];
	memset(last, 0, sizeof last);
	uint32_t seed = 1;
	*ins = *hit = *fail = 0;
	for (uint32_t task = 1; task <= 400; task++) {
		job.task = task;
		job.done_task = task - 3;
		gfx_tc_begin(&g);
		CHECK(g.tc_on);
		for (int n = 0; n < ops; n++) {
			uint32_t k = lcg(&seed) % (task < 200 ? keys / 8 : keys);
			uint32_t key[2] = { k * 0x9E3779B1u, k };
			bool old;
			const struct gfx_tc_entry *e = gfx_tc_find(&g, key, &old);
			if (e) {
				(*hit)++;
				CHECK_EQ(e->off, off[k]);
				CHECK_EQ(old, last[k] != task);
				last[k] = task;
				continue;
			}
			uint32_t sz = small ? 128 : 128 + (lcg(&seed) % 4) * 1000;
			struct gfx_tc_entry *ne = gfx_tc_insert(&g, key, sz);
			if (!ne) {
				(*fail)++;
				continue;
			}
			(*ins)++;
			CHECK(ne->off + ne->size <= g.tc.cap);
			off[k] = ne->off;
			last[k] = task;
		}
		/* Las que se usaron después de done_task siguen donde estaban. */
		for (uint32_t k = 0; k < keys; k++)
			if (last[k] && (int32_t)(last[k] - job.done_task) > 0) {
				bool found = false;
				for (uint32_t i = 0; i < g.tc.count; i++) {
					const struct gfx_tc_entry *e = &g.tc.e[(g.tc.tail + i) % GFX_TC_ENTRIES];
					if (e->key[1] == k) {
						found = true;
						CHECK_EQ(e->off, off[k]);
					}
				}
				CHECK(found);
			}
		/* En el orden de la región desde head_off y sin encimarse. */
		uint32_t prev_end = g.tc.head_off;
		bool wrapped = false;
		for (uint32_t i = 0; i < g.tc.count; i++) {
			const struct gfx_tc_entry *e = &g.tc.e[(g.tc.tail + i) % GFX_TC_ENTRIES];
			if (e->off < prev_end) {
				CHECK(!wrapped);
				wrapped = true;
			}
			CHECK(!wrapped || e->off + e->size <= g.tc.head_off);
			prev_end = e->off + e->size;
		}
	}
}

static void gfx_texcache_ring(void)
{
	start();
	uint32_t ins, hit, fail;
	/* Texturas grandes: la región se llena antes que el directorio. */
	ring_run(40000, 300, 8, false, &ins, &hit, &fail);
	CHECK(ins > 500 && hit > 500 && res.tex_evicted > 300);
	CHECK(fail < ins / 10);  /* lo que no cabe va al arena */
	/* Texturas chicas y muchas claves: se llena el directorio. */
	ring_run(1u << 20, 4000, 40, true, &ins, &hit, &fail);
	CHECK(ins > 4000 && hit > 1000 && res.tex_evicted > 3000);
	CHECK_EQ(fail, 0);
}

/* El hash ve exactamente el tramo pedido, también al dar la vuelta. */
static void gfx_texcache_hash(void)
{
	static uint8_t ring[4096] __attribute__((aligned(16)));
	for (int i = 0; i < 4096; i++)
		ring[i] = (uint8_t)(i * 7 + 3);
	uint32_t k0[2], k1[2];
	struct gfx_hash h;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 8, 24);  /* bytes 8..31 */
	gfx_hash_final(&h, k0);
	ring[7] ^= 1;
	ring[32] ^= 1;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 8, 24);
	gfx_hash_final(&h, k1);
	CHECK(k0[0] == k1[0] && k0[1] == k1[1]);
	ring[31] ^= 1;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 8, 24);
	gfx_hash_final(&h, k1);
	CHECK(k0[0] != k1[0] && k0[1] != k1[1]);
	/* Vuelta: 4088..4095 y 0..7. */
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 4088, 16);
	gfx_hash_final(&h, k0);
	ring[3] ^= 0x80;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 4088, 16);
	gfx_hash_final(&h, k1);
	CHECK(k0[0] != k1[0]);
	ring[8] ^= 0x80;
	ring[4087] ^= 0x80;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 4088, 16);
	gfx_hash_final(&h, k0);
	CHECK(k0[0] == k1[0] && k0[1] == k1[1]);
	/* Mismo contenido en otro lugar alineado igual: misma clave. */
	memcpy(ring + 1024, ring + 512, 256);
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 512, 256);
	gfx_hash_final(&h, k0);
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 1024, 256);
	gfx_hash_final(&h, k1);
	CHECK(k0[0] == k1[0] && k0[1] == k1[1]);
}

void t_suite_gfx(void)
{
	RUN(gfx_combiner_forms);
	RUN(gfx_triangle_transform);
	RUN(gfx_matrix_stack);
	RUN(gfx_lighting);
	RUN(gfx_texgen);
	RUN(gfx_tmem_rgba16_block);
	RUN(gfx_tmem_ci4_tlut);
	RUN(gfx_combiner_premul);
	RUN(gfx_tmem_premul);
	RUN(gfx_fill_and_zclear);
	RUN(gfx_fill_1cycle_edge);
	RUN(gfx_draw_batching);
	RUN(gfx_dl_flow);
	RUN(gfx_load_ucode_skips_foreign);
	RUN(gfx_texcache_across_tasks);
	RUN(gfx_texcache_palette);
	RUN(gfx_texcache_ring);
	RUN(gfx_texcache_hash);
}
