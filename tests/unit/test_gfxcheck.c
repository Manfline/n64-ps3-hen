/*
 * Salsa64 — pruebas del censo gráfico (src/host/gfxcheck.c): la
 * comparación de píxeles y su clasificación, el estado del RDP y el cauce
 * completo (dibujar con el rasterizador de la HLE dos salidas y compararlas).
 * Solo en el host: en la PS3 la suite está vacía.
 */
#include "test.h"

#ifdef GFX_CENSUS

#include <stdlib.h>
#include <string.h>

#include "core/endian.h"
#include "host/gfxcheck.h"
#include "host/raster.h"
#include "rsp/gfx/hle.h"

#define W RASTER_W
#define H RASTER_H

static uint32_t pre[W * H], lle[W * H], hle[W * H];

static void clear_all(void)
{
	memset(pre, 0, sizeof pre);
	memset(lle, 0, sizeof lle);
	memset(hle, 0, sizeof hle);
}

static void rect(uint32_t *img, unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t c)
{
	for (unsigned y = y0; y < y1; y++)
		for (unsigned x = x0; x < x1; x++)
			img[y * W + x] = c;
}

static struct gchk_px compare(void)
{
	struct gchk_px p;
	gchk_px_init(&p);
	gchk_px_add(&p, pre, lle, hle, W, H, W, 0x100000);
	return p;
}

static void gchk_identical(void)
{
	clear_all();
	rect(lle, 10, 10, 60, 60, 0x884422);
	rect(hle, 10, 10, 60, 60, 0x884422);
	struct gchk_px p = compare();
	CHECK_EQ(p.ndiff, 0);
	CHECK_EQ(p.drawn_l, 2500);
	CHECK_EQ(p.drawn_h, 2500);
	CHECK_EQ(p.area, W * H);
	CHECK_EQ(gchk_classify(&p), GCHK_SAME);
	CHECK_EQ(gchk_kind_of(&p), GCHK_KIND_NONE);
}

/* Un paso de 5 bits por canal (8): el redondeo del combiner o del sombreado. */
static void gchk_minor_rounding(void)
{
	clear_all();
	rect(lle, 0, 0, W, H, 0x808080);
	rect(hle, 0, 0, W, H, 0x888888);
	struct gchk_px p = compare();
	CHECK_EQ(p.ndiff, W * H);
	CHECK_EQ(p.maxdiff, 8);
	CHECK_EQ(gchk_classify(&p), GCHK_MINOR);
	/* Hasta 16 de diferencia en una zona lisa la absorbe la media de 5 × 5... */
	rect(hle, 0, 0, W, H, 0x909090);
	p = compare();
	CHECK_EQ(p.maxdiff, 16);
	CHECK_EQ(gchk_classify(&p), GCHK_MINOR);
	/* ...más ya es otro color. */
	rect(hle, 0, 0, W, H, 0x959595);
	p = compare();
	CHECK_EQ(p.maxdiff, 21);
	CHECK_EQ(gchk_classify(&p), GCHK_DIFF);
	CHECK(gchk_is_big(&p));
}

/* Una figura corrida un píxel (cobertura del borde, muestreo medio texel
 * corrido) deja diferencias que se explican por la vecindad: menor. */
static void gchk_edge_shift(void)
{
	clear_all();
	rect(lle, 100, 100, 160, 160, 0xFFFFFF);
	rect(hle, 101, 100, 161, 160, 0xFFFFFF);
	struct gchk_px p = compare();
	CHECK_EQ(p.ndiff, 120);  /* una columna de 60 a cada lado */
	CHECK_EQ(p.nbad, 0);
	CHECK_EQ(gchk_classify(&p), GCHK_MINOR);
	/* Corrida diez píxeles ya es otra figura. */
	rect(hle, 100, 100, 160, 160, 0);
	rect(hle, 110, 100, 170, 160, 0xFFFFFF);
	p = compare();
	CHECK(p.nbad > 0);
	CHECK_EQ(gchk_classify(&p), GCHK_DIFF);
}

static void gchk_missing_extra_color(void)
{
	/* La HLE no dibuja lo que dibuja el microcódigo. */
	clear_all();
	rect(lle, 20, 30, 80, 90, 0xFF0000);
	struct gchk_px p = compare();
	CHECK_EQ(p.ndiff, 3600);
	CHECK_EQ(p.only_l, 3600);
	CHECK_EQ(p.only_h, 0);
	CHECK_EQ(gchk_classify(&p), GCHK_DIFF);
	CHECK_EQ(gchk_kind_of(&p), GCHK_KIND_MISSING);
	CHECK(gchk_is_big(&p));
	CHECK_EQ(p.x0, 20);
	CHECK_EQ(p.y0, 30);
	CHECK_EQ(p.x1, 80);
	CHECK_EQ(p.y1, 90);
	CHECK_EQ(p.addr, 0x100000);
	/* La HLE dibuja de más. */
	clear_all();
	rect(hle, 20, 30, 40, 50, 0x00FF00);
	p = compare();
	CHECK_EQ(gchk_kind_of(&p), GCHK_KIND_EXTRA);
	CHECK_EQ(gchk_classify(&p), GCHK_DIFF);
	CHECK(!gchk_is_big(&p));  /* 400 píxeles: 0,5 % del área */
	/* Los dos dibujan lo mismo con otro color. */
	clear_all();
	rect(lle, 20, 30, 80, 90, 0xFF0000);
	rect(hle, 20, 30, 80, 90, 0x0000FF);
	p = compare();
	CHECK_EQ(gchk_kind_of(&p), GCHK_KIND_COLOR);
	CHECK_EQ(p.both, 3600);
	/* Algo ya dibujado antes de la tarea y que no cambia no cuenta. */
	clear_all();
	rect(pre, 0, 0, 50, 50, 0x123456);
	rect(lle, 0, 0, 50, 50, 0x123456);
	rect(hle, 0, 0, 50, 50, 0x123456);
	p = compare();
	CHECK_EQ(p.ndiff, 0);
	CHECK_EQ(p.drawn_l, 0);
}

/* Unos pocos píxeles de más (≤ 0,2 % del área) con un color lejano son una
 * diferencia menor: bordes sueltos. */
static void gchk_few_pixels(void)
{
	clear_all();
	rect(hle, 5, 5, 15, 15, 0xFFFFFF);  /* 100 píxeles = 0,13 % */
	struct gchk_px p = compare();
	CHECK_EQ(gchk_classify(&p), GCHK_MINOR);
	rect(hle, 5, 5, 25, 25, 0xFFFFFF);  /* 400 píxeles */
	p = compare();
	CHECK_EQ(gchk_classify(&p), GCHK_DIFF);
}

static void gchk_class_names(void)
{
	CHECK(strcmp(gchk_class_name(GCHK_SAME), "idéntica") == 0);
	CHECK(strcmp(gchk_class_name(GCHK_NOHLE), "sin HLE") == 0);
	CHECK(strcmp(gchk_kind_name(GCHK_KIND_MISSING), "la HLE no dibuja") == 0);
}

static struct gfx_state sa, sb;

static uint64_t named(const char *name)
{
	for (unsigned i = 0; i < gchk_state_nfields(); i++)
		if (!strcmp(gchk_state_field_name(i), name))
			return 1ull << i;
	return 0;
}

static void gchk_state(void)
{
	gfx_init(&sa);
	gfx_init(&sb);
	CHECK_EQ(gchk_state_diff(&sa, &sb), 0);
	sa.oh ^= 0x4000;
	sb.cc_lo ^= 1;
	sb.tiles[5].line = 7;
	sa.tiles[2].line = 3;
	uint64_t d = gchk_state_diff(&sa, &sb);
	CHECK_EQ(d, named("oh") | named("cc_lo") | named("tile.line"));
	CHECK(named("oh") != 0 && named("cc_lo") != 0 && named("tile.line") != 0);
	/* Los bits 0 a 7 de othermode H (máscara de mezcla y dither: la HLE no los lleva) se cuentan aparte. */
	gfx_init(&sa);
	gfx_init(&sb);
	sa.oh ^= 0x30;
	CHECK_EQ(gchk_state_diff(&sa, &sb), named("oh.dither"));
	sa.oh ^= 0x30;
	sa.oh ^= 0x100000;  /* el tipo de ciclo sí */
	CHECK_EQ(gchk_state_diff(&sa, &sb), named("oh"));
	/* Lo que no es del RDP (matrices, vértices) no cuenta. */
	gfx_init(&sb);
	gfx_init(&sa);
	sa.mv_top = 3;
	sa.vtx_count = 9;
	CHECK_EQ(gchk_state_diff(&sa, &sb), 0);
}

/* El cauce entero: dos salidas de la HLE (aquí armadas a mano, un TARGET y
 * un dibujo que pinta toda la imagen) se dibujan desde el mismo estado del
 * rasterizador y se comparan. */
#define RAM_SIZE (4u << 20)
#define FB       0x100000u

static uint8_t *ram;
static struct gfx_vertex vtx[6] __attribute__((aligned(16)));
static uint8_t cmd[sizeof(struct gfx_cmd) + sizeof(struct gfx_draw) + sizeof(struct gfx_cmd)]
	__attribute__((aligned(16)));

static void build_job(struct gfx_job *job, struct gfx_result *res, uint32_t r, uint32_t g, uint32_t b, float x1)
{
	memset(cmd, 0, sizeof cmd);
	struct gfx_cmd *t = (struct gfx_cmd *)cmd;
	t->op = GFX_OP_TARGET;
	t->size = sizeof *t;
	t->a = FB;
	t->b = W;
	t->c = 2;
	struct gfx_draw *d = (struct gfx_draw *)(cmd + sizeof *t);
	d->h.op = GFX_OP_DRAW;
	d->h.size = sizeof *d;
	d->h.a = 0;
	d->h.b = 6;
	d->scissor[2] = (uint16_t)(W * 4);
	d->scissor[3] = (uint16_t)(H * 4);
	d->k[GFX_K_ONE][0][0] = r / 255.0f;
	d->k[GFX_K_ONE][0][1] = g / 255.0f;
	d->k[GFX_K_ONE][0][2] = b / 255.0f;
	d->k[GFX_K_ONE][0][3] = 1.0f;
	static const float xs[6] = { 0, 1, 0, 1, 1, 0 }, ys[6] = { 0, 0, 1, 0, 1, 1 };
	for (unsigned i = 0; i < 6; i++) {
		memset(&vtx[i], 0, sizeof vtx[i]);
		vtx[i].x = xs[i] * x1;
		vtx[i].y = ys[i] * (float)H;
		vtx[i].w = 1.0f;
	}
	memset(job, 0, sizeof *job);
	memset(res, 0, sizeof *res);
	job->rdram_ea = (uint64_t)(uintptr_t)ram;
	job->rdram_size = RAM_SIZE;
	job->vtx_ea = (uint64_t)(uintptr_t)vtx;
	job->cmd_ea = (uint64_t)(uintptr_t)cmd;
	res->cmd_bytes = sizeof cmd;
}

static void gchk_raster_pipeline(void)
{
	ram = calloc(1, RAM_SIZE);
	CHECK(ram != NULL);
	if (!ram)
		return;
	int fbfx = raster_fbfx;
	raster_fbfx = 2;  /* con copias a la RDRAM: la comparación debe dejarla como estaba */
	raster_reset();
	void *pre_s = malloc(raster_state_size()), *post_s = malloc(raster_state_size());
	struct gfx_job job;
	struct gfx_result res;

	/* El «LLE»: la pantalla entera de rojo. */
	raster_state_save(pre_s);
	build_job(&job, &res, 255, 0, 0, (float)W);
	raster_task(&job, &res);
	raster_state_save(post_s);
	uint8_t *before = malloc(RAM_SIZE);
	memcpy(before, ram, RAM_SIZE);

	/* La «HLE» igual: idénticas, y el rasterizador vuelve al estado del LLE. */
	struct gchk_px p;
	build_job(&job, &res, 255, 0, 0, (float)W);
	gchk_compare_raster(pre_s, post_s, &job, &res, &p);
	CHECK_EQ(p.ndiff, 0);
	CHECK_EQ(p.area, W * H);
	CHECK(p.drawn_l > 0);
	CHECK_EQ(p.drawn_l, p.drawn_h);
	CHECK_EQ(gchk_classify(&p), GCHK_SAME);
	CHECK_EQ(memcmp(ram, before, RAM_SIZE), 0);
	uint32_t w;
	const uint32_t *img = raster_image(FB, &w);
	CHECK(img != NULL);
	if (img)
		CHECK_EQ(img[10 * W + 10] & 0xFFFFFF, 0xFF0000);  

	/* Una «HLE» que pinta solo la mitad izquierda de verde: distinta; la de
	 * antes sigue siendo el estado del rasterizador. */
	build_job(&job, &res, 0, 255, 0, (float)(W / 2));
	gchk_compare_raster(pre_s, post_s, &job, &res, &p);
	CHECK(p.ndiff > 0);
	CHECK_EQ(gchk_classify(&p), GCHK_DIFF);
	CHECK(gchk_is_big(&p));
	CHECK_EQ(p.addr, FB);
	CHECK_EQ(p.ntgt, 1);
	CHECK_EQ(memcmp(ram, before, RAM_SIZE), 0);
	img = raster_image(FB, &w);
	if (img)
		CHECK_EQ(img[10 * W + 10] & 0xFFFFFF, 0xFF0000);

	free(before);
	free(pre_s);
	free(post_s);
	free(ram);
	ram = NULL;
	raster_reset();
	raster_fbfx = fbfx;
}

void t_suite_gfxcheck(void)
{
	RUN(gchk_identical);
	RUN(gchk_minor_rounding);
	RUN(gchk_edge_shift);
	RUN(gchk_missing_extra_color);
	RUN(gchk_few_pixels);
	RUN(gchk_class_names);
	RUN(gchk_state);
	RUN(gchk_raster_pipeline);
}

#else

void t_suite_gfxcheck(void)
{
}

#endif
