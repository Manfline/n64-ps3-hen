/*
 * Salsa64 — pruebas de la HLE del microcódigo gráfico de Rogue Squadron
 * (src/rsp/gfx/f5rogue.c, de Factor 5): listas de longitud variable con sus
 * datos dentro, vértices de 8 bytes, triángulos con colores por índice y
 * coordenadas de textura, las mallas generadas (G_TRIGEN), el rectángulo de
 * textura que genera desde un vértice y los saltos de lista. Display lists
 * sintéticas con la codificación que se midió contra el microcódigo real
 * (salsa64-run --lle=all); nada viene del juego: vértices, colores y
 * alturas son inventados.
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
	gfx_init(&gs);
	/* La palabra de salto de software con que empieza cada lista. */
	dl(0x00000000, 0);
}

/* G_ENDDL y la tarea. */
static void run(void)
{
	dl(0xB8000000, 0);
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	job.gbi = SALSA64_GBI_F5ROGUE;
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

/* La proyección que lleva [−128, 128] a [−1, 1]. */
static const float proj[16] = { 1.0f / 128, 0, 0, 0, 0, 1.0f / 128, 0, 0, 0, 0, 1.0f / 128, 0, 0, 0, 0, 1 };
static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/* Viewport de 320 × 240 y matrices, y la tabla de colores: lo que arma el juego en
 * cada cuadro. G_MOVEMEM 0x80 trae el viewport en los 16 bytes que siguen. */
static uint32_t color_table(void)
{
	uint32_t a = data_alloc(64);
	for (int i = 0; i < 16; i++)
		t_wr32(a + 4 * i, 0x10203000u + 0x01010100u * (uint32_t)i + 0xF0u + (uint32_t)i);
	return a;
}

static void setup(void)
{
	static const int16_t vp[8] = { 640, 480, 511, 0, 640, 480, 511, 0 };
	dl(0x03800000, 0);
	for (int i = 0; i < 8; i += 4)
		dl((uint32_t)(uint16_t)vp[i] << 16 | (uint16_t)vp[i + 1], (uint32_t)(uint16_t)vp[i + 2] << 16 | (uint16_t)vp[i + 3]);
	dl(0x01030040, put_matrix(proj));  /* proyección, carga */
	dl(0x01020040, put_matrix(ident)); /* modelview, carga */
	dl(0xFF100000 | 319, 0x00100000);  /* color image */
	/* Combiner SHADE en los dos ciclos. */
	dl(0xFC000000 | (15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u),
	   7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 4u << 15 | 7u << 12 | 4u << 9 | 4u << 6 | 7u << 3 | 4u);
}

/* x, y, z de 16 bits y una palabra que el microcódigo no usa. */
static uint32_t put_vertices(const int16_t (*v)[3], int n)
{
	uint32_t a = data_alloc(8 * (uint32_t)n);
	for (int i = 0; i < n; i++) {
		uint8_t *p = t_n64.bus.rdram + a + 8 * i;
		for (int k = 0; k < 3; k++)
			salsa64_wr16(p + 2 * k, (uint16_t)v[i][k]);
		salsa64_wr16(p + 6, 0x1234);
	}
	return a;
}

/* ---- Listas ---- */

/* Cada lista empieza con una palabra de 8 bytes (cuyos 24 bits bajos dicen
 * a dónde salta el 0xB5); G_DL y G_BRANCHDL la saltan, el 0xB5 salta a ese
 * destino (y a su vez salta su primera palabra). */
static void f5_lists(void)
{
	start();
	uint32_t l2 = data_alloc(32), l1 = data_alloc(32);
	/* L1: salto a L2, G_FOG, 0xB5. L2: G_LIGHTING y G_ENDDL (vuelve a la lista principal). */
	t_wr32(l1, l2), t_wr32(l1 + 4, 0);
	t_wr32(l1 + 8, 0xB7000000), t_wr32(l1 + 12, 0x00010000);
	t_wr32(l1 + 16, 0xB5000000), t_wr32(l1 + 20, 0);
	t_wr32(l1 + 24, 0xB7000000), t_wr32(l1 + 28, 0x00000008);  /* no se ejecuta */
	t_wr32(l2, 0x00ABCDEF), t_wr32(l2 + 4, 0);
	t_wr32(l2 + 8, 0xB7000000), t_wr32(l2 + 12, 0x00020000);
	t_wr32(l2 + 16, 0xB8000000), t_wr32(l2 + 20, 0);
	dl(0x06000000, l1);
	dl(0xB7000000, 0x00040000);  /* G_TEXTURE_GEN, ya en la principal */
	dl(0xE9000000, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	/* F3D: G_FOG 0x10, G_LIGHTING 0x20 y G_TEXTURE_GEN 0x200 en su GBI. */
	CHECK(gs.geom & G_FOG);
	CHECK(gs.geom & G_LIGHTING);
	CHECK(gs.geom & G_TEXTURE_GEN);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK_EQ(gs.dl_sp, 0);

	/* G_BRANCHDL: no deja de seguir en el mismo nivel y su cabecera también se salta. */
	start();
	uint32_t b = data_alloc(32);
	t_wr32(b, 0), t_wr32(b + 4, 0);
	t_wr32(b + 8, 0xB7000000), t_wr32(b + 12, 0x00010000);
	t_wr32(b + 16, 0xB8000000), t_wr32(b + 20, 0);
	dl(0x07000000, b);
	dl(0xB7000000, 0x00020000);  /* no se ejecuta: G_BRANCHDL no vuelve */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK(gs.geom & G_FOG);
	CHECK(!(gs.geom & G_LIGHTING));
}

/* G_MOVEWORD: segmentos y niebla (16.16 en vez de los dos 16 bits de Fast3D), y el
 * recorte. */
static void f5_moveword(void)
{
	start();
	dl(0xBC000006u | 1u << 10, 0x00030000);  /* segmento 1 en 0x30000 */
	dl(0xBC000008, 0x00280000);              /* multiplicador de la niebla: 0x280000/65536 = 40 en 16.16 */
	dl(0xBC00000A, 0xFFFF0000u);             /* desplazamiento: -1 */
	uint32_t sub = data_alloc(16);
	(void)sub;
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(gs.segments[1], 0x30000u);
	CHECK(fabsf(gs.fog_mul - 40.0f * 256.0f) < 1e-3f);
	CHECK(fabsf(gs.fog_off + 256.0f) < 1e-3f);
}

/* G_SETOTHERMODE_L_EX y _H_EX: la palabra de 8 bytes que sigue es la máscara que se
 * conserva; los bits nuevos van en w1. */
static void f5_othermode_ex(void)
{
	start();
	dl(0xBE000000, 0x00004000);                 /* H: conserva lo de la máscara y añade el bit 14 */
	dl(0xFFFFFFFFu, 0xFFFFFFFFu);
	dl(0xB3000000, 0x00000013);                 /* L: máscara 0x00FF y bits 0x13 */
	dl(0x000000FFu, 0x00000000);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK((gs.oh & 0x4000u) != 0);
	CHECK_EQ(gs.ol & 0xFFu, 0x13u);

	/* Con otra máscara se borra lo que ya había. */
	start();
	dl(0xBA000000 | 12u << 8 | 4u, 0x0000F000);  /* F3D G_SETOTHERMODE_H: filtro de textura y compañía */
	dl(0xBE000000, 0x00000100);
	dl(0xFFFF0FFFu, 0);
	run();
	CHECK_EQ(gs.oh & 0xFFFFu, 0x0100u);
}

/* ---- Vértices y triángulos ---- */

/* G_VTX: n vértices de 8 bytes desde el lugar 0. La y de pantalla sale al
 * revés que en Fast3D. */
static void f5_vertices(void)
{
	start();
	setup();
	const int16_t v[3][3] = { { 0, 0, 0 }, { 64, 0, 0 }, { 0, 64, 0 } };
	dl(0x04000000 | 3u << 10, put_vertices(v, 3));
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK(fabsf(gs.vscale[0] - 160.0f) < 1e-3f && fabsf(gs.vtrans[1] - 120.0f) < 1e-3f);
	/* x = 64 / 128 = 0,5 → 160 + 0,5 · 160; y = 64 / 128 → 120 + 0,5 · 120 (no 120 − 60). */
	CHECK(fabsf(gs.vtx[1].sx - 240.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[1].sy - 120.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[2].sx - 160.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[2].sy - 180.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[2].y - (-0.5f)) < 1e-6f);  /* y negada: la que se usa de aquí en adelante */
	CHECK_EQ(gs.vtx[2].clip, 0u);

	/* Un vértice fuera por arriba (y de pantalla < 0) en la y del microcódigo. */
	start();
	setup();
	const int16_t w[1][3] = { { 0, 200, 0 } };
	dl(0x04000000 | 1u << 10, put_vertices(w, 1));
	run();
	CHECK(gs.vtx[0].clip & GFX_CLIP_YN);
	CHECK(!(gs.vtx[0].clip & GFX_CLIP_YP));
}

/* G_TRI1 con colores por índice y coordenadas de textura (bit 1 de w0):
 * los 16 bytes que siguen son las coordenadas (una palabra s, t por vértice) y se
 * escalan con G_MOVEMEM 0x82: la mitad alta del producto con la escala de 0 más la
 * baja del producto con la de 1; en 1/32 de texel con perspectiva y en 1/16 sin ella. */
static void f5_tri_colors_tex(void)
{
	for (int persp = 1; persp >= 0; persp--) {
		start();
		setup();
		uint32_t tab = color_table();
		dl(0x02000000, tab);
		const int16_t v[3][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 0, 64, 0 } };
		dl(0x04000000 | 3u << 10, put_vertices(v, 3));
		dl(0x03820000, 0);        /* escala: la palabra de +12 (la de 1) y la de +16 (la de 0) */
		dl(0, 0x00000000);
		dl(0x7E007E00, 0);
		dl(0xBA000000 | 19u << 8 | 1u, persp ? 1u << 19 : 0u);
		dl(0xB7000000, 0x00000200);  /* sombreado suave */
		dl(0xBF000002, 5 * 0 << 16 | 5 * 1 << 8 | 5 * 2);
		dl(0u << 16 | 4u << 8 | 8u, 0);    /* las entradas 0, 1 y 2 de la tabla */
		dl(0x10000000, 0x00001000);        /* (s, t) de v1 y v2 */
		dl(0x08000800, 0);                 /* v3 */
		run();
		CHECK_EQ(res.unknown_cmds, 0);
		CHECK_EQ(res.dl_errors, 0);
		CHECK_EQ(res.triangles, 1);
		float k = persp ? 32.0f : 16.0f;
		/* 0x1000 · 0x7E00 >> 16 = 2016 y 0x800 · 0x7E00 >> 16 = 1008. */
		CHECK(fabsf(gs.vtx[0].s - 2016.0f / k) < 1e-3f && fabsf(gs.vtx[0].t) < 1e-3f);
		CHECK(fabsf(gs.vtx[1].s) < 1e-3f && fabsf(gs.vtx[1].t - 2016.0f / k) < 1e-3f);
		CHECK(fabsf(gs.vtx[2].s - 1008.0f / k) < 1e-3f && fabsf(gs.vtx[2].t - 1008.0f / k) < 1e-3f);
		for (int i = 0; i < 3; i++) {
			uint32_t c = t_rd32(tab + 4 * (uint32_t)i);
			CHECK_EQ(gs.vtx[i].c[0], c >> 24);
			CHECK_EQ(gs.vtx[i].c[1], (c >> 16) & 0xFF);
			CHECK_EQ(gs.vtx[i].c[2], (c >> 8) & 0xFF);
			CHECK_EQ(gs.vtx[i].c[3], c & 0xFF);
		}
	}

	/* Una segunda escala (la de 1) suma el producto bajo con la alta. */
	start();
	setup();
	dl(0x02000000, color_table());
	const int16_t v[3][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 0, 64, 0 } };
	dl(0x04000000 | 3u << 10, put_vertices(v, 3));
	dl(0x03820000, 0);
	dl(0, 0x00040004);   /* la escala de 1: s · 4 y t · 4 sin acarreo */
	dl(0x00000000, 0);
	dl(0xBA000000 | 19u << 8 | 1u, 1u << 19);
	dl(0xBF000002, 5 * 0 << 16 | 5 * 1 << 8 | 5 * 2);
	dl(0u << 16 | 4u << 8 | 8u, 0);
	dl(0x00100000, 0x00000000);
	dl(0x00000000, 0);
	run();
	CHECK(fabsf(gs.vtx[0].s - (float)(0x10 * 4) / 32.0f) < 1e-3f);
}

/* Los colores salen de la tabla por el byte de índice (de 4 en 4) y el sombreado
 * plano toma el del vértice que dice el byte 3 de w1. */
static void f5_tri_flat(void)
{
	start();
	setup();
	uint32_t tab = color_table();
	dl(0x02000000, tab);
	const int16_t v[3][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 0, 64, 0 } };
	dl(0x04000000 | 3u << 10, put_vertices(v, 3));
	/* Sin sombreado suave: el color plano es el del vértice de índice `flag`. */
	for (unsigned flag = 0; flag < 3; flag++) {
		dl(0xBF000000, flag << 24 | 5 * 0 << 16 | 5 * 1 << 8 | 5 * 2);
		dl(0u << 16 | 3u * 4 << 8 | 7u * 4, 0);  /* v1: entrada 0; v2: 3; v3: 7 */
	}
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 3);
	/* Cada triángulo con un color (el de su vértice de índice `flag`): entradas 0, 3 y 7
	 * de la tabla (el rojo cambia de una a otra). */
	const uint8_t want[3] = { (uint8_t)(t_rd32(tab + 0) >> 24), (uint8_t)(t_rd32(tab + 4 * 3) >> 24),
				  (uint8_t)(t_rd32(tab + 4 * 7) >> 24) };
	for (int k = 0; k < 3; k++)
		for (int i = 0; i < 3; i++)
			CHECK_EQ(out_vtx[3 * k + i].r, want[k]);
	CHECK(want[0] != want[1] && want[1] != want[2]);
}

/* G_TRI2: (v1, v2, v3) y (v1, v3, v4), los cuatro índices en los cuatro bytes de w1. */
static void f5_tri2(void)
{
	start();
	setup();
	dl(0x02000000, color_table());
	const int16_t v[4][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 64, 64, 0 }, { -64, 64, 0 } };
	dl(0x04000000 | 4u << 10, put_vertices(v, 4));
	dl(0xB4000000, 3u * 5 << 24 | 0u << 16 | 5u << 8 | 10u);   /* v4 en el byte 3; v1, v2, v3 en 2, 1, 0: 0, 1, 2 */
	dl(0u << 16 | 1u * 4 << 8 | 2u * 4, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(gs.dl_sp, 0);
}

/* ---- G_TRIGEN ---- */

static void trigen(uint32_t w0, uint32_t p[9])
{
	dl(w0, p[0]);
	dl(p[1], p[2]);
	dl(p[3], p[4]);
	dl(p[5], p[6]);
	dl(p[7], p[8]);
}

/* La rejilla de 3 × 3: x, z crecen por filas y columnas desde la esquina, y la altura es
 * el byte con signo ×16; los 9 vértices, 8 triángulos. */
static void f5_trigen_3x3(void)
{
	start();
	setup();
	/* 3 bytes de altura (a +0, +2 y +4) cada 10: 16 entradas. */
	uint32_t hgt = data_alloc(160), col = data_alloc(160);
	for (int i = 0; i < 160; i++)
		t_n64.bus.rdram[hgt + i] = 0;
	for (int i = 0; i < 40; i++)
		t_wr32(col + 4 * i, 0x40000000u + (uint32_t)i * 0x01010100u + 0xFF);
	/* Las alturas: 1, 2, 3 en la primera fila, 4, 5, 6 en la segunda… (bytes en +0, +2, +4, +10…). */
	uint32_t idx = 0;
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 3; c++, idx++)
			t_n64.bus.rdram[hgt + 10 * r + 2 * c] = (uint8_t)(idx + 1);
	uint32_t p[9] = { 0, hgt, col, 0x00700070u, 0, 0, 0x00000020u, (uint32_t)(uint16_t)-48 << 16 | 0, 48 };
	trigen(0x05000001, p);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.triangles, 8);
	/* El vértice col·3 + fila: x = −48 + col · 48, z = fila · 48 (z no se ve en x, y);
	 * y = altura · 16 (en cuartos de ... unidades del modelo). */
	CHECK(fabsf(gs.vtx[0].x - (-48.0f / 128.0f)) < 1e-6f);
	CHECK(fabsf(gs.vtx[3].x - 0.0f) < 1e-6f);
	CHECK(fabsf(gs.vtx[6].x - 48.0f / 128.0f) < 1e-6f);
	/* El vértice 0 es la fila 0 de la columna 0: altura = byte 0 · 16 = 16 → y de clip −16/128 tras negarla. */
	CHECK(fabsf(gs.vtx[0].y - (-16.0f / 128.0f)) < 1e-6f);
	/* El color de cada vértice, de la lista (el de la fila × 3 + columna). */
	CHECK_EQ(gs.vtx[0].c[3], 0xFF);
	/* Los colores van de tres en tres, cada 10 palabras: el 4 (columna 1, fila 1) es la palabra 12. */
	CHECK_EQ(gs.vtx[4].c[0], (uint8_t)(t_rd32(col + 4 * 12) >> 24));
}

/* La rejilla de 5 × 5 (25 alturas seguidas, 32 triángulos) con su opción de interpolar los
 * vértices 1 y 3 de la primera fila. */
static void f5_trigen_5x5(void)
{
	start();
	setup();
	uint32_t hgt = data_alloc(32), col = data_alloc(128);
	for (int i = 0; i < 25; i++)
		t_n64.bus.rdram[hgt + i] = (uint8_t)(i & 3);
	for (int i = 0; i < 25; i++)
		t_wr32(col + 4 * i, 0x20000000u + (uint32_t)i * 0x01000000u + 0xFF);
	uint32_t p[9] = { 0, hgt, col, 0x00700070u, 0, 0, 0x00000010u, (uint32_t)(uint16_t)-64 << 16 | 0, 32 };
	trigen(0x05000000, p);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 32);

	/* La opción 0x00000100 (p1): los vértices impares de la columna 0 son la media de los pares
	 * y salen dos triángulos más. */
	start();
	setup();
	for (int i = 0; i < 25; i++)
		t_n64.bus.rdram[hgt + i] = (uint8_t)(i & 3);
	uint32_t q[9] = { 0x00000100u, hgt, col, 0x00700070u, 0, 0, 0x00000010u, (uint32_t)(uint16_t)-64 << 16 | 0, 32 };
	trigen(0x05000000, q);
	run();
	CHECK_EQ(res.triangles, 34);
	/* Alturas naturales 0 y 2: i & 3 = 0 y 2; la 1 pasa a ser su media (1 · 16): en la columna 0
	 * los vértices son los de las filas 0 a 4 (fila j → natural j · 5). Se mide en la
	 * del vértice 1 de la primera fila (natural 1 ↔ columna 1, fila 0: vértice 5). */
	CHECK(fabsf(gs.vtx[5].y - (-16.0f / 128.0f)) < 1e-6f);
}

/* El cuadrilátero (modo 2): cuatro alturas y cuatro colores en la propia lista y dos
 * triángulos. */
static void f5_trigen_quad(void)
{
	start();
	setup();
	uint32_t p[9] = { 0x00100020u, 0x00300040u, 0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF11u,
			  0x00000020u, (uint32_t)(uint16_t)-48 << 16 | 0, 48 };
	trigen(0x05000200, p);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(gs.vtx[0].c[0], 0x11);
	CHECK_EQ(gs.vtx[1].c[0], 0x55);
	CHECK_EQ(gs.vtx[2].c[0], 0x99);
	CHECK_EQ(gs.vtx[3].c[0], 0xDD);
	CHECK_EQ(gs.dl_sp, 0);
}

/* ---- Rectángulos ---- */

/* G_TEXRECT: 16 bytes (las palabras de s y t y de dsdx y dtdy siguen a las dos primeras
 * sin su opcode). */
static void f5_texrect(void)
{
	start();
	setup();
	dl(0xB7000000, 0x00000004);  /* un cambio de estado que sigue al rectángulo */
	/* Copia de un solo ciclo: un rectángulo de 20 × 10 en (30, 40). */
	dl(0xBA000000 | 20u << 8 | 2u, 0u << 20);
	dl(0xE4000000 | (50u << 2) << 12 | 50u << 2, 0u << 24 | (30u << 2) << 12 | 40u << 2);
	dl(0x00000000, 0x04000400u);
	dl(0xB7000000, 0x00010000);  /* G_FOG: debe ejecutarse tras el rectángulo */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.rects, 1);
	CHECK(gs.geom & G_FOG);
	CHECK_EQ(gs.dl_sp, 0);
}

/* Rectángulo generado desde un vértice: el tamaño sale del viewport, la proyección y la
 * w del vértice; con w = 1 y la proyección de 1/128: p3 · 320 · 512 / 1 >> 16 cuartos de
 * píxel de medio lado. */
static void f5_texrect_gen(void)
{
	start();
	setup();
	const int16_t v[1][3] = { { 0, 0, 0 } };
	dl(0x04000000 | 1u << 10, put_vertices(v, 1));
	dl(0xBA000000 | 20u << 8 | 2u, 0u << 20);
	/* Un ciclo, textura 0. p3 = 0x00200020 (media talla 32), p4: dsdx 1, dtdy 1 en 5.10. */
	dl(0xBD000000, 0xFF8040FFu);
	dl(0, 0x00200020u);
	dl(0x04000400u, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.rects, 1);
	/* Medio lado: 32 · 320 · 512 >> 16 = 80 cuartos = 20 píxeles en x y 32 · 240 · 512 >> 16 = 60 = 15 en y;
	 * centro (160, 120). */
	float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
	for (int i = 0; i < 6; i++) {
		if (out_vtx[i].x < xmin) xmin = out_vtx[i].x;
		if (out_vtx[i].x > xmax) xmax = out_vtx[i].x;
		if (out_vtx[i].y < ymin) ymin = out_vtx[i].y;
		if (out_vtx[i].y > ymax) ymax = out_vtx[i].y;
	}
	CHECK(fabsf(xmin - 140.0f) < 1.01f && fabsf(xmax - 180.0f) < 1.01f);
	CHECK(fabsf(ymin - 105.0f) < 1.01f && fabsf(ymax - 135.0f) < 1.01f);
	CHECK_EQ(gs.prim, 0xFF8040FFu);

	/* Un vértice fuera de la pantalla no genera nada. */
	start();
	setup();
	const int16_t w[1][3] = { { 0, 0, 120 + 0 } };
	(void)w;
	const int16_t far[1][3] = { { 1000, 0, 0 } };
	dl(0x04000000 | 1u << 10, put_vertices(far, 1));
	dl(0xBD000000, 0xFF8040FFu);
	dl(0, 0x00200020u);
	dl(0x04000400u, 0);
	run();
	CHECK_EQ(res.rects, 0);
}

void t_suite_gfx_f5(void)
{
	RUN(f5_lists);
	RUN(f5_moveword);
	RUN(f5_othermode_ex);
	RUN(f5_vertices);
	RUN(f5_tri_colors_tex);
	RUN(f5_tri_flat);
	RUN(f5_tri2);
	RUN(f5_trigen_3x3);
	RUN(f5_trigen_5x5);
	RUN(f5_trigen_quad);
	RUN(f5_texrect);
	RUN(f5_texrect_gen);
}
