/*
 * Salsa64 — pruebas del rasterizador de referencia del host
 * (src/host/raster.c): el espejo del framebuffer en la RDRAM. Las tareas
 * son salidas de la HLE armadas a mano (un TARGET y un DRAW que pinta toda
 * la imagen de un color) y los campos, imágenes del VI; se mira la RDRAM.
 * También la cobertura de los triángulos con AA_EN (gfx_draw.aa).
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
static struct gfx_vertex vtx[12] __attribute__((aligned(16)));
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

/* Una tarea con un dibujo de `n` vértices de `vtx` en la color image `fb`
 * (`w` de ancho, 16 bits), de un color y con o sin AA_EN. */
static void task_draw(uint32_t fb, uint32_t w, uint32_t n, uint32_t r, uint32_t g, uint32_t b, uint8_t aa)
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
	d->h.b = n;
	d->aa = aa;
	d->scissor[2] = (uint16_t)(w * 4);
	d->scissor[3] = (uint16_t)(w * 3 / 4 * 4);
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
	res.cmd_bytes = sizeof cmd;  /* el último gfx_cmd queda en 0: GFX_OP_END */
	raster_task(&job, &res);
}

/* Un rectángulo de `x0, y0` a `x1, y1` en dos triángulos, desde vtx[i]. */
static void quad(int i, float x0, float y0, float x1, float y1)
{
	set_vtx(&vtx[i + 0], x0, y0);
	set_vtx(&vtx[i + 1], x1, y0);
	set_vtx(&vtx[i + 2], x0, y1);
	set_vtx(&vtx[i + 3], x1, y0);
	set_vtx(&vtx[i + 4], x1, y1);
	set_vtx(&vtx[i + 5], x0, y1);
}

/* Una tarea que pinta toda la color image `fb` (`w` de ancho, 16 bits). */
static void task_fill_w(uint32_t fb, uint32_t w, uint32_t r, uint32_t g, uint32_t b)
{
	quad(0, 0, 0, (float)w, (float)(w * 3 / 4));
	task_draw(fb, w, 6, r, g, b, 0);
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

/* Unión en T como la del piso y la pared en Dam (GoldenEye): dos franjas
 * separadas por 0,4 píxeles. Con el centro del píxel la fila 20 (centro en
 * 20,5) queda sin pintar; con AA_EN el RDP escribe todo píxel con alguna
 * de sus 8 muestras dentro, y las de y = 20 y 20,25 caen en la franja de
 * arriba. Las franjas quedan además una fila más gordas por abajo. */
static void raster_aa_tjunction(void)
{
	for (uint8_t aa = 0; aa < 2; aa++) {
		raster_reset();
		memset(ram, 0, sizeof ram);
		task_fill(FB_A, 0, 0, 0);
		quad(0, 0, 10, W, 20.3f);
		quad(6, 0, 20.7f, W, 30);
		task_draw(FB_A, W, 12, 255, 255, 255, aa);
		uint32_t w = 0;
		const uint32_t *img = raster_image(FB_A, &w);
		CHECK(img != NULL && w == W);
		if (!img)
			continue;
		CHECK_EQ(img[9 * W + 100], 0);
		CHECK_EQ(img[10 * W + 100], 0xFFFFFF);
		CHECK_EQ(img[20 * W + 100], aa ? 0xFFFFFFu : 0);
		CHECK_EQ(img[21 * W + 100], 0xFFFFFF);
		CHECK_EQ(img[30 * W + 100], aa ? 0xFFFFFFu : 0);
		CHECK_EQ(img[31 * W + 100], 0);
	}
	raster_reset();
}

void t_suite_raster(void)
{
	RUN(raster_frame_mirror_sm64);
	RUN(raster_aa_tjunction);
}
