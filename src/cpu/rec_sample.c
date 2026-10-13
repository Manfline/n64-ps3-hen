/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — muestreo por tiempo del recompilador (rec_sample.h). */
#include "cpu/rec_sample.h"

#include <stdio.h>
#include <string.h>

#include "cpu/rec.h"

void salsa64_rec_sample_init(struct salsa64_rec_sample *s)
{
	memset(s, 0, sizeof *s);
}

void salsa64_rec_sample_compiled(struct salsa64_rec_sample *s, unsigned id, uint32_t words, uint32_t hot)
{
	if (id >= SALSA64_SMP_IDS)
		return;
	s->samples[id] = 0;  /* un id que vuelve a usarse arranca de cero */
	s->words[id] = (uint16_t)(words > 0xFFFF ? 0xFFFF : words);
	s->hot[id] = (uint16_t)(hot > 0xFFFF ? 0xFFFF : hot);
}

/* Entrada de la tabla para `pc` (la crea si no está), o NULL si está llena. */
static struct salsa64_rec_sample_entry *entry_for(struct salsa64_rec_sample *s, uint32_t pc)
{
	uint32_t h = (pc >> 2) * 2654435761u;
	for (unsigned k = 0; k < SALSA64_SMP_TABLE; k++) {
		struct salsa64_rec_sample_entry *e = &s->table[(h + k) & (SALSA64_SMP_TABLE - 1)];
		if (e->pc == pc)
			return e;
		if (!e->pc) {
			e->pc = pc;
			s->count++;
			return e;
		}
	}
	return NULL;
}

void salsa64_rec_sample_fold(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_sample *s = rec->sample;
	if (!s || !id || id >= SALSA64_SMP_IDS)
		return;
	uint32_t n = s->samples[id];
	if (!n)
		return;
	s->samples[id] = 0;
	struct salsa64_rec_sample_entry *e = entry_for(s, rec->x.xb[id].vpc);
	if (!e) {
		s->full += n;
		return;
	}
	e->samples += n;
	e->n = rec->blocks[id].n;
	e->words = s->words[id];
	e->hot = s->hot[id];
}

void salsa64_rec_sample_fold_all(struct salsa64_rec *rec)
{
	if (!rec->sample)
		return;
	for (unsigned id = 1; id < rec->next_block && id < SALSA64_SMP_IDS; id++)
		salsa64_rec_sample_fold(rec, id);
}

static const char *const mark_names[SALSA64_SMP_N] = {
	"nativo", "camino lento", "despachador", "intérprete", "eventos", "compilar", "espera", "ritmo", "otro",
};

void salsa64_rec_sample_split(const struct salsa64_rec_sample *s, struct salsa64_rec_sample_prev *prev, char *text,
			      size_t size)
{
	uint64_t ticks = s->ticks - prev->ticks;
	uint32_t marks[SALSA64_SMP_N], other = 0;
	for (unsigned m = 0; m < SALSA64_SMP_N; m++) {
		marks[m] = s->marks[m] - prev->marks[m];
		prev->marks[m] = s->marks[m];
		if (m)
			other += marks[m];
	}
	uint32_t unknown = s->unknown - prev->unknown;
	prev->unknown = s->unknown;
	prev->ticks = s->ticks;
	uint64_t native = ticks > other ? ticks - other : 0;  /* bloques con id más los sin id */
	size_t len = (size_t)snprintf(text, size, "muestreo: %llu muestras; nativo %.1f %% (sin id %.1f)",
				      (unsigned long long)ticks, ticks ? 100.0 * native / ticks : 0.0,
				      ticks ? 100.0 * unknown / ticks : 0.0);
	for (unsigned m = 1; m < SALSA64_SMP_N && len < size; m++)
		len += (size_t)snprintf(text + len, size - len, ", %s %.1f", mark_names[m],
					ticks ? 100.0 * marks[m] / ticks : 0.0);
}

void salsa64_rec_sample_report(struct salsa64_rec *rec, unsigned top, void (*line)(void *ctx, const char *text),
			       void *ctx)
{
	struct salsa64_rec_sample *s = rec->sample;
	char text[320];
	if (!s)
		return;
	salsa64_rec_sample_fold_all(rec);
	uint64_t ticks = s->ticks, other = 0, in_table = 0, insns = 0, words = 0, hot = 0;
	for (unsigned m = 1; m < SALSA64_SMP_N; m++)
		other += s->marks[m];
	for (uint32_t k = 0; k < SALSA64_SMP_TABLE; k++) {
		const struct salsa64_rec_sample_entry *e = &s->table[k];
		if (!e->pc)
			continue;
		in_table += e->samples;
		insns += (uint64_t)e->samples * e->n;
		words += (uint64_t)e->samples * e->words;
		hot += (uint64_t)e->samples * e->hot;
	}
	snprintf(text, sizeof text,
		 "muestreo: %llu muestras; en código generado %.1f %% (%u bloques distintos, %llu muestras sin lugar, "
		 "%u sin id); bloques pesados por muestras: %.1f instrucciones, %.0f palabras de PPC (%.0f calientes), "
		 "%.1f por instrucción",
		 (unsigned long long)ticks, ticks ? 100.0 * (ticks - other) / ticks : 0.0, s->count,
		 (unsigned long long)s->full, s->unknown, in_table ? (double)insns / in_table : 0.0,
		 in_table ? (double)words / in_table : 0.0, in_table ? (double)hot / in_table : 0.0,
		 insns ? (double)words / insns : 0.0);
	line(ctx, text);
	size_t len = (size_t)snprintf(text, sizeof text, "muestreo por categoría:");
	for (unsigned m = 1; m < SALSA64_SMP_N && len < sizeof text; m++)
		len += (size_t)snprintf(text + len, sizeof text - len, " %s %.1f %%", mark_names[m],
					ticks ? 100.0 * s->marks[m] / ticks : 0.0);
	line(ctx, text);
	/* Los `top` de más muestras: selección simple, la tabla es chica. */
	for (unsigned k = 0; k < top; k++) {
		struct salsa64_rec_sample_entry *best = NULL;
		for (uint32_t j = 0; j < SALSA64_SMP_TABLE; j++) {
			struct salsa64_rec_sample_entry *e = &s->table[j];
			if (e->pc && e->samples && (!best || e->samples > best->samples))
				best = e;
		}
		if (!best)
			break;
		snprintf(text, sizeof text,
			 "muestreo bloque %08x: %u muestras (%.2f %% del tiempo, %.2f %% del nativo), %u instr., %u palabras "
			 "(%u calientes), %.1f por instrucción",
			 best->pc, best->samples, ticks ? 100.0 * best->samples / ticks : 0.0,
			 in_table ? 100.0 * best->samples / in_table : 0.0, best->n, best->words, best->hot,
			 best->n ? (double)best->words / best->n : 0.0);
		line(ctx, text);
		best->samples = 0;  /* ya listado (las cuentas se agotan: el informe es el final) */
	}
}
