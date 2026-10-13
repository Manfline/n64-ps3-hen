/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas del muestreo por tiempo del recompilador
 * (cpu/rec_sample.h): la muestra va al bloque o a la categoría, las
 * muestras de un bloque se pliegan por dirección (también cuando el id se
 * reutiliza) y el informe lista los de más muestras con sus palabras.
 */
#include "test.h"

#include <stdio.h>
#include <string.h>

#include "cpu/rec.h"
#include "cpu/rec_sample.h"

static struct salsa64_rec sample_rec;         /* unos 8 MB: fuera de la pila; solo se usan blocks, x.xb y next_block */
static struct salsa64_rec_sample smp;
static struct salsa64_cpu smp_cpu;
static char lines[8][320];
static unsigned nlines;

static void take_line(void *ctx, const char *text)
{
	(void)ctx;
	if (nlines < 8)
		snprintf(lines[nlines], sizeof lines[0], "%s", text);
	nlines++;
}

static void setup(void)
{
	memset(&sample_rec, 0, sizeof sample_rec);
	memset(&smp_cpu, 0, sizeof smp_cpu);
	salsa64_rec_sample_init(&smp);
	sample_rec.sample = &smp;
	sample_rec.next_block = 4;
	sample_rec.blocks[1].n = 12;
	sample_rec.x.xb[1].vpc = 0x7F0BA3B8;
	sample_rec.blocks[2].n = 2;
	sample_rec.x.xb[2].vpc = 0x80001000;
	sample_rec.blocks[3].n = 5;
	sample_rec.x.xb[3].vpc = 0x7F0BA3B8;  /* la misma dirección que el 1, recompilado */
	salsa64_rec_sample_compiled(&smp, 1, 900, 402);
	salsa64_rec_sample_compiled(&smp, 2, 51, 28);
	salsa64_rec_sample_compiled(&smp, 3, 880, 400);
}

static void tick(unsigned mark, unsigned id, unsigned times)
{
	smp_cpu.rec_mark = (uint8_t)mark;
	smp_cpu.rec_cur = (uint16_t)id;
	for (unsigned k = 0; k < times; k++)
		salsa64_rec_sample_tick(&smp, &smp_cpu);
}

static void samples_go_to_block_or_category(void)
{
	setup();
	tick(SALSA64_SMP_NATIVE, 1, 30);
	tick(SALSA64_SMP_NATIVE, 2, 10);
	tick(SALSA64_SMP_NATIVE, 0, 4);  /* bloque recortado: sin id */
	tick(SALSA64_SMP_SLOW, 1, 6);     /* en C: el id no cuenta */
	tick(SALSA64_SMP_INTERP, 0, 5);
	tick(200, 0, 1);                  /* categoría desconocida: «otro» */
	CHECK_EQ(smp.ticks, 56);
	CHECK_EQ(smp.samples[1], 30);
	CHECK_EQ(smp.samples[2], 10);
	CHECK_EQ(smp.unknown, 4);
	CHECK_EQ(smp.marks[SALSA64_SMP_SLOW], 6);
	CHECK_EQ(smp.marks[SALSA64_SMP_INTERP], 5);
	CHECK_EQ(smp.marks[SALSA64_SMP_OTHER], 1);
	CHECK_EQ(smp.marks[SALSA64_SMP_NATIVE], 0);
}

static void fold_merges_by_address(void)
{
	setup();
	tick(SALSA64_SMP_NATIVE, 1, 30);
	tick(SALSA64_SMP_NATIVE, 2, 10);
	salsa64_rec_sample_fold(&sample_rec, 1);  /* el bloque 1 muere */
	CHECK_EQ(smp.samples[1], 0);
	CHECK_EQ(smp.count, 1);
	/* El id 1 se reutiliza para otro bloque: arranca de cero. */
	sample_rec.blocks[1].n = 3;
	sample_rec.x.xb[1].vpc = 0x80002000;
	salsa64_rec_sample_compiled(&smp, 1, 40, 25);
	tick(SALSA64_SMP_NATIVE, 1, 7);
	tick(SALSA64_SMP_NATIVE, 3, 20);  /* la dirección del 1 original, recompilada */
	salsa64_rec_sample_fold_all(&sample_rec);
	CHECK_EQ(smp.count, 3);
	unsigned found = 0;
	for (unsigned k = 0; k < SALSA64_SMP_TABLE; k++) {
		const struct salsa64_rec_sample_entry *e = &smp.table[k];
		if (!e->pc)
			continue;
		found++;
		if (e->pc == 0x7F0BA3B8) {
			CHECK_EQ(e->samples, 50);  /* 30 del id 1 y 20 del id 3 */
			CHECK_EQ(e->words, 880);   /* del último compilado ahí */
			CHECK_EQ(e->hot, 400);
			CHECK_EQ(e->n, 5);
		} else if (e->pc == 0x80001000) {
			CHECK_EQ(e->samples, 10);
			CHECK_EQ(e->words, 51);
		} else {
			CHECK_EQ(e->pc, 0x80002000);
			CHECK_EQ(e->samples, 7);
			CHECK_EQ(e->words, 40);
		}
	}
	CHECK_EQ(found, 3);
	CHECK_EQ(smp.full, 0);
}

static void report_lists_heaviest_blocks(void)
{
	setup();
	tick(SALSA64_SMP_NATIVE, 1, 30);
	tick(SALSA64_SMP_NATIVE, 2, 10);
	tick(SALSA64_SMP_NATIVE, 3, 20);
	tick(SALSA64_SMP_SLOW, 0, 20);
	tick(SALSA64_SMP_EVENTS, 0, 20);
	nlines = 0;
	salsa64_rec_sample_report(&sample_rec, 1, take_line, NULL);
	CHECK_EQ(nlines, 3);  /* resumen, categorías y un bloque */
	CHECK(strstr(lines[0], "100 muestras") != NULL);
	CHECK(strstr(lines[0], "en código generado 60.0 %") != NULL);
	CHECK(strstr(lines[1], "camino lento 20.0 %") != NULL);
	CHECK(strstr(lines[1], "eventos 20.0 %") != NULL);
	CHECK(strstr(lines[2], "bloque 7f0ba3b8: 50 muestras (50.00 % del tiempo, 83.33 % del nativo)") != NULL);
	CHECK(strstr(lines[2], "5 instr., 880 palabras (400 calientes)") != NULL);
	/* El reparto por ventana resta lo anterior. */
	struct salsa64_rec_sample_prev prev;
	memset(&prev, 0, sizeof prev);
	char text[320];
	salsa64_rec_sample_split(&smp, &prev, text, sizeof text);
	CHECK(strstr(text, "100 muestras") != NULL);
	CHECK(strstr(text, "nativo 60.0 %") != NULL);
	tick(SALSA64_SMP_INTERP, 0, 10);
	salsa64_rec_sample_split(&smp, &prev, text, sizeof text);
	CHECK(strstr(text, "10 muestras") != NULL);
	CHECK(strstr(text, "intérprete 100.0") != NULL);
}

void t_suite_rec_sample(void)
{
	RUN(samples_go_to_block_or_category);
	RUN(fold_merges_by_address);
	RUN(report_lists_heaviest_blocks);
}
