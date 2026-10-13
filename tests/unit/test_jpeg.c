/*
 * Salsa64 — pruebas de la HLE de la tarea JPEG (src/rsp/jpeg): macrobloques
 * armados a mano en la RDRAM y su salida RGBA5551 en el mismo lugar.
 */
#include "test.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/endian.h"
#include "core/rom.h"
#include "rsp/jpeg/jpeg.h"
#include "rsp/jpeg/selftest.h"

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

/* Solo DC, en todo el rango de un JPEG (±2048 niveles de 8 bits): la IDCT
 * entera da exactamente 2·DC dieciseisavos, como la exacta. */
static void jpeg_dc_exact(void)
{
	static const int16_t q[3][64] = { { 1 }, { 1 }, { 1 } };
	uint8_t mb[768], out[512];
	for (int dc = -1024; dc < 1024; dc++) {
		memset(mb, 0, sizeof mb);
		salsa64_wr16(mb, (uint16_t)(int16_t)dc);  /* Y0; U y V en 0 */
		salsa64_jpeg_mb(2, q, mb, out);
		int level = 2048 + 2 * dc;
		unsigned v = level < 0 ? 0 : level > 0xFF0 ? 31 : (unsigned)(level & 0xF80) >> 7;
		CHECK_EQ(salsa64_rd16(out), gray(v));
		CHECK_EQ(salsa64_rd16(out + 32 * 7 + 14), gray(v));
	}
}

/* Las tareas de prueba del arranque en la PS3 (rsp/jpeg/selftest.c): todos
 * los macrobloques, en su lugar (el macrobloque por separado da lo mismo
 * que la tarea), y el CRC que deben dar también el PPE y el SPE. */
static void jpeg_selftest_crc(void)
{
	static uint8_t a[JPEG_SELFTEST_RAM], b[JPEG_SELFTEST_RAM];
	jpeg_selftest_build(a, JPEG_SELFTEST_SEED);
	memcpy(b, a, sizeof b);
	for (uint32_t t = 0; t < 2; t++) {
		salsa64_jpeg_run(SALSA64_JPEG_OOT, a, sizeof a, jpeg_selftest_task(t), &jres);
		CHECK(!jres.bad);
		CHECK_EQ(jres.macroblocks, JPEG_SELFTEST_MBS);
		/* Lo mismo macrobloque por macrobloque, a otro búfer. */
		struct salsa64_jpeg_task task;
		CHECK(salsa64_jpeg_task_read(b + (jpeg_selftest_task(t) & 0xFFFF), &task));
		int16_t q[3][64];
		for (int i = 0; i < 3; i++)
			salsa64_jpeg_qtable(b + task.qaddr[i], q[i]);
		for (uint32_t m = 0; m < task.count; m++) {
			uint32_t at = task.addr + m * SALSA64_JPEG_MB_IN(task.mode);
			uint8_t out[512];
			salsa64_jpeg_mb(task.mode, q, b + at, out);
			memcpy(b + at, out, SALSA64_JPEG_MB_OUT(task.mode));
		}
	}
	CHECK(memcmp(a, b, sizeof a) == 0);
	uint32_t crc = salsa64_crc32(a, sizeof a);
	if (crc != JPEG_SELFTEST_CRC)
		fprintf(stderr, "jpeg: CRC de las tareas de prueba %08X (JPEG_SELFTEST_CRC dice %08X)\n", crc,
			JPEG_SELFTEST_CRC);
	CHECK_EQ(crc, JPEG_SELFTEST_CRC);
}

/* ---- La tarea JPEG por el RSP (core/rsp.c) -------------------------------- */

#define UCODE_PA 0x100000u
#define SP_STATUS_PA (0x04040000u + 4 * SALSA64_SP_STATUS)

/* Un «microcódigo» de 4 KB en cero salvo sus últimos 4 bytes, elegidos para
 * que su CRC-32 sea el del de OoT (0x761D8504): rsp.c lo toma por el JPEG
 * de OoT sin tener nada del juego. */
static void put_jpeg_task(void)
{
	memset(ram(UCODE_PA), 0, 0x1000);
	salsa64_wr32(ram(UCODE_PA + 0xFFC), 0x03EE3280u);
	uint8_t *d = t_n64.bus.sp_mem + SALSA64_TASK_DMEM;
	memset(d, 0, 64);
	salsa64_wr32(d + 0x00, SALSA64_TASK_JPEG);
	salsa64_wr32(d + 0x08, 0x80100000u);         /* ucode_boot */
	salsa64_wr32(d + 0x0C, 0xD0);
	salsa64_wr32(d + 0x10, 0x80000000u | UCODE_PA);
	salsa64_wr32(d + 0x14, 0x1000);
	salsa64_wr32(d + 0x18, 0x80000000u | (UCODE_PA + 0x1000));
	salsa64_wr32(d + 0x1C, 0x800);
	salsa64_wr32(d + 0x30, 0x80000000u | TASK_PA);
	salsa64_wr32(d + 0x34, 0x20);
	uint8_t *t = ram(TASK_PA);
	salsa64_wr32(t, 0x80000000u | MB_PA);
	salsa64_wr32(t + 4, 2);
	salsa64_wr32(t + 8, 2);
	for (int i = 0; i < 3; i++)
		salsa64_wr32(t + 12 + 4 * i, 0x80000000u | (Q_PA + 128 * i));
}

/* Backend de mentira (el SPE en la PS3): `done` dice que no las primeras
 * veces; `finish` corre la HLE sobre la RDRAM. */
static struct {
	int starts, dones, finishes, busy;
	unsigned kind;
	uint32_t data_ptr;
} jb;

static void jb_start(void *ctx, const struct salsa64_task *task, unsigned kind)
{
	(void)ctx;
	jb.starts++;
	jb.kind = kind;
	jb.data_ptr = task->data_ptr;
}

static bool jb_done(void *ctx, bool idle)
{
	(void)ctx, (void)idle;
	jb.dones++;
	return jb.busy-- <= 0;
}

static void jb_finish(void *ctx, struct salsa64_jpeg_result *res)
{
	(void)ctx;
	jb.finishes++;
	salsa64_jpeg_run(jb.kind, t_n64.bus.rdram, t_n64.bus.rdram_size, jb.data_ptr, res);
}

static const struct salsa64_jpeg_backend jpeg_backend = { NULL, jb_start, jb_done, jb_finish };

/* Sin backend la tarea se hace en el acto (TASKDONE al arrancar); con
 * backend termina por sondeo, el CPU sigue mientras tanto y la RDRAM queda
 * igual que en el hilo del CPU. Un estado guardado la recoge antes. */
static void jpeg_rsp_backend(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	static uint8_t sync_out[2 * 768];
	setup(2);
	coef(2, 0, 0, 0, 300);
	coef(2, 1, 4, 3, -40);
	put_jpeg_task();
	salsa64_rsp_set_jpeg(bus, NULL);
	struct salsa64_rsp_stats st0 = bus->rsp.stats;
	salsa64_bus_write32(bus, SP_STATUS_PA, 0x1 | 0x4 | 0x100 | 0x2000, ~0u);  /* HALT, BROKE y TASKDONE fuera */
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK(salsa64_bus_read32(bus, SP_STATUS_PA) & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->rsp.stats.jpeg, st0.jpeg + 1);
	CHECK_EQ(bus->rsp.stats.jpeg_mb, st0.jpeg_mb + 2);
	memcpy(sync_out, ram(MB_PA), sizeof sync_out);

	setup(2);
	coef(2, 0, 0, 0, 300);
	coef(2, 1, 4, 3, -40);
	put_jpeg_task();
	memset(&jb, 0, sizeof jb);
	jb.busy = 2;
	salsa64_rsp_set_jpeg(bus, &jpeg_backend);
	salsa64_bus_write32(bus, SP_STATUS_PA, 0x1 | 0x4 | 0x100 | 0x2000, ~0u);
	CHECK_EQ(jb.starts, 1);
	CHECK_EQ(jb.kind, SALSA64_JPEG_OOT);
	CHECK_EQ(jb.data_ptr, 0x80000000u | TASK_PA);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_JPEG);
	CHECK_EQ(salsa64_bus_read32(bus, SP_STATUS_PA) & (SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_TASKDONE), 0);
	uint64_t t = t_n64.cpu.cycles + bus->rsp.jpeg_cycles;
	for (int i = 0; i < 2; i++, t += bus->rsp.poll_cycles) {
		salsa64_sched_dispatch(&t_n64.sched, t);
		CHECK_EQ(jb.finishes, 0);
		CHECK_EQ(bus->rsp.busy, SALSA64_TASK_JPEG);
	}
	salsa64_sched_dispatch(&t_n64.sched, t);
	CHECK_EQ(jb.finishes, 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK(salsa64_bus_read32(bus, SP_STATUS_PA) & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->rsp.stats.jpeg, st0.jpeg + 2);
	CHECK_EQ(bus->rsp.stats.jpeg_mb, st0.jpeg_mb + 4);
	CHECK(memcmp(sync_out, ram(MB_PA), sizeof sync_out) == 0);

	/* Estado guardado con la tarea en vuelo: se recoge y su fin llega a su
	 * hora sin volver a llamar al backend. */
	put_jpeg_task();
	memset(&jb, 0, sizeof jb);
	jb.busy = 100;
	salsa64_bus_write32(bus, SP_STATUS_PA, 0x1 | 0x4 | 0x100 | 0x2000, ~0u);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_JPEG);
	CHECK(salsa64_rsp_quiesce(bus));
	CHECK_EQ(jb.finishes, 1);
	salsa64_sched_dispatch(&t_n64.sched, t_n64.cpu.cycles + bus->rsp.jpeg_cycles);
	CHECK_EQ(jb.finishes, 1);
	CHECK_EQ(jb.dones, 0);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK(salsa64_bus_read32(bus, SP_STATUS_PA) & SALSA64_SP_STATUS_TASKDONE);
	salsa64_rsp_set_jpeg(bus, NULL);
}

void t_suite_jpeg(void)
{
	RUN(jpeg_dc_quadrants);
	RUN(jpeg_chroma_mode0);
	RUN(jpeg_zigzag_orientation);
	RUN(jpeg_idct_reference);
	RUN(jpeg_count_and_bad);
	RUN(jpeg_dc_exact);
	RUN(jpeg_selftest_crc);
	RUN(jpeg_rsp_backend);
}
