/*
 * Salsa64 — pruebas de las variantes de Rare de la HLE gráfica: el
 * microcódigo de Perfect Dark (src/rsp/gfx/f3dpd.c, sobre F3D), el de
 * Diddy Kong Racing (src/rsp/gfx/f3ddkr.c, sobre F3D) y el de Conker
 * (src/rsp/gfx/f3dexbg.c, sobre F3DEX2). Display lists sintéticas
 * con su codificación, ejecutadas con gfx_run y revisadas en el estado y la
 * salida. Nada viene de los juegos: vértices, luces y tablas son inventados.
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define DL_PA   0x10000u
#define DATA_PA 0x20000u

static struct gfx_state gs;
static struct gfx_vertex out_vtx[1024] __attribute__((aligned(16)));
static uint8_t out_cmd[16 * 1024] __attribute__((aligned(16)));
static uint8_t out_tex[64 * 1024] __attribute__((aligned(128)));
static struct gfx_job job;
static struct gfx_result res;
static uint32_t dl_pc, data_pc;
static enum salsa64_gbi gbi;

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

static void start(enum salsa64_gbi g)
{
	gbi = g;
	dl_pc = DL_PA;
	data_pc = DATA_PA;
	gfx_init(&gs);
}

/* Termina la lista (G_ENDDL de cada familia) y ejecuta la tarea. */
static void run(void)
{
	dl(gbi == SALSA64_GBI_F3DPD || gbi == SALSA64_GBI_F3DDKR || gbi == SALSA64_GBI_F3DJFG ? 0xB8000000 : 0xDF000000, 0);
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	job.gbi = gbi;
	job.dl = DL_PA;
	gfx_run(&gs, &job, &res);
}

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

static uint32_t put_viewport_320x240(void)
{
	uint32_t a = data_alloc(16);
	const int16_t vp[8] = { 640, 480, 511, 0, 640, 480, 511, 0 };
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, (uint16_t)vp[i]);
	return a;
}

/* ---- Perfect Dark (F3D de Rare) ---- */

/* Vértice de 12 bytes: x, y, z, banderas, color (offset en la tabla), s, t. */
struct pd_vtx { int16_t x, y, z; uint8_t ci; int16_t s, t; };

static uint32_t pd_put_vertices(const struct pd_vtx *v, int n)
{
	uint32_t a = data_alloc(12 * (uint32_t)n);
	for (int i = 0; i < n; i++) {
		uint8_t *p = t_n64.bus.rdram + a + 12 * i;
		salsa64_wr16(p + 0, (uint16_t)v[i].x);
		salsa64_wr16(p + 2, (uint16_t)v[i].y);
		salsa64_wr16(p + 4, (uint16_t)v[i].z);
		p[6] = 0xA5;  /* banderas: no cambian nada */
		p[7] = v[i].ci;
		salsa64_wr16(p + 8, (uint16_t)v[i].s);
		salsa64_wr16(p + 10, (uint16_t)v[i].t);
	}
	return a;
}

static void pd_setup(void)
{
	dl(0x03800010, put_viewport_320x240());              /* G_MOVEMEM viewport */
	dl(0x01010040, put_matrix(ident));                   /* proyección, carga */
	dl(0x01020040, put_matrix(ident));                   /* modelview, carga */
	dl(0xFF100000 | 319, 0x00100000);                    /* color image */
	/* Combiner SHADE en los dos ciclos. */
	dl(0xFC000000 | (15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u),
	   7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 4u << 15 | 7u << 12 | 4u << 9 | 4u << 6 | 7u << 3 | 4u);
	dl(0xB6000000, 0xFFFFFFFF);                          /* G_CLEARGEOMETRYMODE */
	dl(0xB7000000, 0x00000204);                          /* SHADE | suave (de F3D) */
}

static void pd_vertex(uint32_t addr, unsigned n, unsigned v0)
{
	dl(0x04000000 | (n - 1) << 20 | v0 << 16 | n * 12, addr);
}

/* G_VTXCOLORBASE (segmentada) y el color de cada vértice por su offset en
 * la tabla; índices de G_TRI1 ×10 como en F3D. */
static void pd_vertex_colors(void)
{
	start(SALSA64_GBI_F3DPD);
	pd_setup();
	uint32_t table = data_alloc(16);
	const uint8_t cols[16] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 1, 2, 3, 4 };
	memcpy(t_n64.bus.rdram + table, cols, sizeof cols);
	dl(0xBC000006 | (5 * 4) << 8, table - 0x100);        /* segmento 5 */
	dl(0x07000000, 0x05000100);                          /* G_VTXCOLORBASE 5:0x100 */
	const struct pd_vtx v[3] = {
		{ 0, 0, 0, 8, 32, 64 }, { 1, 0, 0, 0, 0, 0 }, { 0, 1, 0, 12, 0, 0 },
	};
	pd_vertex(pd_put_vertices(v, 3), 3, 2);
	dl(0xBF000000, 2 * 10 << 16 | 3 * 10 << 8 | 4 * 10);
	dl(0xE9000000, 0);                                   /* G_RDPFULLSYNC */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.triangles, 1);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK_EQ(gs.rare.color_base, table);
	CHECK_EQ(gs.vtx[2].c[0], 90);
	CHECK_EQ(gs.vtx[2].c[3], 120);
	CHECK_EQ(gs.vtx[3].c[1], 20);
	CHECK_EQ(gs.vtx[4].c[0], 1);
	CHECK_EQ(gs.vtx[4].c[3], 4);
	CHECK(fabsf(gs.vtx[3].x - 1.0f) < 1e-6f && fabsf(gs.vtx[4].y - 1.0f) < 1e-6f);
	/* s, t en s10.5 sin escala de G_TEXTURE (0): 0 texels. */
	CHECK(out_vtx[0].x == 160.0f && out_vtx[1].x == 320.0f && out_vtx[2].y == 0.0f);
	CHECK_EQ(out_vtx[0].r, 90);
	CHECK_EQ(out_vtx[2].a, 4);
}

/* Con G_LIGHTING la tabla trae la normal (X, Y, Z con signo) y el alfa. */
static void pd_lighting(void)
{
	start(SALSA64_GBI_F3DPD);
	pd_setup();
	dl(0xB7000000, 0x00020000);                          /* G_LIGHTING (de F3D) */
	dl(0xBC000002, 0x80000000u + (1 + 1) * 32);          /* una luz */
	uint32_t l0 = data_alloc(16), l1 = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram;
	p[l0] = p[l0 + 1] = p[l0 + 2] = 200;
	p[l0 + 10] = 127;                                    /* hacia +z */
	p[l1] = p[l1 + 1] = p[l1 + 2] = 20;                  /* ambiental */
	dl(0x03860010, l0);
	dl(0x03880010, l1);
	uint32_t table = data_alloc(8);
	const uint8_t nrm[8] = { 0, 0, 127, 0x40, 0, 0, (uint8_t)-127, 0x80 };
	memcpy(p + table, nrm, sizeof nrm);
	dl(0x07000000, table);
	const struct pd_vtx v[2] = { { 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 4, 0, 0 } };
	pd_vertex(pd_put_vertices(v, 2), 2, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(gs.vtx[0].c[0], 220);
	CHECK_EQ(gs.vtx[0].c[3], 0x40);
	CHECK_EQ(gs.vtx[1].c[2], 20);
	CHECK_EQ(gs.vtx[1].c[3], 0x80);
}

/* G_TRI4 (0xB1) de la familia de Rare y G_QUAD (0xB5, índices ×10). */
static void pd_tri4_quad(void)
{
	start(SALSA64_GBI_F3DPD);
	pd_setup();
	dl(0x01020040, put_matrix((const float[16]){ 0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 1 }));
	uint32_t table = data_alloc(64);
	for (int i = 0; i < 16; i++)
		t_n64.bus.rdram[table + 4 * i] = (uint8_t)(i * 16);
	dl(0x07000000, table);
	struct pd_vtx v[16];
	memset(v, 0, sizeof v);
	for (int i = 0; i < 16; i++) {
		v[i].x = (int16_t)(i & 3);
		v[i].y = (int16_t)(i >> 2);
		v[i].ci = (uint8_t)(4 * i);
	}
	pd_vertex(pd_put_vertices(v, 16), 16, 0);
	/* (0,1,4) y (5,9,8); el tercero vacío. */
	dl(0xB1000000 | 0u << 8 | 5u << 4 | 0u, 0u << 16 | 8u << 12 | 9u << 8 | 4u << 4 | 1u);
	dl(0xB5000000, 10u * 10 << 24 | 11u * 10 << 16 | 15u * 10 << 8 | 14u * 10);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 4);
	CHECK_EQ(out_vtx[0].r, 0 * 16);
	CHECK_EQ(out_vtx[2].r, 4 * 16);
	CHECK_EQ(out_vtx[5].r, 8 * 16);
	/* El cuadrilátero: (10, 11, 15) y (10, 15, 14). */
	CHECK_EQ(out_vtx[6].r, 10 * 16);
	CHECK_EQ(out_vtx[8].r, 15 * 16);
	CHECK_EQ(out_vtx[11].r, 14 * 16);
}

/* ---- Conker (F3DEX2 de Rare) ---- */

struct vtx { int16_t x, y, z; uint16_t flags; int16_t s, t; uint8_t c[4]; };

static uint32_t put_vertices(const struct vtx *v, int n)
{
	uint32_t a = data_alloc(16 * (uint32_t)n);
	for (int i = 0; i < n; i++) {
		uint8_t *p = t_n64.bus.rdram + a + 16 * i;
		salsa64_wr16(p + 0, (uint16_t)v[i].x);
		salsa64_wr16(p + 2, (uint16_t)v[i].y);
		salsa64_wr16(p + 4, (uint16_t)v[i].z);
		salsa64_wr16(p + 6, v[i].flags);
		salsa64_wr16(p + 8, (uint16_t)v[i].s);
		salsa64_wr16(p + 10, (uint16_t)v[i].t);
		memcpy(p + 12, v[i].c, 4);
	}
	return a;
}

static void cbfd_vertex(uint32_t addr, unsigned n, unsigned v0)
{
	dl(0x01000000 | n << 12 | (v0 + n) << 1, addr);
}

static void cbfd_setup(void)
{
	dl(0xDC080008, put_viewport_320x240());              /* G_MOVEMEM viewport */
	dl(0xDA380007, put_matrix(ident));                   /* proyección, carga */
	dl(0xDA380003, put_matrix(ident));                   /* modelview, carga */
	dl(0xFF100000 | 319, 0x00100000);
	dl(0xFC000000 | (15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u),
	   7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 4u << 15 | 7u << 12 | 4u << 9 | 4u << 6 | 7u << 3 | 4u);
	dl(0xD9000000, G_SHADE | G_SHADING_SMOOTH);          /* G_GEOMETRYMODE */
}

/* G_TRI4 en 0x10–0x1F con índices de 5 bits (el primero toma los 4 bits
 * bajos del opcode y el bit 23; el tercero 3 bits de w0 y 2 de w1). */
static uint64_t cbfd_tri4_words(const unsigned v[12])
{
	uint32_t w0 = 0x10000000u | (v[0] & 31) << 23 | (v[1] & 31) << 18 | (v[2] >> 2 & 7) << 15 |
		      (v[3] & 31) << 10 | (v[4] & 31) << 5 | (v[5] & 31);
	uint32_t w1 = (v[2] & 3) << 30 | (v[6] & 31) << 25 | (v[7] & 31) << 20 | (v[8] & 31) << 15 |
		      (v[9] & 31) << 10 | (v[10] & 31) << 5 | (v[11] & 31);
	return (uint64_t)w0 << 32 | w1;
}

static void cbfd_tri4(void)
{
	start(SALSA64_GBI_F3DEXBG);
	cbfd_setup();
	dl(0xDA380003, put_matrix((const float[16]){ 0.125f, 0, 0, 0, 0, 0.125f, 0, 0, 0, 0, 0.125f, 0, 0, 0, 0, 1 }));
	struct vtx v[32];
	memset(v, 0, sizeof v);
	for (int i = 0; i < 32; i++) {
		v[i].x = (int16_t)(i & 7);
		v[i].y = (int16_t)(i >> 3);
		v[i].c[0] = (uint8_t)(i * 8);
		v[i].c[3] = 255;
	}
	cbfd_vertex(put_vertices(v, 32), 32, 0);
	/* (29, 30, 22): el 29 lleva 0xE en el opcode y el 22 parte en w1;
	 * (0, 1, 8); uno vacío (3, 3, 3); (31, 23, 24). */
	const unsigned idx[12] = { 29, 30, 22, 0, 1, 8, 3, 3, 3, 31, 23, 24 };
	uint64_t w = cbfd_tri4_words(idx);
	CHECK_EQ((uint32_t)(w >> 56), 0x1E);
	dl((uint32_t)(w >> 32), (uint32_t)w);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 3);
	const unsigned want[9] = { 29, 30, 22, 0, 1, 8, 31, 23, 24 };
	for (int i = 0; i < 9; i++)
		CHECK_EQ(out_vtx[i].r, want[i] * 8);
}

/* El G_LOAD_UCODE de Conker no termina la lista: pasa a la iluminación
 * avanzada y la lista sigue hasta su G_RDPFULLSYNC. */
static void cbfd_load_ucode(void)
{
	start(SALSA64_GBI_F3DEXBG);
	cbfd_setup();
	struct vtx v[3];
	memset(v, 0, sizeof v);
	v[1].x = 1, v[2].y = 1;
	cbfd_vertex(put_vertices(v, 3), 3, 0);
	dl(0xE1000000, 0x00100000);                          /* G_RDPHALF_1: el código */
	dl(0xDD0007FF, 0x00110000);                          /* G_LOAD_UCODE */
	dl(0x05000204, 0);                                   /* G_TRI1 */
	dl(0xE9000000, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 1);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK(!(res.flags & GFX_RES_LOAD_UCODE));
	CHECK(gs.rare.advanced);

	/* En F3DEX2 el mismo comando carga otro microcódigo: su tramo se salta
	 * (gfx_dl_skip_foreign) y el triángulo no se dibuja. */
	start(SALSA64_GBI_F3DEX2);
	cbfd_vertex(put_vertices(v, 3), 3, 0);
	dl(0xDD0007FF, 0x00110000);
	dl(0x05000204, 0);
	dl(0xE9000000, 0);
	run();
	CHECK(res.flags & GFX_RES_LOAD_UCODE);
	CHECK_EQ(res.triangles, 0);
}

/* Luz de 48 bytes: color, dirección, intensidad de luz puntual (en
 * dieciseisavos) y posición. */
static uint32_t cbfd_light(unsigned col, int dx, int dy, int dz, unsigned ca, int px, int py, int pz)
{
	uint32_t a = data_alloc(48);
	uint8_t *p = t_n64.bus.rdram + a;
	memset(p, 0, 48);
	p[0] = p[1] = p[2] = (uint8_t)col;
	p[4] = p[5] = p[6] = (uint8_t)col;
	p[8] = (uint8_t)dx, p[9] = (uint8_t)dy, p[10] = (uint8_t)dz;
	p[12] = (uint8_t)ca;
	salsa64_wr16(p + 32, (uint16_t)px);
	salsa64_wr16(p + 34, (uint16_t)py);
	salsa64_wr16(p + 36, (uint16_t)pz);
	return a;
}

/* G_MV_LIGHT con el offset en (w0 >> 5) & 0x3FFF: lugar `slot` de 48 bytes. */
static void cbfd_movemem_light(unsigned slot, uint32_t addr)
{
	uint32_t off = slot * 48;
	dl(0xDC000000 | (48 / 8 - 1) << 19 | off << 5 | 10, addr);
}

/* G_MW_COORD_MOD: escala 1 y desplazamiento (ox, oy, oz). */
static void cbfd_coord_mod(int ox, int oy, int oz)
{
	dl(0xDB100000, 0x00010001);                          /* escala entera x, y */
	dl(0xDB100004, 0x00010000);                          /* z, w */
	dl(0xDB100008, 0xDEADBEEF);                          /* bit 3: se descarta */
	dl(0xDB100010, 0);                                   /* fracciones: escala 1 */
	dl(0xDB100014, 0);
	dl(0xDB100020, (uint32_t)(uint16_t)ox << 16 | (uint16_t)oy);
	dl(0xDB100024, (uint32_t)(uint16_t)oz << 16);
}

/* Luz básica: ambiental + luces puntuales (la direccional n−1 no cuenta);
 * multiplica el color del vértice; el bit 15 de las banderas la apaga. */
static void cbfd_lighting(void)
{
	start(SALSA64_GBI_F3DEXBG);
	cbfd_setup();
	dl(0xD9FFFFFF, G_LIGHTING);
	dl(0xDB020000, 2 * 48);                              /* G_MW_NUMLIGHT: dos luces */
	cbfd_coord_mod(256, 0, 0);
	/* Luz 0 puntual en el origen: el vértice en x = 0 queda a 256 (con el
	 * desplazamiento), 2·256²/65536 = 2 y ca = 16/16 → intensidad 0,5. */
	cbfd_movemem_light(2, cbfd_light(200, 0, 0, 0, 16, 0, 0, 0));
	cbfd_movemem_light(3, cbfd_light(255, 0, 0, 127, 0, 0, 0, 0));  /* direccional */
	cbfd_movemem_light(4, cbfd_light(50, 0, 0, 0, 0, 0, 0, 0));      /* ambiental */
	uint32_t normals = data_alloc(16);
	memset(t_n64.bus.rdram + normals, 0, 16);
	dl(0xDC000000 | (16 / 8 - 1) << 19 | 14, normals);   /* G_MV_NORMALS */
	struct vtx v[3];
	memset(v, 0, sizeof v);
	v[0].flags = 127, v[0].c[0] = 255, v[0].c[1] = 200, v[0].c[3] = 0x33;
	v[1].flags = 0x8000 | 127, v[1].c[0] = 77;            /* sin luz */
	v[2].x = 256, v[2].flags = 127, v[2].c[0] = 255;      /* a 512: intensidad 0,125 */
	uint32_t va = put_vertices(v, 3);
	cbfd_vertex(va, 3, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(gs.num_lights, 2);
	CHECK(fabsf(gs.rare.coord_mod[12] - 1.0f) < 1e-6f && fabsf(gs.rare.coord_mod[8] - 256.0f) < 1e-6f);
	/* (50 + 200·0,5)/255 = 150/255. */
	CHECK_EQ(gs.vtx[0].c[0], 150);
	CHECK_EQ(gs.vtx[0].c[1], 118);
	CHECK_EQ(gs.vtx[0].c[3], 0x33);
	CHECK_EQ(gs.vtx[1].c[0], 77);
	CHECK_EQ(gs.vtx[2].c[0], 75);  /* 50 + 200·0,125 */
}

/* Iluminación avanzada (tras G_LOAD_UCODE): la direccional n−1 por N·L con
 * la normal de la tabla (X e Y, 2 bytes por lugar del búfer) y Z del byte
 * 7 de las banderas; con G_POINT_LIGHTING también las puntuales por N·L. */
static void cbfd_lighting_advanced(void)
{
	start(SALSA64_GBI_F3DEXBG);
	cbfd_setup();
	dl(0xD9FFFFFF, G_LIGHTING);
	dl(0xDB020000, 2 * 48);
	cbfd_coord_mod(0, 0, 0);
	cbfd_movemem_light(2, cbfd_light(100, 127, 0, 0, 32, 0, 0, 0));  /* puntual, hacia +x */
	cbfd_movemem_light(3, cbfd_light(200, 0, 0, 127, 0, 0, 0, 0));  /* direccional +z */
	cbfd_movemem_light(4, cbfd_light(20, 0, 0, 0, 0, 0, 0, 0));
	uint32_t normals = data_alloc(16);
	uint8_t *n = t_n64.bus.rdram + normals;
	memset(n, 0, 16);
	n[2 * 5 + 0] = 127;                                  /* lugar 5: +x */
	dl(0xDC000000 | (16 / 8 - 1) << 19 | 14, normals);
	dl(0xDD0007FF, 0);                                   /* avanzada */
	struct vtx v[2];
	memset(v, 0, sizeof v);
	v[0].flags = 127, v[0].c[0] = v[0].c[1] = 255;        /* lugar 4: normal +z */
	v[1].c[0] = v[1].c[1] = 255;                          /* lugar 5: normal +x */
	uint32_t va = put_vertices(v, 2);
	cbfd_vertex(va, 2, 4);
	dl(0xD9FFFFFF, G_POINT_LIGHTING);
	cbfd_vertex(va, 2, 6);                                /* lugares 6 y 7: normal 0 */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	/* Vértice en el origen, luz puntual ahí: intensidad 1 sin
	 * G_POINT_LIGHTING. +z: 20 + 200 + 100; +x: 20 + 100. */
	CHECK_EQ(gs.vtx[4].c[0], 255);
	CHECK_EQ(gs.vtx[5].c[0], 120);
	/* Con G_POINT_LIGHTING y normal 0 (lugares 6 y 7 de la tabla, y el
	 * byte 7 del primero = 127 en z): solo la ambiental y la direccional. */
	CHECK_EQ(gs.vtx[6].c[0], 220);
	CHECK_EQ(gs.vtx[7].c[0], 20);
}

/* G_MOVEWORD y G_MOVEMEM propios: lookat X e Y en 0 y 48, segmentos, la
 * tabla de normales segmentada y la textura por lookat con su normal. */
static void cbfd_movemem_moveword(void)
{
	start(SALSA64_GBI_F3DEXBG);
	cbfd_setup();
	dl(0xDB060000 | 3 * 4, 0x00020000);                  /* segmento 3 */
	uint32_t lx = cbfd_light(0, 127, 0, 0, 0, 0, 0, 0), ly = cbfd_light(0, 0, 127, 0, 0, 0, 0, 0);
	dl(0xDC000000 | (16 / 8 - 1) << 19 | 0 << 5 | 10, 0x03000000 | (lx - 0x20000));
	dl(0xDC000000 | (16 / 8 - 1) << 19 | 48 << 5 | 10, 0x03000000 | (ly - 0x20000));
	uint32_t normals = data_alloc(16);
	uint8_t *n = t_n64.bus.rdram + normals;
	memset(n, 0, 16);
	n[0] = 127;                                          /* lugar 0: +x */
	dl(0xDC000000 | (16 / 8 - 1) << 19 | 14, 0x03000000 | (normals - 0x20000));
	dl(0xD7000002, 0x07C007C0);                          /* G_TEXTURE: tile 0, encendida */
	dl(0xD9FFFFFF, G_LIGHTING | G_TEXTURE_GEN);
	dl(0xDB020000, 0);                                   /* sin luces: solo la ambiental */
	struct vtx v[1];
	memset(v, 0, sizeof v);
	v[0].c[0] = 99;
	cbfd_vertex(put_vertices(v, 1), 1, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(gs.segments[3], 0x00020000);
	CHECK_EQ(gs.rare.normal_base, normals);
	CHECK_EQ(gs.num_lights, 0);
	CHECK(fabsf(gs.lookat[0][0] - 1.0f) < 1e-3f && fabsf(gs.lookat[1][1] - 1.0f) < 1e-3f);
	/* Normal +x: s al máximo (~31 texels), t en el medio (~15,5). */
	CHECK(fabsf(gs.vtx[0].s - 31.0f) < 0.05f);
	CHECK(fabsf(gs.vtx[0].t - 15.5f) < 0.05f);
	/* Ambiental en 0: el color del vértice se apaga. */
	CHECK_EQ(gs.vtx[0].c[0], 0);
}

/* Conker usa hasta 11 luces: la ambiental queda en el lugar 13. */
static void cbfd_many_lights(void)
{
	start(SALSA64_GBI_F3DEXBG);
	cbfd_setup();
	dl(0xD9FFFFFF, G_LIGHTING);
	dl(0xDB020000, 11 * 48);
	cbfd_coord_mod(0, 0, 0);
	/* Diez puntuales lejos (sin efecto) salvo la 9, en el origen con
	 * intensidad 1; la direccional (10) no cuenta en el modo básico. */
	for (unsigned l = 0; l < 10; l++)
		cbfd_movemem_light(2 + l, cbfd_light(30, 0, 0, 0, l == 9 ? 16 : 0, 0, 0, 0));
	cbfd_movemem_light(12, cbfd_light(255, 0, 0, 127, 0, 0, 0, 0));
	cbfd_movemem_light(13, cbfd_light(40, 0, 0, 0, 0, 0, 0, 0));
	uint32_t normals = data_alloc(16);
	memset(t_n64.bus.rdram + normals, 0, 16);
	dl(0xDC000000 | (16 / 8 - 1) << 19 | 14, normals);
	struct vtx v[1];
	memset(v, 0, sizeof v);
	v[0].c[0] = 255;
	cbfd_vertex(put_vertices(v, 1), 1, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(gs.num_lights, 11);
	CHECK_EQ(gs.vtx[0].c[0], 70);  /* 40 + 30 */
}

/* ---- Diddy Kong Racing (F3D de Rare con matrices, vértices y triángulos por DMA) ---- */

/* Vértice de 10 bytes: x, y, z (s16) y RGBA. */
struct dkr_vtx { int16_t x, y, z; uint8_t c[4]; };

static uint32_t dkr_put_vertices(const struct dkr_vtx *v, int n)
{
	uint32_t a = data_alloc(10 * (uint32_t)n);
	for (int i = 0; i < n; i++) {
		uint8_t *p = t_n64.bus.rdram + a + 10 * i;
		salsa64_wr16(p + 0, (uint16_t)v[i].x);
		salsa64_wr16(p + 2, (uint16_t)v[i].y);
		salsa64_wr16(p + 4, (uint16_t)v[i].z);
		memcpy(p + 6, v[i].c, 4);
	}
	return a;
}

/* Triángulo de 16 bytes: banderas, tres índices y (s, t) de cada esquina. */
static uint32_t dkr_put_triangle(unsigned flag, unsigned v0, unsigned v1, unsigned v2, const int16_t st[6])
{
	uint32_t a = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram + a;
	p[0] = (uint8_t)flag, p[1] = (uint8_t)v0, p[2] = (uint8_t)v1, p[3] = (uint8_t)v2;
	for (int i = 0; i < 6; i++)
		salsa64_wr16(p + 4 + 2 * i, (uint16_t)st[i]);
	return a;
}

/* Matriz que lleva un vértice (x, y, z) a x/128, y/128, z/128 con w = 1. */
static uint32_t dkr_matrix(float tx)
{
	float m[16] = { 0.0078125f, 0, 0, 0, 0, 0.0078125f, 0, 0, 0, 0, 0.0078125f, 0, tx, 0, 0, 1 };
	return put_matrix(m);
}

static void dkr_setup(void)
{
	dl(0x03800010, put_viewport_320x240());              /* G_MOVEMEM viewport */
	dl(0xFF100000 | 319, 0x00100000);                    /* color image */
	dl(0xFC000000 | (15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u),
	   7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 4u << 15 | 7u << 12 | 4u << 9 | 4u << 6 | 7u << 3 | 4u);
	dl(0xB6000000, 0xFFFFFFFF);                          /* G_CLEARGEOMETRYMODE */
	dl(0xB7000000, 0x00000004);                          /* SHADE */
}

/* Matriz por DMA a la ranura 0, vértices de 10 bytes y un triángulo con
 * las coordenadas de textura en él: posición, color, s y t, y el recorte
 * por caras que pide la bandera 0x40. */
static void dkr_vertices_triangles(void)
{
	start(SALSA64_GBI_F3DDKR);
	dkr_setup();
	dl(0x01000040, dkr_matrix(0.0f));                    /* G_MTX: ranura 0 (bits 22 y 23 en 0) */
	const struct dkr_vtx v[3] = {
		{ 0, 0, 0, { 10, 20, 30, 40 } }, { 128, 0, 0, { 50, 60, 70, 80 } }, { 0, 128, 0, { 90, 100, 110, 120 } },
	};
	dl(0x04000000 | (3 - 1) << 19, dkr_put_vertices(v, 3));        /* G_VTX: 3 vértices desde el 0 */
	const int16_t st[6] = { 32, 64, 0, 0, 64, 32 };
	dl(0x05000010 | 1 << 16, dkr_put_triangle(0, 0, 1, 2, st));    /* G_DMATRI: 1, con textura */
	dl(0xE9000000, 0);                                   /* G_RDPFULLSYNC */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.triangles, 1);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK(gs.tex_on);
	CHECK(gs.geom & G_CULL_BACK);
	CHECK(gs.geom & G_SHADING_SMOOTH);
	CHECK_EQ(gs.vtx[0].c[0], 10);
	CHECK_EQ(gs.vtx[2].c[3], 120);
	/* Pantalla: x = 160 + 160·x, y = 120 − 120·y (w = 1). */
	CHECK(out_vtx[0].x == 160.0f && out_vtx[1].x == 320.0f && out_vtx[2].x == 160.0f);
	CHECK(out_vtx[0].y == 120.0f && out_vtx[2].y == 0.0f);
	/* s y t en s10.5: 32 → 1 texel, tal cual (el G_TEXTURE de DKR no se usa). */
	CHECK(out_vtx[0].s == 1.0f && out_vtx[0].t == 2.0f);
	CHECK(out_vtx[1].s == 0.0f && out_vtx[1].t == 0.0f);
	CHECK(out_vtx[2].s == 2.0f && out_vtx[2].t == 1.0f);
	CHECK_EQ(out_vtx[1].r, 50);
	CHECK_EQ(out_vtx[2].a, 120);

	/* Con la bandera 0x40, sin recorte por caras, y la textura apagada. */
	start(SALSA64_GBI_F3DDKR);
	dkr_setup();
	dl(0x01000040, dkr_matrix(0.0f));
	dl(0x04000000 | (3 - 1) << 19, dkr_put_vertices(v, 3));
	dl(0x05000010, dkr_put_triangle(0x40, 0, 1, 2, st));
	run();
	CHECK_EQ(res.triangles, 1);
	CHECK(!gs.tex_on);
	CHECK(!(gs.geom & (G_CULL_BACK | G_CULL_FRONT)));
}

/* Bases de G_DMA_OFFSETS (matrices y vértices), la ranura elegida por G_MTX
 * (bits 22 y 23) y por G_MOVEWORD 0x0A, la multiplicación por la ranura 0
 * (Jet Force Gemini) y los vértices que siguen a los anteriores (bit 16). */
static void dkr_offsets_slots(void)
{
	start(SALSA64_GBI_F3DDKR);
	dkr_setup();
	uint32_t m0 = dkr_matrix(0.0f), m1 = dkr_matrix(0.5f);
	const struct dkr_vtx v[4] = {
		{ 0, 0, 0, { 1, 2, 3, 4 } }, { 128, 0, 0, { 5, 6, 7, 8 } }, { 0, 128, 0, { 9, 10, 11, 12 } },
		{ 128, 128, 0, { 13, 14, 15, 16 } },
	};
	uint32_t va = dkr_put_vertices(v, 4);
	/* Direcciones relativas a las bases (la matriz 1 y los vértices 2 y 3). */
	dl(0xBF000000 | m0, va);                             /* G_DMA_OFFSETS */
	dl(0x01000040, 0);                                   /* ranura 0 = m0 + 0 */
	dl(0x01400040, m1 - m0);                             /* ranura 1 (bit 22) */
	dl(0xBC00000A, 0 << 6);                              /* selecciona la ranura 0 */
	dl(0x04000000 | (2 - 1) << 19, 0);                   /* v0, v1 desde el 0 */
	dl(0xBC00000A, 1 << 6);                              /* la ranura 1 */
	dl(0x04010000 | (2 - 1) << 19, 2 * 10);              /* v2, v3: siguen a los anteriores (bit 16) */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK(fabsf(gs.vtx[0].x) < 1e-6f && fabsf(gs.vtx[1].x - 1.0f) < 1e-6f);
	CHECK_EQ(gs.vtx[1].c[0], 5);
	/* La ranura 1 traslada en x 0,5: v2 en (0,5; 1), v3 en (1,5; 1). */
	CHECK(fabsf(gs.vtx[2].x - 0.5f) < 1e-6f && fabsf(gs.vtx[2].y - 1.0f) < 1e-6f);
	CHECK(fabsf(gs.vtx[3].x - 1.5f) < 1e-6f);
	CHECK_EQ(gs.vtx[3].c[2], 15);
	CHECK_EQ(gs.rare.dkr.dma_mtx, m0);
	CHECK_EQ(gs.rare.dkr.dma_vtx, va);

	/* Jet Force Gemini: el conteo no suma 1 y el bit 23 multiplica por la ranura 0. */
	start(SALSA64_GBI_F3DJFG);
	dkr_setup();
	const float tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 64, 0, 0, 1 };
	uint32_t m2 = put_matrix(tr);
	dl(0x01000040, dkr_matrix(0.0f));
	dl(0x01810040, m2);                                  /* ranura 1: (x + 64) · ranura 0 */
	dl(0x04000000 | 2 << 19, dkr_put_vertices(v, 4));    /* 2 vértices (no 3) */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(gs.rare.dkr.cur_mtx, 1u);
	CHECK(fabsf(gs.vtx[0].x - 0.5f) < 1e-6f && fabsf(gs.vtx[1].x - 1.5f) < 1e-6f);
	CHECK(gs.vtx[2].x == 0.0f && gs.vtx[2].w == 0.0f);   /* el tercero no se cargó */
}

/* G_DL_IN_MEM: n comandos y de vuelta, sin G_ENDDL (el siguiente de la
 * lista llamada no se ejecuta). */
static void dkr_counted_list(void)
{
	start(SALSA64_GBI_F3DDKR);
	dkr_setup();
	uint32_t list = data_alloc(32);
	t_wr32(list, 0xB7000000), t_wr32(list + 4, 0x00010000);       /* G_FOG */
	t_wr32(list + 8, 0xB7000000), t_wr32(list + 12, 0x00020000);  /* G_LIGHTING */
	t_wr32(list + 16, 0xB7000000), t_wr32(list + 20, 0x00040000); /* G_TEXTURE_GEN: no */
	dl(0x07000000 | 2 << 16 | 2 * 8, list);
	dl(0xB7000000, 0x00080000);                          /* el que sigue en la lista principal */
	dl(0xE9000000, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK(gs.geom & G_FOG);
	CHECK(gs.geom & G_LIGHTING);
	CHECK(!(gs.geom & G_TEXTURE_GEN));
	CHECK(gs.geom & G_TEXTURE_GEN_LINEAR);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK_EQ(gs.dl_sp, 0);
}

/* Carteles: con G_MOVEWORD 0x02 los vértices que siguen (bit 16) van desde
 * el lugar 1 y se suman al 0, ya transformado. */
static void dkr_billboard(void)
{
	start(SALSA64_GBI_F3DDKR);
	dkr_setup();
	dl(0x01000040, dkr_matrix(0.0f));
	const struct dkr_vtx c = { 256, 128, 0, { 1, 1, 1, 1 } };
	dl(0x04000000, dkr_put_vertices(&c, 1));             /* el centro, en el lugar 0 */
	dl(0xBC000002, 1);                                   /* carteles */
	const struct dkr_vtx q[2] = { { 0, 0, 0, { 2, 2, 2, 2 } }, { 128, 0, 0, { 3, 3, 3, 3 } } };
	dl(0x04010000 | (2 - 1) << 19, dkr_put_vertices(q, 2));   /* desde el lugar 1 */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK(fabsf(gs.vtx[0].x - 2.0f) < 1e-6f && fabsf(gs.vtx[0].y - 1.0f) < 1e-6f);
	CHECK(fabsf(gs.vtx[1].x - 2.0f) < 1e-6f && fabsf(gs.vtx[1].y - 1.0f) < 1e-6f);   /* 0 + el centro */
	CHECK(fabsf(gs.vtx[2].x - 3.0f) < 1e-6f && fabsf(gs.vtx[2].y - 1.0f) < 1e-6f);
	CHECK_EQ(gs.vtx[1].c[0], 2);
	CHECK_EQ(gs.vtx[2].c[0], 3);
	CHECK_EQ(gs.rare.dkr.billboard, 1u);
}

/* G_DMA_TEX_OFFSET (0x02, Jet Force Gemini y Mickey's Speedway): una tabla
 * de 32 desplazamientos en bytes que cada G_SETTIMG siguiente suma a su
 * dirección, de cualquier formato, uno tras otro; G_DMA_OFFSETS apaga el
 * interruptor y otro 0x02 vuelve a empezar la tabla. */
static uint32_t dkr_put_texoffsets(const uint16_t *t, unsigned n)
{
	uint32_t a = data_alloc(64);
	for (unsigned i = 0; i < 32; i++)
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, i < n ? t[i] : 0);
	return a;
}

/* n_set G_SETTIMG (el segundo en IA, los demás en RGBA) tras el 0x02; con
 * `again`, además un G_DMA_OFFSETS y otro G_SETTIMG. Devuelve la dirección
 * física que deja el último. */
static uint32_t dkr_timg_after(unsigned n_set, bool again)
{
	start(SALSA64_GBI_F3DJFG);
	const uint16_t t[3] = { 0x0B00, 0x1600, 0x2100 };
	uint32_t tab = dkr_put_texoffsets(t, 3);
	dl(0xBC000806, 0x00030000);                         /* segmento 2 = 0x30000 */
	dl(0xBF000000, 0);
	dl(0x02000050, tab);
	for (unsigned i = 0; i < n_set; i++)
		dl(0xFD000000 | (i == 1 ? 3u << 21 | 2u << 19 : 2u << 19) | 31, 0x02001000 + 0x100 * i);
	if (again) {
		dl(0xBF000000, 0);
		dl(0xFD000000 | 2u << 19 | 31, 0x02002000);       /* sin desplazamiento */
	}
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	return gs.timg_addr;
}

static void dkr_tex_offsets(void)
{
	/* El primero suma la entrada 0, el segundo (IA) la 1 y el tercero la 2. */
	CHECK_EQ(dkr_timg_after(1, false), 0x30000u + 0x1000 + 0x0B00);
	CHECK_EQ(dkr_timg_after(2, false), 0x30000u + 0x1100 + 0x1600);
	CHECK_EQ(dkr_timg_after(3, false), 0x30000u + 0x1200 + 0x2100);
	/* Más allá de las entradas dadas no suma nada. */
	CHECK_EQ(dkr_timg_after(4, false), 0x30000u + 0x1300);
	/* G_DMA_OFFSETS apaga el interruptor. */
	CHECK_EQ(dkr_timg_after(1, true), 0x30000u + 0x2000);

	/* Otro 0x02 reinicia el índice: el primero de la segunda tabla suma su entrada 0. */
	start(SALSA64_GBI_F3DJFG);
	const uint16_t t1[2] = { 0x0010, 0x0020 }, t2[2] = { 0x0300, 0x0400 };
	uint32_t a1 = dkr_put_texoffsets(t1, 2), a2 = dkr_put_texoffsets(t2, 2);
	dl(0x02000050, a1);
	dl(0xFD100000 | 31, 0x00005000);                     /* + 0x10 */
	dl(0xFD100000 | 31, 0x00005000);                     /* + 0x20 */
	dl(0x02000050, a2);
	dl(0xFD100000 | 31, 0x00006000);                     /* + 0x300 */
	run();
	CHECK_EQ(gs.timg_addr, 0x6300u);

	/* La tabla tiene 32 entradas: la 33.ª no existe (sin desplazamiento). */
	start(SALSA64_GBI_F3DJFG);
	uint16_t full[32];
	for (unsigned i = 0; i < 32; i++)
		full[i] = 0x10;
	uint32_t af = dkr_put_texoffsets(full, 32);
	dl(0x02000050, af);
	for (unsigned i = 0; i < 33; i++)
		dl(0xFD100000 | 31, 0x00008000);
	run();
	CHECK_EQ(gs.timg_addr, 0x8000u);
	CHECK_EQ(gs.rare.dkr.tex_i, 33u);

	/* Sin 0x02 no cambia nada. */
	start(SALSA64_GBI_F3DJFG);
	dl(0xFD100000 | 31, 0x00009000);
	run();
	CHECK_EQ(gs.timg_addr, 0x9000u);
}

void t_suite_gfx_rare(void)
{
	RUN(dkr_vertices_triangles);
	RUN(dkr_offsets_slots);
	RUN(dkr_counted_list);
	RUN(dkr_billboard);
	RUN(dkr_tex_offsets);
	RUN(pd_vertex_colors);
	RUN(pd_lighting);
	RUN(pd_tri4_quad);
	RUN(cbfd_tri4);
	RUN(cbfd_load_ucode);
	RUN(cbfd_lighting);
	RUN(cbfd_lighting_advanced);
	RUN(cbfd_movemem_moveword);
	RUN(cbfd_many_lights);
}
