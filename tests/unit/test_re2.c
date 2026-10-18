/*
 * Salsa64 — pruebas de la HLE de las tareas de video de Resident Evil 2
 * (src/rsp/jpeg/re2.c): los tres microcódigos, su reconocimiento y su
 * integración en core/rsp.c. La aritmética está medida contra el
 * microcódigo real con salsa64-run --lle-check-jpeg (docs/estado.md); aquí
 * se fijan valores que salieron de esa medición, el CRC de las tareas de
 * prueba del arranque en la PS3 (el SPE tiene que dar el mismo) y que
 * parámetros absurdos no se salen de la RDRAM.
 */
#include "test.h"

#include <stdio.h>
#include <string.h>

#include "core/endian.h"
#include "core/rom.h"
#include "rsp/jpeg/jpeg.h"
#include "rsp/jpeg/re2.h"
#include "rsp/jpeg/selftest.h"

static uint32_t next_rand(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

/* 256 bytes de código con la suma de bytes de cada microcódigo. */
static void ucode_with_sum(uint8_t *p, uint32_t sum)
{
	memset(p, 0, 0x1000);
	for (unsigned i = 0; i < 256 && sum; i++) {
		unsigned v = sum > 255 ? 255 : sum;
		p[i] = (uint8_t)v;
		sum -= v;
	}
}

static void re2_detect(void)
{
	uint8_t u[0x1000];
	ucode_with_sum(u, 0x450f);
	CHECK_EQ(salsa64_re2_detect(u, 256), SALSA64_JPEG_RE2_RESIZE);
	ucode_with_sum(u, 0x3b44);
	CHECK_EQ(salsa64_re2_detect(u, 0x1000), SALSA64_JPEG_RE2_DECODE);
	ucode_with_sum(u, 0x3d84);
	CHECK_EQ(salsa64_re2_detect(u, 0x1000), SALSA64_JPEG_RE2_FILL);
	CHECK(salsa64_jpeg_is_re2(SALSA64_JPEG_RE2_FILL));
	CHECK(!salsa64_jpeg_is_re2(SALSA64_JPEG_OOT));
	ucode_with_sum(u, 0x3d85);
	CHECK_EQ(salsa64_re2_detect(u, 0x1000), SALSA64_JPEG_NONE);
	ucode_with_sum(u, 0x450f);
	CHECK_EQ(salsa64_re2_detect(u, 255), SALSA64_JPEG_NONE);  /* sin los 256 bytes */
}

static uint8_t sram[RE2_SELFTEST_RAM] __attribute__((aligned(16)));

static const enum salsa64_jpeg_kind kinds[RE2_SELFTEST_TASKS] = { SALSA64_JPEG_RE2_RESIZE, SALSA64_JPEG_RE2_DECODE,
								  SALSA64_JPEG_RE2_FILL, SALSA64_JPEG_RE2_FILL };

static void run_all(uint8_t *ram, struct salsa64_jpeg_result res[RE2_SELFTEST_TASKS])
{
	for (unsigned t = 0; t < RE2_SELFTEST_TASKS; t++)
		salsa64_re2_run(kinds[t], ram, RE2_SELFTEST_RAM, re2_selftest_task(t), RE2_SELFTEST_DRAM_STACK,
				RE2_SELFTEST_DRAM_STACK_SIZE, &res[t]);
}

/* Las tareas de prueba del arranque en la PS3: lo que escriben y su CRC (el SPE, el PPE y el host
 * dan lo mismo). */
static void re2_selftest_crc(void)
{
	struct salsa64_jpeg_result res[RE2_SELFTEST_TASKS];
	re2_selftest_build(sram, RE2_SELFTEST_SEED);
	for (unsigned t = 0; t < RE2_SELFTEST_TASKS; t++)
		CHECK(salsa64_re2_supported(kinds[t], sram, sizeof sram, re2_selftest_task(t)));
	run_all(sram, res);
	CHECK(!res[0].bad && !res[1].bad && !res[2].bad && !res[3].bad);
	CHECK_EQ(res[0].macroblocks, 8);
	CHECK_EQ(res[0].mb_out, 72);
	CHECK_EQ(res[0].mb_stride, 80);
	CHECK_EQ(res[0].out_addr, 0x5008);
	CHECK_EQ(res[1].macroblocks, 2);
	CHECK_EQ(res[1].mb_stride, 2112);
	CHECK_EQ(res[1].mb_out, 1056 + 960);
	CHECK_EQ(res[2].macroblocks, 3);
	CHECK_EQ(res[2].mb_stride, 160);
	CHECK_EQ(res[2].mb_out, 80 + 64);
	CHECK_EQ(res[3].macroblocks, 2);
	CHECK_EQ(res[3].mb_stride, 320);
	CHECK_EQ(res[3].mb_out, 160 + 128);
	uint32_t crc = salsa64_crc32(sram, sizeof sram);
	if (crc != RE2_SELFTEST_CRC)
		fprintf(stderr, "re2: CRC de las tareas de prueba %08X (RE2_SELFTEST_CRC dice %08X)\n", crc,
			RE2_SELFTEST_CRC);
	CHECK_EQ(crc, RE2_SELFTEST_CRC);
}

/* FILL: cada byte (los cuatro) es (s + d) >> 1, sin tocar lo que queda entre las filas; con las filas
 * sin alinear (la primera) y alineadas a 16 (la segunda, la del juego). */
static void re2_fill_exact(void)
{
	static uint8_t before[RE2_SELFTEST_RAM];
	struct salsa64_jpeg_result res[RE2_SELFTEST_TASKS];
	static const struct { uint32_t src, dst, len, pitch, advance, iters; } f[2] = {
		{ 0x4008, 0x8000, 64, 80, 160, 3 }, { 0x4400, 0x8400, 128, 160, 320, 2 },
	};
	re2_selftest_build(sram, RE2_SELFTEST_SEED);
	memcpy(before, sram, sizeof sram);
	run_all(sram, res);
	unsigned wrote = 0;
	for (unsigned k = 0; k < 2; k++) {
		for (unsigned it = 0; it < f[k].iters; it++)
			for (unsigned r = 0; r < 2; r++)
				for (unsigned i = 0; i < f[k].len; i++) {
					uint32_t o = it * f[k].advance + r * f[k].pitch + i;
					CHECK_EQ(sram[f[k].dst + o], (before[f[k].src + o] + before[f[k].dst + o]) >> 1);
					wrote++;
				}
		/* Las filas se separan por bytes que no se tocan. */
		for (unsigned it = 0; it < f[k].iters; it++)
			for (unsigned i = f[k].len; i < f[k].pitch; i++)
				CHECK_EQ(sram[f[k].dst + it * f[k].advance + i], before[f[k].dst + it * f[k].advance + i]);
		CHECK_EQ(memcmp(sram + f[k].src, before + f[k].src, f[k].iters * f[k].advance), 0);  /* el origen queda igual */
	}
	CHECK_EQ(wrote, 384 + 512);
}

/* DECODE: valores medidos con el microcódigo real (Y, Cb, Cr → R, G, B), los de los extremos con la
 * cuenta a mano, y el byte de alfa. */
static void re2_decode_exact(void)
{
	static const struct { uint8_t y, cb, cr, r, g, b; } v[] = {
		{ 119, 47, 150, 84, 75, 0 },  { 83, 156, 123, 44, 45, 73 }, { 151, 152, 133, 91, 82, 109 },
		{ 233, 139, 133, 139, 131, 145 }, { 31, 110, 140, 26, 16, 2 },
		{ 128, 128, 128, 74, 74, 74 },  /* ⌊128 · 128 · 38155 / 2^16⌋ = 9538, >> 7 */
		{ 0, 0, 0, 0, 67, 0 },          /* r y b recortados a 0; g = (5851 + 2820) >> 7 */
	};
	struct salsa64_re2_decode t;
	re2_selftest_build(sram, RE2_SELFTEST_SEED);
	CHECK(salsa64_re2_decode_read(sram + (re2_selftest_task(1) & 0xFFFF), &t));
	CHECK_EQ(t.width, 240);
	CHECK_EQ(t.k[0], 0xB375);
	uint8_t y[480], cb[240], cr[240], r0[960], r1[960];
	memset(y, 0, sizeof y);
	for (unsigned i = 0; i < 240; i++)
		cb[i] = cr[i] = 128;
	for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++) {
		y[2 * k] = v[k].y;       /* la fila 0, píxel 2k */
		y[240 + 2 * k + 1] = v[k].y;  /* la fila 1, píxel 2k + 1 */
		cb[k] = v[k].cb;
		cr[k] = v[k].cr;
	}
	salsa64_re2_decode_pair(&t, y, cb, cr, r0, r1);
	for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++) {
		CHECK_EQ(r0[4 * (2 * k) + 0], v[k].r);
		CHECK_EQ(r0[4 * (2 * k) + 1], v[k].g);
		CHECK_EQ(r0[4 * (2 * k) + 2], v[k].b);
		CHECK_EQ(r1[4 * (2 * k + 1) + 0], v[k].r);
		CHECK_EQ(r1[4 * (2 * k + 1) + 1], v[k].g);
		CHECK_EQ(r1[4 * (2 * k + 1) + 2], v[k].b);
	}
	/* El alfa: los bytes 131, 135, … del DMA de Cr en las 28 primeras posiciones de la fila 0, cero
	 * en el resto y en la fila 1. */
	for (unsigned i = 0; i < 240; i++)
		cr[i] = (uint8_t)(i + 1);
	salsa64_re2_decode_pair(&t, y, cb, cr, r0, r1);
	for (unsigned x = 0; x < 240; x++) {
		unsigned a = 0x83 + 4 * x;
		CHECK_EQ(r0[4 * x + 3], a < 240 ? cr[a] : 0);
		CHECK_EQ(r1[4 * x + 3], 0);
	}
	CHECK_EQ(r0[4 * 27 + 3], 0x83 + 4 * 27 + 1);
	CHECK_EQ(r0[4 * 28 + 3], 0);
}

/* RESIZE: un origen liso da la imagen lisa y los extremos (negro y blanco) los de la cuenta; el
 * umbral del difuminado cambia solo el redondeo (de 0 a 7 en vez de 4) y nunca pasa de un paso de 5
 * bits; una fila de 436 píxeles tiene 55 grupos. */
static void re2_resize_flat(void)
{
	struct salsa64_re2_resize t;
	struct salsa64_re2_dither st;
	static uint8_t dm[SALSA64_RE2_DMEM];
	re2_selftest_build(sram, RE2_SELFTEST_SEED);
	CHECK(salsa64_re2_resize_read(sram + (re2_selftest_task(0) & 0xFFFF), &t));
	CHECK_EQ(t.width, 36);
	CHECK_EQ(t.thresh, 5);
	CHECK_EQ(salsa64_re2_resize_src(&t, 0), 0x1000 + 960);  /* y0 empieza en la fila 1 */
	/* Blanco: ⌊255 · peso / 2^16⌋ de cuatro pesos que suman 4 · 16383 · 4 / 2^16 < 1 da 254; el
	 * redondeo lleva a 255 y 5 bits a 31: RGBA5551 = 0xFFFF. */
	memset(dm, 0xFF, 2 * SALSA64_RE2_SRC_ROW);
	salsa64_re2_resize_begin(&st, 0, 0);
	salsa64_re2_resize_row(dm, &t, &st, 0);
	for (unsigned p = 0; p < 36; p++)
		CHECK_EQ(salsa64_rd16(dm + SALSA64_RE2_OUT_BASE + 2 * p), 0xFFFF);
	/* Negro: 0 + 4 → 0 en 5 bits, alfa 1. */
	memset(dm, 0, sizeof dm);
	salsa64_re2_resize_begin(&st, 0, 0);
	salsa64_re2_resize_row(dm, &t, &st, 0);
	for (unsigned p = 0; p < 36; p++)
		CHECK_EQ(salsa64_rd16(dm + SALSA64_RE2_OUT_BASE + 2 * p), 0x0001);
	/* Gris: 128 con los pesos de cada píxel; sin difuminado y con él todos los canales de un píxel
	 * valen lo mismo, y las dos versiones no se separan más de un paso de 5 bits. */
	struct salsa64_re2_resize t0 = t;
	t0.thresh = 0;
	static uint8_t dm0[SALSA64_RE2_DMEM];
	for (unsigned r = 0; r < 4; r++) {
		memset(dm, 128, 2 * SALSA64_RE2_SRC_ROW);
		memset(dm0, 128, 2 * SALSA64_RE2_SRC_ROW);
		salsa64_re2_resize_begin(&st, 0, 0);
		salsa64_re2_resize_row(dm, &t, &st, r);
		struct salsa64_re2_dither st0;
		salsa64_re2_resize_begin(&st0, 0, 0);
		salsa64_re2_resize_row(dm0, &t0, &st0, r);
		for (unsigned p = 0; p < 36; p++) {
			uint32_t a = salsa64_rd16(dm + SALSA64_RE2_OUT_BASE + 2 * p), b = salsa64_rd16(dm0 + SALSA64_RE2_OUT_BASE + 2 * p);
			CHECK_EQ((a >> 11) & 31, (a >> 6) & 31);
			CHECK_EQ((a >> 6) & 31, (a >> 1) & 31);
			CHECK_EQ(a & 1, 1);
			int d = (int)((a >> 11) & 31) - (int)((b >> 11) & 31);
			CHECK(d >= -1 && d <= 1);
		}
	}
}

/* Parámetros absurdos (con las constantes buenas): la HLE solo acepta lo medido y, si lo acepta, no
 * toca nada fuera de la RDRAM (con ASan y UBSan). Y constantes cambiadas: no la hace. */
static void re2_hostile(void)
{
	static uint8_t fuzz[RE2_SELFTEST_RAM] __attribute__((aligned(16)));
	uint32_t s = 20261017u;
	struct salsa64_jpeg_result res;
	unsigned accepted = 0, rejected = 0;
	for (unsigned it = 0; it < 3000; it++) {
		re2_selftest_build(fuzz, RE2_SELFTEST_SEED);
		unsigned which = it % RE2_SELFTEST_TASKS;
		uint32_t at = re2_selftest_task(which) & 0xFFFF;
		/* Palabras de parámetros al azar (a veces valores razonables, a veces cualquier cosa). */
		for (unsigned w = 0; w < 10; w++)
			if (next_rand(&s) % 3 == 0) {
				uint32_t v = next_rand(&s);
				switch (next_rand(&s) % 4) {
				case 0: v = 0; break;
				case 1: v &= 0xFFFF; break;
				case 2: v = 0x80000000u | (v & 0xFFFF8); break;
				default: break;
				}
				salsa64_wr32(fuzz + at + 4 * w, v);
			}
		if (next_rand(&s) % 5 == 0)
			salsa64_wr16(fuzz + at + 0x40 + 2 * (next_rand(&s) % 16), (uint16_t)next_rand(&s));
		bool ok = salsa64_re2_supported(kinds[which], fuzz, sizeof fuzz, 0x80000000u | at);
		ok ? accepted++ : rejected++;
		if (ok)
			salsa64_re2_run(kinds[which], fuzz, sizeof fuzz, 0x80000000u | at, next_rand(&s), next_rand(&s), &res);
	}
	CHECK(accepted > 100);
	CHECK(accepted < 3000);
	CHECK(rejected > 100);
	/* Un segmento de datos fuera de la RDRAM. */
	CHECK(!salsa64_re2_supported(SALSA64_JPEG_RE2_FILL, fuzz, sizeof fuzz, 0x1FFFFFF0u));
	salsa64_re2_run(SALSA64_JPEG_RE2_FILL, fuzz, sizeof fuzz, 0x1FFFFFF0u, 0, 0, &res);
	CHECK(res.bad);
	/* Constantes cambiadas: RESIZE (ancho de fila), DECODE (el cero de la croma no importa, sí la
	 * geometría) y FILL (v10[4]). */
	re2_selftest_build(fuzz, RE2_SELFTEST_SEED);
	CHECK(salsa64_re2_supported(SALSA64_JPEG_RE2_RESIZE, fuzz, sizeof fuzz, re2_selftest_task(0)));
	salsa64_wr16(fuzz + 0x100 + 0x60, 640);
	CHECK(!salsa64_re2_supported(SALSA64_JPEG_RE2_RESIZE, fuzz, sizeof fuzz, re2_selftest_task(0)));
	re2_selftest_build(fuzz, RE2_SELFTEST_SEED);
	salsa64_wr16(fuzz + 0x300 + 0x28, 0x4000);
	CHECK(!salsa64_re2_supported(SALSA64_JPEG_RE2_FILL, fuzz, sizeof fuzz, re2_selftest_task(2)));
	re2_selftest_build(fuzz, RE2_SELFTEST_SEED);
	salsa64_wr32(fuzz + 0x200 + 0x10, 256);  /* ancho de DECODE más grande de lo medido */
	CHECK(!salsa64_re2_supported(SALSA64_JPEG_RE2_DECODE, fuzz, sizeof fuzz, re2_selftest_task(1)));
}

/* ---- Las tareas por el RSP (core/rsp.c) ------------------------------------ */

#define UCODE_PA 0x100000u
#define SP_STATUS_PA (0x04040000u + 4 * SALSA64_SP_STATUS)

static void put_task(unsigned which, uint32_t sum)
{
	re2_selftest_build(t_n64.bus.rdram, RE2_SELFTEST_SEED);
	ucode_with_sum(t_n64.bus.rdram + UCODE_PA, sum);
	uint8_t *d = t_n64.bus.sp_mem + SALSA64_TASK_DMEM;
	memset(d, 0, 64);
	salsa64_wr32(d + 0x00, SALSA64_TASK_GFX);          /* tipo gráfico, como en el juego */
	salsa64_wr32(d + 0x04, 4);
	salsa64_wr32(d + 0x08, 0x80100000u);               /* ucode_boot */
	salsa64_wr32(d + 0x0C, 0xD0);
	salsa64_wr32(d + 0x10, 0x80000000u | UCODE_PA);
	salsa64_wr32(d + 0x14, 0x1000);
	salsa64_wr32(d + 0x18, re2_selftest_task(which));  /* los parámetros van en ucode_data */
	salsa64_wr32(d + 0x1C, 0x80);
	salsa64_wr32(d + 0x20, RE2_SELFTEST_DRAM_STACK);
	salsa64_wr32(d + 0x24, RE2_SELFTEST_DRAM_STACK_SIZE);
	/* data_ptr en cero */
}

static const uint32_t sums[3] = { 0x450f, 0x3b44, 0x3d84 };

static struct {
	int starts, finishes, busy;
	unsigned kind;
	uint32_t data_ptr, stack, stack_size;
} vb;

static void vb_start(void *ctx, const struct salsa64_task *task, unsigned kind)
{
	(void)ctx;
	vb.starts++;
	vb.kind = kind;
	vb.data_ptr = task->data_ptr;
	vb.stack = task->dram_stack;
	vb.stack_size = task->dram_stack_size;
}

static bool vb_done(void *ctx, bool idle)
{
	(void)ctx, (void)idle;
	return vb.busy-- <= 0;
}

static void vb_finish(void *ctx, struct salsa64_jpeg_result *res)
{
	(void)ctx;
	vb.finishes++;
	salsa64_re2_run(vb.kind, t_n64.bus.rdram, t_n64.bus.rdram_size, vb.data_ptr, vb.stack, vb.stack_size, res);
}

static const struct salsa64_jpeg_backend video_backend = { NULL, vb_start, vb_done, vb_finish };

/* La tarea de video llega como una gráfica con un microcódigo sin texto: se reconoce por la suma
 * del código, se hace (en el hilo del CPU o por el backend) y termina con TASKDONE sin
 * interrupción del DP; una tarea con constantes distintas no se hace. */
static void re2_rsp(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	static uint8_t expect[RE2_SELFTEST_RAM];
	static const unsigned rows[RE2_SELFTEST_TASKS] = { 8, 2, 3, 2 };
	static const unsigned sum_of[RE2_SELFTEST_TASKS] = { 0, 1, 2, 2 };
	salsa64_rsp_set_jpeg(bus, NULL);
	for (unsigned which = 0; which < RE2_SELFTEST_TASKS; which++) {
		/* Lo que da la HLE directamente. */
		struct salsa64_jpeg_result res;
		re2_selftest_build(expect, RE2_SELFTEST_SEED);
		salsa64_re2_run(kinds[which], expect, sizeof expect, re2_selftest_task(which), RE2_SELFTEST_DRAM_STACK,
				RE2_SELFTEST_DRAM_STACK_SIZE, &res);
		put_task(which, sums[sum_of[which]]);
		struct salsa64_rsp_stats st0 = bus->rsp.stats;
		salsa64_bus_write32(bus, SP_STATUS_PA, 0x1 | 0x4 | 0x100 | 0x2000, ~0u);
		CHECK_EQ(bus->rsp.busy, 0);
		CHECK(salsa64_bus_read32(bus, SP_STATUS_PA) & SALSA64_SP_STATUS_TASKDONE);
		CHECK_EQ(bus->rsp.stats.video, st0.video + 1);
		CHECK_EQ(bus->rsp.stats.video_rows, st0.video_rows + rows[which]);
		CHECK_EQ(bus->rsp.stats.video_k[kinds[which] - SALSA64_JPEG_RE2_RESIZE],
			 st0.video_k[kinds[which] - SALSA64_JPEG_RE2_RESIZE] + 1);
		CHECK_EQ(bus->rsp.stats.video_bad, st0.video_bad);
		CHECK_EQ(bus->rsp.stats.jpeg, st0.jpeg);
		CHECK_EQ(bus->rsp.stats.unknown_gbi, st0.unknown_gbi);
		CHECK_EQ(bus->rsp.stats.dp_sched, st0.dp_sched);  /* sin interrupción del DP */
		CHECK_EQ(memcmp(bus->rdram, expect, RE2_SELFTEST_RAM), 0);
	}
	/* Por un backend: recibe la tarea con los parámetros en data_ptr y el estado de dram_stack. */
	re2_selftest_build(expect, RE2_SELFTEST_SEED);
	salsa64_re2_run(SALSA64_JPEG_RE2_RESIZE, expect, sizeof expect, re2_selftest_task(0), RE2_SELFTEST_DRAM_STACK,
			RE2_SELFTEST_DRAM_STACK_SIZE, &(struct salsa64_jpeg_result){ 0 });
	put_task(0, sums[0]);
	memset(&vb, 0, sizeof vb);
	vb.busy = 2;
	salsa64_rsp_set_jpeg(bus, &video_backend);
	salsa64_bus_write32(bus, SP_STATUS_PA, 0x1 | 0x4 | 0x100 | 0x2000, ~0u);
	CHECK_EQ(vb.starts, 1);
	CHECK_EQ(vb.kind, SALSA64_JPEG_RE2_RESIZE);
	CHECK_EQ(vb.data_ptr, re2_selftest_task(0));
	CHECK_EQ(vb.stack, RE2_SELFTEST_DRAM_STACK);
	CHECK_EQ(vb.stack_size, RE2_SELFTEST_DRAM_STACK_SIZE);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_JPEG);
	uint64_t t = t_n64.cpu.cycles + bus->rsp.jpeg_cycles;
	for (int i = 0; i < 2; i++, t += bus->rsp.poll_cycles) {
		salsa64_sched_dispatch(&t_n64.sched, t);
		CHECK_EQ(vb.finishes, 0);
	}
	salsa64_sched_dispatch(&t_n64.sched, t);
	CHECK_EQ(vb.finishes, 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK(salsa64_bus_read32(bus, SP_STATUS_PA) & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(memcmp(bus->rdram, expect, RE2_SELFTEST_RAM), 0);
	salsa64_rsp_set_jpeg(bus, NULL);
	/* Constantes que no son las medidas: no se hace (ni se llama al backend) y la tarea termina. */
	put_task(2, sums[2]);
	salsa64_wr16(bus->rdram + 0x300 + 0x28, 0x4000);
	struct salsa64_rsp_stats st1 = bus->rsp.stats;
	memcpy(expect, bus->rdram, RE2_SELFTEST_RAM);
	salsa64_bus_write32(bus, SP_STATUS_PA, 0x1 | 0x4 | 0x100 | 0x2000, ~0u);
	CHECK_EQ(bus->rsp.stats.video, st1.video);
	CHECK_EQ(bus->rsp.stats.video_unsup, st1.video_unsup + 1);
	CHECK_EQ(memcmp(bus->rdram, expect, RE2_SELFTEST_RAM), 0);
	CHECK(salsa64_bus_read32(bus, SP_STATUS_PA) & SALSA64_SP_STATUS_TASKDONE);
}

void t_suite_re2(void)
{
	RUN(re2_detect);
	RUN(re2_selftest_crc);
	RUN(re2_fill_exact);
	RUN(re2_decode_exact);
	RUN(re2_resize_flat);
	RUN(re2_hostile);
	RUN(re2_rsp);
}
