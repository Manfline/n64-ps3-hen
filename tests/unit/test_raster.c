/*
 * Salsa64 — pruebas del rasterizador de referencia del host
 * (src/host/raster.c): el espejo del framebuffer en la RDRAM. Las tareas
 * son salidas de la HLE armadas a mano (un TARGET y un DRAW que pinta toda
 * la imagen de un color) y los campos, imágenes del VI; se mira la RDRAM.
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "core/rcp.h"
#include "host/raster.h"

#define W     320u
#define H     240u
#define LINE  (W * 2)
#define FB    (LINE * H)
/* Tres framebuffers seguidos, como los de un juego con triple búfer. */
#define FB_A  0x10000u
#define FB_B  (FB_A + FB)
#define FB_C  (FB_B + FB)

static uint8_t ram[FB_C + FB] __attribute__((aligned(16)));
static struct gfx_vertex vtx[6] __attribute__((aligned(16)));
static uint8_t cmd[sizeof(struct gfx_cmd) + sizeof(struct gfx_draw) + sizeof(struct gfx_cmd)]
	__attribute__((aligned(16)));

static uint16_t rgba5551(uint32_t r, uint32_t g, uint32_t b)
{
	return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1);
}

static uint16_t px(uint32_t fb, uint32_t x, uint32_t y)
{
	return salsa64_rd16(ram + fb + (y * W + x) * 2);
}

static void set_vtx(struct gfx_vertex *v, float x, float y)
{
	memset(v, 0, sizeof *v);
	v->x = x;
	v->y = y;
	v->w = 1.0f;
}

/* Una tarea con un FILLRECT de `x0, y0`–`x1, y1` sobre una color image de
 * `fb`/`w` cuyas tijeras llegan a la fila `lh` (más de una pantalla). */
static void task_tall_fill(uint32_t fb, uint32_t w, uint32_t lh, float x0, float y0, float x1, float y1, uint32_t r,
			   uint32_t g, uint32_t b)
{
	set_vtx(&vtx[0], x0, y0);
	set_vtx(&vtx[1], x1, y0);
	set_vtx(&vtx[2], x0, y1);
	set_vtx(&vtx[3], x1, y0);
	set_vtx(&vtx[4], x1, y1);
	set_vtx(&vtx[5], x0, y1);
	memset(cmd, 0, sizeof cmd);
	struct gfx_cmd *t = (struct gfx_cmd *)cmd;
	t->op = GFX_OP_TARGET;
	t->size = sizeof *t;
	t->a = fb;
	t->b = w;
	t->c = 2;
	struct gfx_draw *d = (struct gfx_draw *)(cmd + sizeof *t);
	d->h.op = GFX_OP_DRAW;
	d->h.size = sizeof *d;
	d->h.a = 0;
	d->h.b = 6;
	d->cycle = 3;  /* G_CYC_FILL */
	d->scissor[2] = (uint16_t)(w * 4);
	d->scissor[3] = (uint16_t)(lh * 4);
	d->k[GFX_K_ONE][0][0] = r / 255.0f;
	d->k[GFX_K_ONE][0][1] = g / 255.0f;
	d->k[GFX_K_ONE][0][2] = b / 255.0f;
	d->k[GFX_K_ONE][0][3] = 1.0f;
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	memset(&res, 0, sizeof res);
	job.rdram_ea = (uint64_t)(uintptr_t)ram;
	job.rdram_size = sizeof ram;
	job.vtx_ea = (uint64_t)(uintptr_t)vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)cmd;
	res.cmd_bytes = sizeof cmd;
	raster_task(&job, &res);
}

/* Una tarea que pinta toda la color image `fb` (`w` de ancho, 16 bits). */
static void task_fill_w(uint32_t fb, uint32_t w, uint32_t r, uint32_t g, uint32_t b)
{
	memset(cmd, 0, sizeof cmd);
	struct gfx_cmd *t = (struct gfx_cmd *)cmd;
	t->op = GFX_OP_TARGET;
	t->size = sizeof *t;
	t->a = fb;
	t->b = w;
	t->c = 2;  /* G_IM_SIZ_16b, G_IM_FMT_RGBA */
	struct gfx_draw *d = (struct gfx_draw *)(cmd + sizeof *t);
	d->h.op = GFX_OP_DRAW;
	d->h.size = sizeof *d;
	d->h.a = 0;
	d->h.b = 6;
	d->scissor[2] = (uint16_t)(w * 4);
	d->scissor[3] = (uint16_t)(w * 3 / 4 * 4);
	d->k[GFX_K_ONE][0][0] = r / 255.0f;
	d->k[GFX_K_ONE][0][1] = g / 255.0f;
	d->k[GFX_K_ONE][0][2] = b / 255.0f;
	d->k[GFX_K_ONE][0][3] = 1.0f;
	float fw = (float)w, fh = (float)(w * 3 / 4);
	set_vtx(&vtx[0], 0, 0);
	set_vtx(&vtx[1], fw, 0);
	set_vtx(&vtx[2], 0, fh);
	set_vtx(&vtx[3], fw, 0);
	set_vtx(&vtx[4], fw, fh);
	set_vtx(&vtx[5], 0, fh);

	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	memset(&res, 0, sizeof res);
	job.rdram_ea = (uint64_t)(uintptr_t)ram;
	job.rdram_size = sizeof ram;
	job.vtx_ea = (uint64_t)(uintptr_t)vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)cmd;
	res.cmd_bytes = sizeof cmd;  /* el último gfx_cmd queda en 0: GFX_OP_END */
	raster_task(&job, &res);
}

static void task_fill(uint32_t fb, uint32_t r, uint32_t g, uint32_t b)
{
	task_fill_w(fb, W, r, g, b);
}

/* Un campo del VI mostrando el framebuffer `fb` (desde su línea 1, como SM64). */
static void field(uint32_t fb)
{
	struct salsa64_vi_frame f;
	memset(&f, 0, sizeof f);
	f.type = SALSA64_VI_RGBA5551;
	f.origin = fb + LINE;
	f.width = W;
	f.height = H - 3;
	f.stride = LINE;
	f.bpp = 2;
	raster_vi(&f);
}

/* Lo que un DMA del juego deja en la memoria de un framebuffer. */
static void dma_data(uint32_t fb)
{
	for (uint32_t y = 20; y < 40; y++)
		for (uint32_t x = 0; x < LINE; x++)
			ram[fb + y * LINE + x] = (uint8_t)(0xA5 ^ x ^ y);
}

static int data_intact(uint32_t fb)
{
	for (uint32_t y = 20; y < 40; y++)
		for (uint32_t x = 0; x < LINE; x++)
			if (ram[fb + y * LINE + x] != (uint8_t)(0xA5 ^ x ^ y))
				return 0;
	return 1;
}

/* Super Mario 64 al pasar del logo a la cara de Mario, sin datos del juego:
 * triple búfer de 320 × 240; el RDP dibuja B, el VI sigue dos campos en A,
 * un DMA carga datos encima de A y de B, y el VI muestra B (y A) sin que
 * el RDP vuelva a dibujarlos. El espejo copiaba el framebuffer presentado
 * en cada campo y pisaba esos datos: el juego dejaba de mandar tareas
 * gráficas (la 96 era la última). Ahora lo dibujado se copia una vez, al
 * terminar la tarea si ya se sabe que es un framebuffer y si no al
 * presentarlo por primera vez. */
static void raster_frame_mirror_sm64(void)
{
	const uint32_t fb[3] = { FB_A, FB_B, FB_C };
	int fbfx = raster_fbfx;
	raster_reset();
	raster_fbfx = 2;
	memset(ram, 0, sizeof ram);

	/* El arranque: tareas que solo dibujan una imagen chica lejos de los
	 * framebuffers (una copia reciente protege su memoria durante
	 * RB_KEEP_TASKS tareas, y al empezar todas cuentan como recientes). */
	for (int k = 0; k < 40; k++)
		task_fill_w(0x1000, 64, 0, 0, 0);

	/* Antes de que el VI muestre nada no se sabe que es un framebuffer: el
	 * primer cuadro llega a la RDRAM al presentarlo. */
	task_fill(FB_A, 255, 0, 0);
	CHECK_EQ(px(FB_A, 10, 10), 0);
	field(FB_A);
	CHECK_EQ(px(FB_A, 10, 10), rgba5551(255, 0, 0));
	CHECK_EQ(px(FB_A, 319, 237), rgba5551(255, 0, 0));
	field(FB_A);

	/* Triple búfer: cada tarea dibuja uno y el VI lo muestra dos campos.
	 * Los ya mostrados alguna vez se copian al terminar la tarea. */
	for (uint32_t k = 1; k < 7; k++) {
		uint32_t f = fb[k % 3], r = 30 * k;
		task_fill(f, r, 0, 0);
		if (k >= 3)
			CHECK_EQ(px(f, 10, 10), rgba5551(r, 0, 0));
		field(f);
		field(f);
		CHECK_EQ(px(f, 10, 10), rgba5551(r, 0, 0));
	}

	/* El cambio de escena. */
	task_fill(FB_B, 0, 255, 0);
	CHECK_EQ(px(FB_B, 10, 10), rgba5551(0, 255, 0));
	field(FB_A);
	field(FB_A);
	dma_data(FB_A);
	dma_data(FB_B);
	field(FB_B);
	field(FB_B);
	field(FB_A);
	CHECK(data_intact(FB_A));
	CHECK(data_intact(FB_B));
	CHECK_EQ(px(FB_B, 10, 10), rgba5551(0, 255, 0));  /* lo que el RDP dejó fuera del DMA */

	/* Lo que el RDP vuelve a dibujar sí se copia. */
	task_fill(FB_A, 0, 0, 255);
	CHECK_EQ(px(FB_A, 10, 30), rgba5551(0, 0, 255));

	raster_reset();
	raster_fbfx = fbfx;
}

/* Star Fox 64: una tarea limpia el framebuffer y, antes de que termine, el
 * juego carga por DMA datos encima; el espejo al terminar la tarea no debe
 * pisarlos. Un DMA anterior a la tarea sí se pisa (la copia es más nueva). */
static void raster_mirror_after_dma(void)
{
	raster_reset();
	memset(ram, 0, sizeof ram);
	for (int k = 0; k < 40; k++)
		task_fill_w(0x1000, 64, 0, 0, 0);
	task_fill(FB_A, 255, 0, 0);
	field(FB_A);
	field(FB_A);
	uint32_t t = 41;  /* tareas hechas: la próxima es la 42 */
	/* DMA antes de la tarea 42: la tarea lo pisa, como el RDP. */
	dma_data(FB_A);
	raster_rdram_written(FB_A, FB, t);
	task_fill(FB_A, 0, 255, 0);
	CHECK_EQ(px(FB_A, 10, 30), rgba5551(0, 255, 0));
	/* DMA mientras corre la tarea 43 (en vuelo: 43): el espejo no lo pisa. */
	t = 43;
	dma_data(FB_A);
	raster_rdram_written(FB_A + 20 * LINE, 20 * LINE, t);
	task_fill(FB_A, 0, 0, 255);
	CHECK(data_intact(FB_A));
	CHECK_EQ(px(FB_A, 10, 10), rgba5551(0, 255, 0));  /* el cuadro anterior sigue ahí */
	/* La tarea 44 vuelve a dibujar después del DMA: se copia. */
	task_fill(FB_A, 0, 0, 255);
	CHECK_EQ(px(FB_A, 10, 30), rgba5551(0, 0, 255));
	raster_reset();
}

/* Star Fox 64: FILLRECT de 320 × 720 sobre una color image en el primer
 * framebuffer, que limpia los tres seguidos (y su borde de 8 px, que el
 * juego nunca vuelve a tocar). No se crea un destino de 720: el relleno va
 * a cada framebuffer existente. */
static void raster_tall_fill(void)
{
	raster_reset();
	memset(ram, 0, sizeof ram);
	task_fill(FB_A, 255, 0, 0);
	task_fill(FB_B, 0, 255, 0);
	task_fill(FB_C, 0, 0, 255);
	task_tall_fill(FB_A, W, 3 * H, 0, 0, (float)W, 3.0f * H, 40, 50, 60);
	uint32_t w = 0;
	const uint32_t *a = raster_image(FB_A, &w), *b = raster_image(FB_B, &w), *c = raster_image(FB_C, &w);
	CHECK(a && b && c);
	if (!a || !b || !c)
		return;
	CHECK_EQ(a[3 * W + 3], 0x28323cu);  /* (40, 50, 60) */
	CHECK_EQ(b[100 * W + 200], 0x28323cu);
	CHECK_EQ(c[(H - 1) * W + (W - 1)], 0x28323cu);
	/* Solo las filas 500–520 de la imagen alta: la parte alta de C. */
	task_tall_fill(FB_A, W, 3 * H, 0, 500, (float)W, 520, 1, 2, 3);
	CHECK_EQ(c[25 * W + 7], 0x010203u);
	CHECK_EQ(c[41 * W + 7], 0x28323cu);
	CHECK_EQ(b[100 * W + 200], 0x28323cu);
	raster_reset();
}

void t_suite_raster(void)
{
	RUN(raster_tall_fill);
	RUN(raster_mirror_after_dma);
	RUN(raster_frame_mirror_sm64);
}
