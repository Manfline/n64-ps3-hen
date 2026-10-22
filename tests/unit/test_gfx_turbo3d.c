/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Listas de objetos, matrices, vértices y comandos RDP propios. */
#include "test.h"
#include <math.h>
#include <string.h>
#include "core/endian.h"
#include "rsp/gfx/hle.h"
#include "rsp/gfx/selftest.h"

static struct gfx_state gs;
static uint8_t ram[GFX_SELFTEST_RAM] __attribute__((aligned(128)));
static struct gfx_vertex vtx[512] __attribute__((aligned(16)));
static uint8_t cmd[16384] __attribute__((aligned(16)));
static uint8_t tex[65536] __attribute__((aligned(128)));
static struct gfx_job job;
static struct gfx_result res;

static void start(void)
{
	memset(&job, 0, sizeof job);
	job.dl = gfx_selftest_build(ram, SALSA64_GBI_TURBO3D, 7);
	job.rdram_ea = (uint64_t)(uintptr_t)ram;
	job.rdram_size = sizeof ram;
	job.vtx_ea = (uint64_t)(uintptr_t)vtx;
	job.vtx_cap = sizeof vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)cmd;
	job.cmd_cap = sizeof cmd;
	job.tex_ea = (uint64_t)(uintptr_t)tex;
	job.tex_cap = sizeof tex;
	job.gbi = SALSA64_GBI_TURBO3D;
	gfx_init(&gs);
}

static void run(void) { gfx_run(&gs, &job, &res); }
static void rejected(void)
{
	run();
	CHECK_EQ(res.dl_errors, 1);
	CHECK(res.flags & GFX_RES_FOREIGN_END);
}

static void turbo_models(void)
{
	start(); run();
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.flags & GFX_RES_CUT, 0);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK_EQ(res.vtx_bytes, 6 * sizeof *vtx);
	CHECK(fabsf(vtx[0].x - 40.0f) < 0.01f);
	CHECK_EQ(vtx[0].r, ram[0x1100 + 12]);
	CHECK(gs.vtx[1].s == 16.0f);
	CHECK(!memcmp(vtx, vtx + 3, 3 * sizeof *vtx));
}

static void turbo_segments(void)
{
	start();
	/* El global se resuelve con los segmentos anteriores, el objeto y
	 * su RDP con los que acaba de cargar. */
	salsa64_wr32(ram + 0x6000 + 16 + 4, 0x6000);
	salsa64_wr32(ram + 4, 0x01000100);
	salsa64_wr32(ram + 0x6100 + 12, 0x01000420);
	run();
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.triangles, 2);
	CHECK(res.flags & GFX_RES_FULLSYNC);
}

static void turbo_bounds(void)
{
	start(); job.dl = sizeof ram - 8; rejected();
	start(); job.dl = 1; rejected();
	start(); salsa64_wr32(ram + 4, sizeof ram - 16); rejected();
	start(); salsa64_wr32(ram, sizeof ram - 96); rejected();
	start(); salsa64_wr32(ram + 8, sizeof ram - 16); rejected();
	start(); salsa64_wr32(ram + 12, sizeof ram - 2); rejected();
	start(); ram[0x6100 + 8] = 4; rejected(); /* 253 + 4 */
	start(); ram[0x6300] = 252; rejected();  /* no cargado */
}

static void turbo_no_stale_vertices(void)
{
	start(); run();
	CHECK_EQ(res.triangles, 2);
	salsa64_wr32(ram + 8, 0); /* no hereda los de la tarea anterior */
	rejected();
}

static void turbo_matrix_bounds(void)
{
	start();
	uint32_t a = sizeof ram - 24;
	memcpy(ram + a, ram + 0x6100, 24);
	salsa64_wr32(ram + 4, a);
	rejected();
	start();
	/* Traslación X +1/4 en la matriz ya combinada, no multiplicarla otra vez. */
	salsa64_wr16(ram + 0x6100 + 24 + 32 + 24, 16384);
	run();
	CHECK_EQ(res.dl_errors, 0);
	CHECK(fabsf(vtx[0].x - 80.0f) < 0.01f);
}

static void turbo_rdp_bounds(void)
{
	start(); salsa64_wr32(ram + 0x6100 + 12, 0x6401); rejected();
	start();
	salsa64_wr32(ram + 0x6100 + 12, sizeof ram - 8);
	salsa64_wr32(ram + sizeof ram - 8, 0xE4004004); /* TEXRECT sin segunda palabra doble */
	rejected();
	start();
	salsa64_wr32(ram + 0x6100 + 12, sizeof ram - 8);
	salsa64_wr32(ram + sizeof ram - 8, 0xCF000000); /* triángulo crudo incompleto */
	rejected();
}

static void turbo_texrect_first(void)
{
	start();
	/* El primer comando de la lista auxiliar tiene 16 bytes. */
	salsa64_wr32(ram + 0x6100 + 12, 0x6500);
	salsa64_wr32(ram + 0x6500, 0xE4004004);
	salsa64_wr32(ram + 0x6504, 0);
	salsa64_wr32(ram + 0x6508, 0);
	salsa64_wr32(ram + 0x650C, 0x04000400);
	run();
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.rects, 1);
	CHECK_EQ(res.triangles, 2);
}

static void turbo_zero_sum(void)
{
	start();
	salsa64_wr32(ram + 0x6100 + 12, 0x6500);
	salsa64_wr32(ram + 0x6500, 0xFA000000);
	salsa64_wr32(ram + 0x6504, 0x06000000); /* suma cero, no terminador */
	salsa64_wr32(ram + 0x6508, 0xE9000000);
	run();
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(gs.prim, 0x06000000);
	CHECK(res.flags & GFX_RES_FULLSYNC);
}

static void turbo_budget(void)
{
	start();
	memset(ram, 0, 0x2000);
	memset(ram + 0x6100, 0, 88);
	ram[0x6100 + 11] = 1;
	salsa64_wr32(ram + 0x6100 + 12, 0x8000);
	for (unsigned i = 0; i < 512; i++)
		salsa64_wr32(ram + 16 * i + 4, 0x6100);
	for (unsigned i = 0; i < 4096; i++)
		salsa64_wr32(ram + 0x8000 + 8 * i, 0xE7000000);
	run();
	CHECK_EQ(res.commands, GFX_MAX_COMMANDS);
	CHECK(res.flags & GFX_RES_LOOP);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.triangles, 0);
}

void t_suite_gfx_turbo3d(void)
{
	RUN(turbo_models);
	RUN(turbo_segments);
	RUN(turbo_bounds);
	RUN(turbo_no_stale_vertices);
	RUN(turbo_matrix_bounds);
	RUN(turbo_rdp_bounds);
	RUN(turbo_texrect_first);
	RUN(turbo_zero_sum);
	RUN(turbo_budget);
}
