/*
 * Salsa64 — las tareas de prueba de la HLE gráfica (src/rsp/gfx/selftest.c)
 * que el PPE compara con cada imagen del SPE 0 al cargarla: que de verdad
 * pasen por lo que dicen (dibujos con y sin textura, dos texturas,
 * rectángulos, borrado de z, el fondo de S2DEX2 en la segunda generación),
 * sin errores, y que dos ejecuciones den lo mismo. Y la imagen de cada
 * microcódigo (src/rsp/gfx/spe0.h).
 */
#include "test.h"

#include <stddef.h>
#include <string.h>

#include "core/rcp.h"
#include "rsp/gfx/hle.h"
#include "rsp/gfx/selftest.h"

static struct gfx_state gs;
static uint8_t ram[GFX_SELFTEST_RAM] __attribute__((aligned(128)));
static struct gfx_vertex vtx_a[512] __attribute__((aligned(16))), vtx_b[512] __attribute__((aligned(16)));
static uint8_t cmd_a[16384] __attribute__((aligned(16))), cmd_b[16384] __attribute__((aligned(16)));
static uint8_t tex_a[65536] __attribute__((aligned(128))), tex_b[65536] __attribute__((aligned(128)));
static uint8_t cache_a[65536] __attribute__((aligned(128))), cache_b[65536] __attribute__((aligned(128)));

static void run(uint32_t gbi, struct gfx_vertex *vtx, uint8_t *cmd, uint8_t *tex, uint8_t *cache,
		struct gfx_result *res)
{
	struct gfx_job job;
	memset(&job, 0, sizeof job);
	job.dl = gfx_selftest_build(ram, gbi, 7);
	job.rdram_ea = (uint64_t)(uintptr_t)ram;
	job.rdram_size = GFX_SELFTEST_RAM;
	job.vtx_ea = (uint64_t)(uintptr_t)vtx;
	job.vtx_cap = sizeof vtx_a;
	job.cmd_ea = (uint64_t)(uintptr_t)cmd;
	job.cmd_cap = sizeof cmd_a;
	job.tex_ea = (uint64_t)(uintptr_t)tex;
	job.tex_cap = sizeof tex_a;
	job.cache_ea = (uint64_t)(uintptr_t)cache;
	job.cache_cap = sizeof cache_a;
	job.task = 1;
	job.gbi = gbi;
	job.ucode = GFX_SELFTEST_UCODE;
	memset(vtx, 0, sizeof vtx_a);
	memset(cmd, 0, sizeof cmd_a);
	memset(tex, 0, sizeof tex_a);
	memset(cache, 0, sizeof cache_a);
	gfx_init(&gs);
	gfx_run(&gs, &job, res);
}

static void check_gbi(uint32_t gbi, bool rare)
{
	struct gfx_result ra, rb;
	uint32_t first;
	run(gbi, vtx_a, cmd_a, tex_a, cache_a, &ra);
	run(gbi, vtx_b, cmd_b, tex_b, cache_b, &rb);
	bool gen2 = gbi == SALSA64_GBI_F3DEX2 || gbi == SALSA64_GBI_F3DZEX2;
	CHECK(ra.draws >= 3);
	CHECK_EQ(ra.flags & GFX_RES_CUT, 0);
	CHECK(ra.flags & GFX_RES_FULLSYNC);
	CHECK_EQ(ra.dl_errors, 0);
	if (!rare) {
		/* 2 + 2 + 1 triángulos (y el G_TRI2 tras el fondo), 2 rellenos y 2
		 * TEXRECT (y las tiras del fondo), 2 texturas (y la del fondo). */
		CHECK_EQ(ra.triangles, gen2 ? 7u : 5u);
		CHECK_EQ(ra.rects, gen2 ? 5u : 4u);
		CHECK_EQ(ra.unknown_cmds, 0);
		CHECK_EQ(ra.textures, gen2 ? 3u : 2u);
		CHECK_EQ(ra.bg, gen2 ? 1u : 0u);
	}
	/* Lo mismo dos veces, byte por byte (en el SPE se compara con tolerancia). */
	CHECK_EQ(ra.cmd_bytes, rb.cmd_bytes);
	CHECK_EQ(ra.vtx_bytes, rb.vtx_bytes);
	CHECK_EQ(gfx_selftest_compare_cmd(cmd_a, cmd_b, ra.cmd_bytes, 0, &first), 0);
	CHECK_EQ(gfx_selftest_compare_vtx((const uint8_t *)vtx_a, (const uint8_t *)vtx_b, ra.vtx_bytes, 0, &first), 0);
	CHECK(!memcmp(tex_a, tex_b, sizeof tex_a) && !memcmp(cache_a, cache_b, sizeof cache_a));
}

static void gfx_selftest_lists(void)
{
	check_gbi(SALSA64_GBI_F3D, false);
	check_gbi(SALSA64_GBI_F3DEX, false);
	check_gbi(SALSA64_GBI_F3DWR, false);
	check_gbi(SALSA64_GBI_F3DEX2, false);
	check_gbi(SALSA64_GBI_F3DZEX2, false);
	/* Las de Rare interpretan distinto vértices y triángulos: basta que
	 * dibujen algo y terminen (lo que se compara es SPE contra PPE). */
	check_gbi(SALSA64_GBI_F3DPD, true);
	check_gbi(SALSA64_GBI_F3DDKR, true);
	check_gbi(SALSA64_GBI_F3DJFG, true);
	check_gbi(SALSA64_GBI_F3DEXBG, true);
	check_gbi(SALSA64_GBI_F5ROGUE, true);
}

/* La comparación con tolerancia: los flotantes de los vértices y de los
 * dibujos (xf, k) casi iguales pasan; los enteros (color, la cabecera, las
 * banderas de un dibujo), no. */
static void gfx_selftest_compare_floats(void)
{
	struct gfx_vertex va[2], vb[2];
	uint32_t first, u;
	memset(va, 0, sizeof va);
	va[0].x = 1.0f, va[0].y = -2.5f, va[1].w = 100.0f, va[1].r = 7;
	memcpy(vb, va, sizeof va);
	CHECK_EQ(gfx_selftest_compare_vtx((uint8_t *)va, (uint8_t *)vb, sizeof va, 4, &first), 0);
	memcpy(&u, &vb[0].y, 4);
	u += 3;  /* 3 ulps */
	memcpy(&vb[0].y, &u, 4);
	CHECK_EQ(gfx_selftest_compare_vtx((uint8_t *)va, (uint8_t *)vb, sizeof va, 4, &first), 0);
	CHECK_EQ(gfx_selftest_compare_vtx((uint8_t *)va, (uint8_t *)vb, sizeof va, 2, &first), 1);
	CHECK_EQ(first, 4);
	/* El SPE deja 1 en vez de 0 en un flotante: pasa. */
	memcpy(vb, va, sizeof va);
	u = 1;
	memcpy(&vb[1].s, &u, 4);
	CHECK_EQ(gfx_selftest_compare_vtx((uint8_t *)va, (uint8_t *)vb, sizeof va, 4, &first), 0);
	/* El color es exacto. */
	vb[1].r = 8;
	CHECK_EQ(gfx_selftest_compare_vtx((uint8_t *)va, (uint8_t *)vb, sizeof va, 64, &first), 1);
	CHECK_EQ(first, sizeof(struct gfx_vertex) + offsetof(struct gfx_vertex, r));

	static struct gfx_draw da, db;
	memset(&da, 0, sizeof da);
	da.h.op = GFX_OP_DRAW;
	da.h.size = sizeof da;
	da.tex[1].xf[2] = 0.25f;
	da.k[3][1][2] = 0.5f;
	da.blend = 1;
	db = da;
	memcpy(&u, &db.k[3][1][2], 4);
	u += 2;
	memcpy(&db.k[3][1][2], &u, 4);
	u = 1;
	memcpy(&db.tex[0].xf[3], &u, 4);
	CHECK_EQ(gfx_selftest_compare_cmd((uint8_t *)&da, (uint8_t *)&db, sizeof da, 4, &first), 0);
	db.blend = 0;
	CHECK_EQ(gfx_selftest_compare_cmd((uint8_t *)&da, (uint8_t *)&db, sizeof da, 4, &first), 1);
	CHECK_EQ(first, offsetof(struct gfx_draw, blend) & ~3u);
}

/* Cada microcódigo con HLE tiene su imagen, y la regla de spe0.h: lo que se
 * mezcla en una tarea (S2DEX2 y F3DFLX dentro de F3DEX2) va con su familia. */
static void gfx_spe0_images(void)
{
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3D), GFX_SPE0_F3D);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3DEX), GFX_SPE0_F3D);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3DWR), GFX_SPE0_F3D);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3DPD), GFX_SPE0_F3D);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3DDKR), GFX_SPE0_F3D);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3DJFG), GFX_SPE0_F3D);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3DEX2), GFX_SPE0_F3DEX2);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3DZEX2), GFX_SPE0_F3DEX2);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F3DEXBG), GFX_SPE0_CONKER);
	CHECK_EQ(gfx_spe0_image(SALSA64_GBI_F5ROGUE), GFX_SPE0_F5ROGUE);
	CHECK_EQ((uint64_t)(int64_t)gfx_spe0_image(SALSA64_GBI_UNKNOWN), (uint64_t)(int64_t)-1);
	CHECK((GFX_SPE0_PARTS_1 & (GFX_PART_S2DEX2 | GFX_PART_FLX)) == (GFX_PART_S2DEX2 | GFX_PART_FLX));
	CHECK((GFX_SPE0_PARTS_2 & GFX_PART_F3DEX2) && (GFX_SPE0_PARTS_0 & GFX_PART_F3D));
	CHECK((GFX_SPE0_PARTS_3 & GFX_PART_F5ROGUE) && (GFX_SPE0_PARTS_3 & GFX_PART_F3D) && !(GFX_SPE0_PARTS_3 & GFX_PART_F3DEX2));
	/* El host compila todo. */
	CHECK_EQ(GFX_PARTS, GFX_PART_ALL);
}

void t_suite_gfx_selftest(void)
{
	RUN(gfx_selftest_lists);
	RUN(gfx_selftest_compare_floats);
	RUN(gfx_spe0_images);
}
