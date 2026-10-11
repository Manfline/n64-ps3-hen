/*
 * Salsa64 — pruebas de la HLE de la tarea JPEG (src/rsp/jpeg): macrobloques
 * armados a mano en la RDRAM y su salida RGBA5551 en el mismo lugar.
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/jpeg/jpeg.h"

#define TASK_PA 0x40000u  /* estructura de la tarea */
#define Q_PA    0x40100u  /* tres tablas de cuantización */
#define MB_PA   0x41000u  /* macrobloques */

static struct salsa64_jpeg_result jres;

static uint8_t *ram(uint32_t pa)
{
	return t_n64.bus.rdram + pa;
}

/* Tablas de cuantización todas a q y macrobloques en cero. */
static void setup(int16_t q)
{
	for (int t = 0; t < 3; t++)
		for (int i = 0; i < 64; i++)
			salsa64_wr16(ram(Q_PA + 128 * t + 2 * i), (uint16_t)q);
	memset(ram(MB_PA), 0, 16 * 768);
}

/* Coeficiente `zz` (orden zigzag) del bloque b del macrobloque m. */
static void coef(uint32_t mode, int m, int b, int zz, int16_t v)
{
	uint32_t mb_bytes = (mode + 4) * 128;
	salsa64_wr16(ram(MB_PA + m * mb_bytes + b * 128 + 2 * zz), (uint16_t)v);
}

static void run_task(uint32_t mode, uint32_t count, uint32_t addr)
{
	uint8_t *d = ram(TASK_PA);
	salsa64_wr32(d, 0x80000000u | addr);
	salsa64_wr32(d + 4, count);
	salsa64_wr32(d + 8, mode);
	for (int t = 0; t < 3; t++)
		salsa64_wr32(d + 12 + 4 * t, 0x80000000u | (Q_PA + 128 * t));
	salsa64_jpeg_run(SALSA64_JPEG_OOT, t_n64.bus.rdram, t_n64.bus.rdram_size, 0x80000000u | TASK_PA, &jres);
}

static uint16_t px(uint32_t mb_addr, int x, int y)
{
	return salsa64_rd16(ram(mb_addr + 32 * y + 2 * x));
}

static uint16_t gray(unsigned v)
{
	return (uint16_t)(v << 11 | v << 6 | v << 1 | 1);
}

/* Solo DC, modo 2 (4:2:0): cada bloque Y es un cuadrante de 8 × 8 del
 * macrobloque de 16 × 16 (Y0 Y1 arriba, Y2 Y3 abajo). DC c con
 * cuantización 1: c·16/8 = 2c dieciseisavos sobre el gris 2048. */
static void jpeg_dc_quadrants(void)
{
	setup(1);
	coef(2, 0, 1, 0, 400);   /* 2048 + 800 = 0xB20: 5 bits = 22 */
	coef(2, 0, 2, 0, -400);  /* 1248 = 0x4E0: 9 */
	coef(2, 0, 3, 0, 800);   /* 3648 = 0xE40: 28 */
	run_task(2, 1, MB_PA);
	CHECK(!jres.bad);
	CHECK_EQ(jres.macroblocks, 1);
	CHECK_EQ(jres.out_addr, MB_PA);
	CHECK_EQ(jres.mb_stride, 768);
	CHECK_EQ(jres.mb_out, 512);
	static const unsigned v[4] = { 16, 22, 9, 28 };
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
			CHECK_EQ(px(MB_PA, x, y), gray(v[(y >= 8) * 2 + (x >= 8)]));
	/* Lo que sigue a los 512 bytes de salida queda como estaba (U y V). */
	CHECK_EQ(salsa64_rd16(ram(MB_PA + 512)), 0);
}

/* Croma, modo 0 (4:2:2: Y0 Y1 U V, 16 × 8): DC de U = 200 (400
 * dieciseisavos): B = 2048 + 1,7729·400 → 21, G = 2048 − ⌊0,3443·400⌋ →
 * 14, R = 16. */
static void jpeg_chroma_mode0(void)
{
	setup(1);
	coef(0, 0, 2, 0, 200);
	run_task(0, 1, MB_PA);
	CHECK_EQ(jres.mb_stride, 512);
	CHECK_EQ(jres.mb_out, 256);
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 16; x++)
			CHECK_EQ(px(MB_PA, x, y), (uint16_t)(16 << 11 | 14 << 6 | 21 << 1 | 1));

	/* DC de V = −100 (−200 dieciseisavos). */
	setup(1);
	coef(0, 0, 3, 0, -100);
	run_task(0, 1, MB_PA);
	/* R = 2048 − 280,5 = 1767 → 0x6E7 → 13; G = 2048 − ⌊−142,88⌋ = 2191 →
	 * 0x88F → 17; B = 16. */
	CHECK_EQ(px(MB_PA, 5, 3), (uint16_t)(13 << 11 | 17 << 6 | 16 << 1 | 1));
}

/* Zigzag: el coeficiente 1 es la primera frecuencia horizontal (cambia de
 * columna en columna, igual en cada fila) y el 2 la primera vertical. */
static void jpeg_zigzag_orientation(void)
{
	setup(1);
	coef(2, 0, 0, 1, 300);
	run_task(2, 1, MB_PA);
	for (int y = 1; y < 8; y++)
		for (int x = 0; x < 8; x++)
			CHECK_EQ(px(MB_PA, x, y), px(MB_PA, x, 0));
	CHECK(((px(MB_PA, 0, 0) >> 11) & 31) > ((px(MB_PA, 7, 0) >> 11) & 31));

	setup(1);
	coef(2, 0, 0, 2, 300);
	run_task(2, 1, MB_PA);
	for (int y = 0; y < 8; y++)
		for (int x = 1; x < 8; x++)
			CHECK_EQ(px(MB_PA, x, y), px(MB_PA, 0, y));
	CHECK(((px(MB_PA, 0, 0) >> 11) & 31) > ((px(MB_PA, 0, 7) >> 11) & 31));
}

/* Referencia en doble precisión (IDCT directa) de un bloque Y con
 * cuantización q, comparada con la HLE en un macrobloque de modo 2 con
 * coeficientes pseudoaleatorios y croma neutra. */
static void jpeg_idct_reference(void)
{
	static const uint8_t zz[64] = {
		0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
		3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
		10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
		21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
	};
	setup(3);
	int16_t c[64];
	uint32_t seed = 12345;
	for (int i = 0; i < 64; i++) {
		seed = seed * 1103515245u + 12345u;
		int r = (int)((seed >> 16) % 61) - 30;
		c[i] = (int16_t)(i == 0 ? 50 : i < 20 ? r : r / 6);
		coef(2, 0, 0, i, c[i]);
	}
	run_task(2, 1, MB_PA);
	int exact = 0, close = 0;
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			double s = 0.0;
			for (int v = 0; v < 8; v++)
				for (int u = 0; u < 8; u++) {
					double cu = u ? 1.0 : sqrt(0.5), cv = v ? 1.0 : sqrt(0.5);
					double f = (double)(c[zz[v * 8 + u]] * 3 * 16);
					s += 0.25 * cu * cv * f * cos((2 * x + 1) * u * M_PI / 16) * cos((2 * y + 1) * v * M_PI / 16);
				}
			double level = 2048.0 + (double)lrint(s);
			int ref = level < 0 ? 0 : level > 0xFF0 ? 0x1F : ((int)level & 0xF80) >> 7;
			int got = (px(MB_PA, x, y) >> 11) & 31;
			exact += got == ref;
			close += got - ref <= 1 && ref - got <= 1;
		}
	CHECK(exact >= 62);
	CHECK_EQ(close, 64);
}

/* Varios macrobloques: cada uno en su lugar (paso de 768 bytes en modo 2);
 * parámetros malos no escriben nada. */
static void jpeg_count_and_bad(void)
{
	setup(1);
	for (int m = 0; m < 3; m++)
		coef(2, m, 0, 0, (int16_t)(100 * m));
	run_task(2, 3, MB_PA);
	CHECK_EQ(jres.macroblocks, 3);
	CHECK_EQ(jres.out_bytes, 2 * 768 + 512);
	CHECK_EQ(px(MB_PA + 768, 0, 0), gray((2048 + 200) >> 7));
	CHECK_EQ(px(MB_PA + 1536, 0, 0), gray((2048 + 400) >> 7));

	setup(1);
	run_task(1, 1, MB_PA);  /* modo 1: no existe */
	CHECK(jres.bad);
	CHECK_EQ(jres.macroblocks, 0);
	CHECK_EQ(salsa64_rd16(ram(MB_PA)), 0);
	run_task(2, 1, t_n64.bus.rdram_size - 256);  /* se sale de la RDRAM */
	CHECK(jres.bad);
	CHECK_EQ(jres.macroblocks, 0);

	/* Un microcódigo que no es el de OoT no se reconoce. */
	uint8_t junk[0x1000];
	memset(junk, 0x5A, sizeof junk);
	CHECK_EQ(salsa64_jpeg_detect(junk, sizeof junk), SALSA64_JPEG_NONE);
	CHECK_EQ(salsa64_jpeg_detect(junk, 0x800), SALSA64_JPEG_NONE);
}

void t_suite_jpeg(void)
{
	RUN(jpeg_dc_quadrants);
	RUN(jpeg_chroma_mode0);
	RUN(jpeg_zigzag_orientation);
	RUN(jpeg_idct_reference);
	RUN(jpeg_count_and_bad);
}
