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

/* Lo mismo con un dibujo normal (1 ciclo) en vez de un relleno. */
static void task_tall_draw(uint32_t fb, uint32_t w, uint32_t lh, float x0, float y0, float x1, float y1, uint32_t r,
			   uint32_t g, uint32_t b)
{
	task_tall_fill(fb, w, lh, x0, y0, x1, y1, r, g, b);  /* arma el comando... */
	((struct gfx_draw *)(cmd + sizeof(struct gfx_cmd)))->cycle = 0;  /* ...y se repite como 1 ciclo */
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
	/* Donkey Kong 64: 459 filas sobre dos búferes (más de pantalla y
	 * media): también se reparte, sin crear un destino de 320 × 459. */
	task_tall_fill(FB_A, W, 459, 0, 0, (float)W, 459, 9, 8, 7);
	CHECK_EQ(a[0], 0x090807u);
	CHECK_EQ(b[218 * W + 5], 0x090807u);
	CHECK_EQ(b[230 * W + 5], 0x28323cu);
	uint32_t bw = 0;
	CHECK(raster_image(FB_A, &bw) == a);  /* sigue siendo el de 240 */
	CHECK(raster_image(FB_A + 250 * LINE, &bw) == b);  /* no hay un destino de 459 */
	/* Con un dibujo de verdad (la transición del cierre de DK64, que el CPU
	 * lee después) la imagen de 320 × 459 sí se crea tal cual. */
	task_tall_draw(FB_A, W, 459, 0, 0, (float)W, 459, 100, 110, 120);
	const uint32_t *t = raster_image(FB_A + 250 * LINE, &bw);
	CHECK(t && t != b);
	raster_reset();
}

/* Un dibujo de 1 ciclo de (x0, y0)–(x1, y1) sobre la color image `fb`
 * (320 de ancho) con tijeras sc (píxeles enteros). */
static void task_scissor(uint32_t fb, float x0, float y0, float x1, float y1, const uint16_t sc[4], uint32_t r,
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
	t->b = W;
	t->c = 2;
	struct gfx_draw *d = (struct gfx_draw *)(cmd + sizeof *t);
	d->h.op = GFX_OP_DRAW;
	d->h.size = sizeof *d;
	d->h.a = 0;
	d->h.b = 6;
	for (int k = 0; k < 4; k++)
		d->scissor[k] = (uint16_t)(sc[k] * 4);
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

/* Datos del juego (medias palabras pares: el bit del alfa en 0). */
static uint16_t game_word(uint32_t x, uint32_t y)
{
	return (uint16_t)(0x8000 | ((x * 7 + y * 13) & 0x3FFF) << 1);
}

/* ¿Quedó intacto todo lo que está fuera de [x0, x1) × [y0, y1) en las
 * primeras `rows` filas de `fb`? */
static int outside_intact(uint32_t fb, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t rows)
{
	for (uint32_t y = 0; y < rows; y++)
		for (uint32_t x = 0; x < W; x++)
			if ((x < x0 || x >= x1 || y < y0 || y >= y1) && px(fb, x, y) != game_word(x, y))
				return 0;
	return 1;
}

/* Super Smash Bros., en el KO: con su z-buffer como color image (320 de
 * ancho, tijeras 10..310 × 10..230) dibuja un rectángulo de toda la
 * pantalla y una malla encima. El juego guarda datos en los márgenes del
 * z-buffer, fuera de las tijeras: la copia a la RDRAM de filas enteras les
 * ponía a 1 el bit del alfa y el juego se colgaba. Solo se copia lo que el
 * RDP pudo escribir. Lo mismo para el espejo de un framebuffer. */
static void raster_copy_inside_scissor(void)
{
	static const uint16_t sc[4] = { 10, 10, 310, 230 };
	int fbfx = raster_fbfx;
	raster_reset();
	raster_fbfx = 2;
	memset(ram, 0, sizeof ram);
	for (int k = 0; k < 40; k++)
		task_fill_w(0x1000, 64, 0, 0, 0);
	task_fill(FB_A, 255, 0, 0);
	field(FB_A);
	field(FB_A);
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			salsa64_wr16(ram + FB_C + (y * W + x) * 2, game_word(x, y));
			salsa64_wr16(ram + FB_A + (y * W + x) * 2, game_word(x, y));
		}
	/* FB_C no es un framebuffer del VI: copia esperada a la RDRAM. */
	task_scissor(FB_C, 0, 0, (float)W, (float)H, sc, 0, 0, 0);
	CHECK_EQ(px(FB_C, 10, 10), rgba5551(0, 0, 0));
	CHECK_EQ(px(FB_C, 309, 229), rgba5551(0, 0, 0));
	CHECK(outside_intact(FB_C, 10, 10, 310, 230, H));
	/* Una malla chica: solo su caja (y lo de antes sigue en la RDRAM). */
	task_scissor(FB_C, 100.0f, 50.0f, 120.5f, 60.0f, sc, 255, 255, 255);
	CHECK_EQ(px(FB_C, 110, 55), rgba5551(255, 255, 255));
	CHECK_EQ(px(FB_C, 20, 20), rgba5551(0, 0, 0));
	CHECK(outside_intact(FB_C, 10, 10, 310, 230, H));
	/* El framebuffer (ya mostrado: espejo al terminar la tarea). */
	task_scissor(FB_A, 0, 0, (float)W, (float)H, sc, 0, 255, 0);
	CHECK_EQ(px(FB_A, 10, 10), rgba5551(0, 255, 0));
	CHECK(outside_intact(FB_A, 10, 10, 310, 230, H));
	raster_reset();
	raster_fbfx = fbfx;
}

/* Star Fox 64 al arrancar (el ruido de colores del logo de «Nintendo
 * presents»): el FILLRECT de 320 × 720 que limpia los tres framebuffers
 * (y su borde de 8 px, que el juego no vuelve a tocar) llega en la primera
 * tarea, cuando aún no existe ninguno; los destinos nacen después, uno por
 * tarea, con un rectángulo de 8..312 × 8..232, y se ceban con la RDRAM,
 * donde quedan los datos que el juego cargó antes por DMA en el primero. El
 * borde tiene que salir con el color del relleno; si un DMA tocó el destino
 * después del relleno, manda lo que el juego dejó. */
static void raster_tall_fill_first(void)
{
	static const uint16_t inner[4] = { 0, 0, (uint16_t)W, (uint16_t)H };
	int fbfx = raster_fbfx;
	raster_fbfx = 2;
	raster_reset();
	memset(ram, 0, sizeof ram);
	/* Los datos del DMA de arranque: filas 0–31 del primer framebuffer. */
	for (uint32_t y = 0; y < 32; y++)
		for (uint32_t x = 0; x < W; x++)
			salsa64_wr16(ram + FB_A + (y * W + x) * 2, game_word(x, y));
	raster_rdram_written(FB_A, 32 * LINE, 0);
	task_tall_fill(FB_A, W, 3 * H, 0, 0, (float)W, 3.0f * H, 40, 50, 60);
	task_scissor(FB_B, 8, 8, 312, 232, inner, 255, 0, 0);
	task_scissor(FB_C, 8, 8, 312, 232, inner, 0, 255, 0);
	task_scissor(FB_A, 8, 8, 312, 232, inner, 0, 0, 255);
	uint32_t w = 0;
	const uint32_t *a = raster_image(FB_A, &w), *b = raster_image(FB_B, &w), *c = raster_image(FB_C, &w);
	CHECK(a && b && c);
	if (a && b && c) {
		CHECK_EQ(a[0], 0x28323cu);
		CHECK_EQ(a[5 * W + 3], 0x28323cu);
		CHECK_EQ(a[20 * W + 315], 0x28323cu);
		CHECK_EQ(a[100 * W + 100], 0x0000ffu);
		CHECK_EQ(b[0], 0x28323cu);
		CHECK_EQ(b[(H - 1) * W + 4], 0x28323cu);
		CHECK_EQ(b[100 * W + 100], 0xff0000u);
		CHECK_EQ(c[3 * W + 3], 0x28323cu);
		CHECK_EQ(c[100 * W + 100], 0x00ff00u);
	}
	/* Un destino que nace mucho después no recibe el relleno viejo. */
	raster_reset();
	memset(ram, 0, sizeof ram);
	task_tall_fill(FB_A, W, 3 * H, 0, 0, (float)W, 3.0f * H, 40, 50, 60);
	for (int k = 0; k < 40; k++)
		task_fill_w(0x1000, 64, 0, 0, 0);
	task_scissor(FB_B, 8, 8, 312, 232, inner, 255, 0, 0);
	b = raster_image(FB_B, &w);
	CHECK(b && b[0] == 0u);
	/* Con un DMA posterior al relleno en la memoria del destino, manda el DMA. */
	raster_reset();
	memset(ram, 0, sizeof ram);
	for (uint32_t y = 0; y < 4; y++)
		for (uint32_t x = 0; x < W; x++)
			salsa64_wr16(ram + FB_B + (y * W + x) * 2, rgba5551(200, 100, 50));
	task_tall_fill(FB_A, W, 3 * H, 0, 0, (float)W, 3.0f * H, 40, 50, 60);
	raster_rdram_written(FB_B, 4 * LINE, 1);
	task_scissor(FB_B, 8, 8, 312, 232, inner, 255, 0, 0);
	task_scissor(FB_C, 8, 8, 312, 232, inner, 0, 255, 0);
	b = raster_image(FB_B, &w);
	c = raster_image(FB_C, &w);
	CHECK(b && c);
	if (b && c) {
		CHECK_EQ(b[3 * W + 3], 0xce6331u);  /* (200, 100, 50) de la RDRAM, a 8 bits */
		CHECK_EQ(c[3 * W + 3], 0x28323cu);  /* sin DMA: el relleno */
	}
	raster_reset();
	raster_fbfx = fbfx;
}


/* Pokémon Stadium al elegir VS: una sola tarea con nueve color images chicas
 * (de 172 de ancho; la tabla de destinos tiene ocho ranuras), pegadas una a
 * otra en la RDRAM. La novena recicla la ranura de la más vieja, que dibujó
 * más filas: la caja de lo dibujado era la de la anterior y la copia a la
 * RDRAM pisaba las filas de más de la novena, que son datos del juego (los
 * nodos del planificador de eventos; el juego saltaba a un callback pisado y
 * se quedaba en su pantalla de error). La ranura reciclada copia antes lo
 * dibujado de la imagen vieja a su dirección y empieza sin su caja. */
static void raster_recycled_slot(void)
{
	enum { N = 9, SW = 172, FULL = 60, LAST = 20, GAP = SW * 2 * FULL };
	static struct gfx_vertex v9[N * 6] __attribute__((aligned(16)));
	static uint8_t c9[N * (sizeof(struct gfx_cmd) + sizeof(struct gfx_draw)) + sizeof(struct gfx_cmd)]
		__attribute__((aligned(16)));
	static uint8_t big[0x60000] __attribute__((aligned(16)));
	const uint32_t base = 0x20000;
	int fbfx = raster_fbfx;
	raster_reset();
	raster_fbfx = 2;
	memset(big, 0, sizeof big);
	memset(c9, 0, sizeof c9);
	uint32_t off = 0;
	for (unsigned k = 0; k < N; k++) {
		uint32_t rows = k == N - 1 ? LAST : FULL;
		struct gfx_cmd *t = (struct gfx_cmd *)(c9 + off);
		t->op = GFX_OP_TARGET;
		t->size = sizeof *t;
		t->a = base + k * GAP;
		t->b = SW;
		t->c = 2;
		off += sizeof *t;
		struct gfx_draw *d = (struct gfx_draw *)(c9 + off);
		d->h.op = GFX_OP_DRAW;
		d->h.size = sizeof *d;
		d->h.a = k * 6;
		d->h.b = 6;
		d->scissor[2] = SW * 4;
		d->scissor[3] = (uint16_t)(rows * 4);
		d->k[GFX_K_ONE][0][0] = 1.0f;
		d->k[GFX_K_ONE][0][3] = 1.0f;
		set_vtx(&v9[k * 6 + 0], 0, 0);
		set_vtx(&v9[k * 6 + 1], (float)SW, 0);
		set_vtx(&v9[k * 6 + 2], 0, (float)rows);
		set_vtx(&v9[k * 6 + 3], (float)SW, 0);
		set_vtx(&v9[k * 6 + 4], (float)SW, (float)rows);
		set_vtx(&v9[k * 6 + 5], 0, (float)rows);
		off += sizeof *d;
	}
	/* Los datos del juego tras las primeras filas de la novena imagen. */
	uint32_t last = base + (N - 1) * GAP;
	for (uint32_t y = LAST; y < FULL; y++)
		for (uint32_t x = 0; x < SW; x++)
			salsa64_wr16(big + last + (y * SW + x) * 2, game_word(x, y));
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	memset(&res, 0, sizeof res);
	job.rdram_ea = (uint64_t)(uintptr_t)big;
	job.rdram_size = sizeof big;
	job.vtx_ea = (uint64_t)(uintptr_t)v9;
	job.cmd_ea = (uint64_t)(uintptr_t)c9;
	res.cmd_bytes = off + sizeof(struct gfx_cmd);
	raster_task(&job, &res);
	/* Lo dibujado de la novena sale en sus 20 filas... */
	CHECK_EQ(salsa64_rd16(big + last + (5 * SW + 10) * 2), rgba5551(255, 0, 0));
	/* La primera imagen (la ranura que se recicló) llegó a la RDRAM antes de
	 * que la ranura cambiara de dueña, con su dirección y sus 60 filas. */
	CHECK_EQ(salsa64_rd16(big + base + (30 * SW + 10) * 2), rgba5551(255, 0, 0));
	CHECK_EQ(salsa64_rd16(big + base + (59 * SW + 171) * 2), rgba5551(255, 0, 0));
	/* ...y el resto de su memoria sigue siendo del juego. */
	unsigned bad = 0;
	for (uint32_t y = LAST; y < FULL; y++)
		for (uint32_t x = 0; x < SW; x++)
			bad += salsa64_rd16(big + last + (y * SW + x) * 2) != game_word(x, y);
	CHECK_EQ(bad, 0);
	raster_reset();
	raster_fbfx = fbfx;
}

void t_suite_raster(void)
{
	RUN(raster_recycled_slot);
	RUN(raster_copy_inside_scissor);
	RUN(raster_tall_fill);
	RUN(raster_tall_fill_first);
	RUN(raster_mirror_after_dma);
	RUN(raster_frame_mirror_sm64);
}
