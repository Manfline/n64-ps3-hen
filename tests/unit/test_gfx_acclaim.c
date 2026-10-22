/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas sintéticas de la HLE F3DEX2ACCLAIM. Los datos de
 * display list, luces y vértices son inventados; no proceden de juegos.
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define DL_PA   0x10000u
#define DATA_PA 0x20000u

static struct gfx_state gs;
static struct gfx_vertex out_vtx[64] __attribute__((aligned(16)));
static uint8_t out_cmd[4096] __attribute__((aligned(16)));
static uint8_t out_tex[4096] __attribute__((aligned(128)));
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
	memset(t_n64.bus.rdram + a, 0, (bytes + 15) & ~15u);
	return a;
}

static void start(enum salsa64_gbi kind)
{
	gbi = kind;
	dl_pc = DL_PA;
	data_pc = DATA_PA;
	gfx_init(&gs);
}

static void run(void)
{
	dl(0xDF000000u, 0);
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
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
}

static uint32_t put_light24(uint8_t r, uint8_t gr, uint8_t b, int8_t x, int8_t y, int8_t z)
{
	uint32_t a = data_alloc(24);
	uint8_t *p = t_n64.bus.rdram + a;
	p[0] = r; p[1] = gr; p[2] = b;
	p[8] = (uint8_t)x; p[9] = (uint8_t)y; p[10] = (uint8_t)z;
	return a;
}

static uint32_t put_light16(int16_t x, int16_t y, int16_t z, uint8_t r, uint8_t gr, uint8_t b,
			    int16_t ca, uint16_t la, uint16_t qa)
{
	uint32_t a = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram + a;
	salsa64_wr16(p + 0, (uint16_t)x);
	salsa64_wr16(p + 2, (uint16_t)y);
	salsa64_wr16(p + 4, (uint16_t)z);
	p[6] = r; p[7] = gr; p[8] = b;
	salsa64_wr16(p + 10, (uint16_t)ca);
	salsa64_wr16(p + 12, la);
	salsa64_wr16(p + 14, qa);
	return a;
}

static void movemem_light(uint32_t offset, uint32_t len, uint32_t addr)
{
	dl(0xDC00000Au | (offset << 5) | ((len / 8 - 1) << 19), addr);
}

static uint32_t put_vertex(int16_t x, int16_t y, int16_t z, uint8_t r, uint8_t gr, uint8_t b, uint8_t a)
{
	uint32_t addr = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram + addr;
	salsa64_wr16(p + 0, (uint16_t)x);
	salsa64_wr16(p + 2, (uint16_t)y);
	salsa64_wr16(p + 4, (uint16_t)z);
	p[12] = r; p[13] = gr; p[14] = b; p[15] = a;
	return addr;
}

static void vertex(uint32_t addr, unsigned n, unsigned v0)
{
	dl(0x01000000u | n << 12 | (v0 + n) << 1, addr);
}

static void acclaim_light_formats(void)
{
	start(SALSA64_GBI_F3DEX2ACCLAIM);
	movemem_light(0, 24, put_light24(1, 2, 3, 127, 0, 0));
	movemem_light(24, 24, put_light24(4, 5, 6, 0, -127, 0));
	movemem_light(48, 24, put_light24(11, 22, 33, 0, 0, 127));
	movemem_light(72, 24, put_light24(44, 55, 66, 0, 127, 0));
	/* El largo no agrupa luces compactas: solo se actualiza la ranura 2. */
	movemem_light(96, 32, put_light16(-300, 400, -500, 7, 8, 9, 1234, 32768, 0xABCD));
	movemem_light(208, 16, put_light16(300, -400, 500, 70, 80, 90, 2345, 65535, 0x1357));
	/* Huecos, ranura posterior a la última y lectura incompleta: ninguno escribe. */
	movemem_light(80, 16, put_light16(1, 1, 1, 99, 99, 99, 1, 1, 1));
	movemem_light(88, 16, put_light16(2, 2, 2, 98, 98, 98, 2, 2, 2));
	movemem_light(224, 16, put_light16(3, 3, 3, 97, 97, 97, 3, 3, 3));
	movemem_light(112, 16, t_n64.bus.rdram_size - 8);
	run();

	CHECK(fabsf(gs.lookat[0][0] - 1.0f) < 1e-5f);
	CHECK(fabsf(gs.lookat[1][1] + 1.0f) < 1e-5f);
	CHECK_EQ((unsigned)gs.lights[0].col[0], 11);
	CHECK_EQ((unsigned)gs.lights[1].col[2], 66);
	CHECK(fabsf(gs.lights[0].dir[2] - 1.0f) < 1e-5f);
	CHECK_EQ((int)gs.rare.acclaim.pos[0][0], -300);
	CHECK_EQ((int)gs.rare.acclaim.pos[0][1], 400);
	CHECK_EQ((int)gs.rare.acclaim.pos[0][2], -500);
	CHECK_EQ((unsigned)gs.lights[2].col[1], 8);
	CHECK_EQ((int)gs.rare.acclaim.ca[0], 1234);
	CHECK(fabsf(gs.rare.acclaim.la[0] - 0.5f) < 1e-7f);
	CHECK_EQ((unsigned)gs.rare.acclaim.qa[0], 0xABCD);
	CHECK_EQ((int)gs.rare.acclaim.pos[7][1], -400);
	CHECK_EQ((unsigned)gs.lights[9].col[2], 90);
	CHECK_EQ((unsigned)gs.rare.acclaim.qa[7], 0x1357);
	CHECK_EQ((unsigned)gs.rare.acclaim.ca[1], 0); /* ni largo ni lectura parcial tocaron la ranura 3 */
}

static void load_attenuation_lights(void)
{
	movemem_light(96, 16, put_light16(1, 2, 3, 100, 80, 60, 2, 32768, 0));
	movemem_light(112, 16, put_light16(1, 2, 3, 0, 0, 40, 2, 32768, 0));
	movemem_light(128, 16, put_light16(1, 2, 3, 255, 255, 255, -1, 65535, 0));
	movemem_light(144, 16, put_light16(1, 2, 3, 255, 255, 255, 2, 0, 0));
	movemem_light(160, 16, put_light16(3, 2, 3, 255, 255, 255, 2, 65535, 0));
	movemem_light(176, 16, put_light16(1, 2, 3, 10, 10, 10, 1, 65535, 0));
	movemem_light(192, 16, put_light16(1, 2, 3, 200, 0, 0, 2, 32768, 0));
}

static void run_lighting(enum salsa64_gbi kind, uint32_t geom, bool ambient, uint8_t expected_a)
{
	start(kind);
	if (ambient) {
		movemem_light(48, 24, put_light24(10, 20, 30, 0, 0, 127));
		dl(0xDB020000u, 0); /* cero luces direccionales: la ranura 0 es ambiental */
	}
	load_attenuation_lights();
	dl(0xD9FFFFFFu, geom);
	if (geom & G_FOG)
		dl(0xDB080000u, 42); /* mul = 0, offset = 42 */
	vertex(put_vertex(1, 2, 3, 10, 20, 30, expected_a), 1, 0);
	run();
}

static void acclaim_attenuation_and_bit(void)
{
	run_lighting(SALSA64_GBI_F3DEX2ACCLAIM, G_ACCLAIM_LIGHTING, false, 77);
	CHECK_EQ(gs.vtx[0].c[0], 255); /* varios aportes y saturación solo al final */
	CHECK_EQ(gs.vtx[0].c[1], 110);
	CHECK_EQ(gs.vtx[0].c[2], 140);
	CHECK_EQ(gs.vtx[0].c[3], 77);

	/* Sin el bit propio, aun con las luces cargadas, queda el color de vértice. */
	run_lighting(SALSA64_GBI_F3DEX2ACCLAIM, 0, false, 78);
	CHECK_EQ(gs.vtx[0].c[0], 10);
	CHECK_EQ(gs.vtx[0].c[1], 20);
	CHECK_EQ(gs.vtx[0].c[2], 30);
	CHECK_EQ(gs.vtx[0].c[3], 78);

	/* El despacho anterior F3DEX2 no activa el camino Acclaim. */
	run_lighting(SALSA64_GBI_F3DEX2, G_ACCLAIM_LIGHTING, false, 79);
	CHECK_EQ(gs.vtx[0].c[0], 10);
	CHECK_EQ(gs.vtx[0].c[1], 20);
	CHECK_EQ(gs.vtx[0].c[2], 30);
	CHECK_EQ(gs.vtx[0].c[3], 79);

	/* Se suma después de la luz normal/ambiental; la niebla decide el alfa. */
	run_lighting(SALSA64_GBI_F3DEX2ACCLAIM, G_LIGHTING | G_ACCLAIM_LIGHTING | G_FOG, true, 80);
	CHECK_EQ(gs.vtx[0].c[0], 255);
	CHECK_EQ(gs.vtx[0].c[1], 110);
	CHECK_EQ(gs.vtx[0].c[2], 140);
	CHECK_EQ(gs.vtx[0].c[3], 42);
}

void t_suite_gfx_acclaim(void)
{
	RUN(acclaim_light_formats);
	RUN(acclaim_attenuation_and_bit);
}
