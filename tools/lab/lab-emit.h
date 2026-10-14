/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — laboratorio del recompilador: lo que comparten rec-lab.c y
 * lab-trace.c (docs/lab-recompilador.md). Se incluye en una sola unidad de
 * traducción, después de cpu/rec.c (que se incluye en vez de enlazarse: así se
 * ven sus funciones internas).
 */
#ifndef SALSA64_LAB_EMIT_H
#define SALSA64_LAB_EMIT_H

#include "cpu/rec.c"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define LAB_CACHE_BYTES (24u << 20)
#define H_BASE 4080  /* palabra de la caché de las funciones de C ficticias */

static void die(const char *msg, const char *arg)
{
	fprintf(stderr, "rec-lab: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
	exit(1);
}

static uint8_t *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		die("no se puede abrir", path);
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc((size_t)n);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n)
		die("lectura incompleta", path);
	fclose(f);
	*size = (size_t)n;
	return b;
}

/* Como salsa64_rec_init sin lo nativo (TOC, funciones de C, mapa de página):
 * las funciones de C que llama el código generado quedan en direcciones
 * ficticias dentro del alcance de `bl`. */
static bool lab_init(struct salsa64_rec *rec, struct salsa64_cpu *cpu, uint32_t *code, size_t bytes, uint32_t opts, bool far_calls,
		     bool attach)
{
	uint32_t size = cpu->rdram_size;
	memset(rec, 0, sizeof *rec);
	rec->opts = opts;
	rec->profile_sites = false;  /* sin perfil: como el paquete de la PS3 por omisión */
	rec->cpu = cpu;
	rec->code = code;
	rec->code_words = (uint32_t)(bytes / 4);
	rec->scratch = rec->code_words - SCRATCH_WORDS;
	rec->rdram_size = size;
	while ((1u << rec->rdram_shift) < size)
		rec->rdram_shift++;
	uint64_t base = (uintptr_t)code;
	/* Al final de la zona reservada al preámbulo (nunca se emite ahí): lab-analiza.py las reconoce. */
	rec->h_load = base + 4 * (H_BASE + 0);
	rec->h_store = base + 4 * (H_BASE + 1);
	rec->h_exec = base + 4 * (H_BASE + 2);
	rec->h_exception = base + 4 * (H_BASE + 3);
	rec->h_tlbw = base + 4 * (H_BASE + 4);
	rec->h_eret = base + 4 * (H_BASE + 5);
	if (far_calls) {  /* como SALSA64_REC_FAR=1: C a más de 32 MB de la caché, llamadas por mtctr/bctrl */
		uint64_t far_base = 0x00A00000u;
		rec->h_load = far_base + 0 * 16, rec->h_store = far_base + 1 * 16, rec->h_exec = far_base + 2 * 16;
		rec->h_exception = far_base + 3 * 16, rec->h_tlbw = far_base + 4 * 16, rec->h_eret = far_base + 5 * 16;
	}
	rec->hot_words = 0;
	rec->seg_words = ((rec->scratch - cold_base(rec)) / SALSA64_REC_SEGMENTS) & ~31u;
	rec->calls_near = salsa64_rec_calls_near_at(rec, 0);
	uint32_t n = emit_preamble(rec);
	if (n > preamble_words())
		return false;
	rec->fpu_rtn_ok = !(opts & SALSA64_REC_OPT_FPU_INLINE) && salsa64_rec_emit_fpu_rtns(rec, n, preamble_words());
	seg_reset(rec);
	rec->seg_end = rec->scratch;
	rec->next_block = 1;
	rec->fr = cpu->status & SALSA64_ST_FR;
	salsa64_rec_x_init(rec);
	if (attach) {  /* con la máquina corriendo (lab-trace) el intérprete no debe saber del recompilador */
		cpu->rec = rec;
		cpu->code_flags = rec->flags;
	}
	return true;
}

/* Un bloque traducido: lo que el análisis necesita. */
struct lab_emitted {
	unsigned n, fr, seg;
	uint32_t bytes, hot, total, entry;
	bool frdep, loop;
	uint32_t mips[SALSA64_REC_MAX_INSNS];
	uint16_t mh[SALSA64_REC_MAX_INSNS + 1], mc[SALSA64_REC_MAX_INSNS + 1];
	uint8_t how[SALSA64_REC_MAX_INSNS + 1];
	uint32_t *words;      /* copia de las palabras PPC (el llamador la libera) */
};

/* Traduce el bloque en (vpc, pa, seg) con Status.FR = fr; el código queda en el mismo tramo
 * de la caché (no se ejecuta), así que se copia. false si no compiló. */
static bool lab_emit(struct salsa64_rec *rec, uint64_t vpc, uint32_t pa, unsigned seg, unsigned fr, struct lab_emitted *e)
{
	struct salsa64_cpu *cpu = rec->cpu;
	uint32_t saved_status = cpu->status;
	rec->used = cold_base(rec);
	rec->fr = fr;
	cpu->status = (cpu->status & ~(uint32_t)SALSA64_ST_FR) | (fr ? SALSA64_ST_FR : 0);
	rec->cur_id = 0;
	rec->compiled_fr_dep = false;
	unsigned n = 0;
	uint32_t bytes = 4;
	uint32_t entry = salsa64_rec_compile(rec, vpc, pa, seg, SALSA64_REC_MAX_INSNS, false, &n, &bytes);
	cpu->status = saved_status;
	if (!entry)
		return false;
	e->n = n;
	e->bytes = bytes;
	e->fr = fr;
	e->seg = seg;
	e->hot = rec->nbuf[0];
	e->total = rec->nbuf[0] + rec->nbuf[1];
	e->entry = entry;
	e->frdep = rec->compiled_fr_dep;
	e->loop = rec->lab.loop;
	for (unsigned q = 0; q < bytes / 4 && q < SALSA64_REC_MAX_INSNS; q++)
		e->mips[q] = salsa64_rec_fetch(rec, pa + 4 * q);
	memcpy(e->mh, rec->lab.hot, sizeof e->mh);
	memcpy(e->mc, rec->lab.cold, sizeof e->mc);
	memcpy(e->how, rec->lab.how, sizeof e->how);
	e->words = malloc(e->total * sizeof *e->words);
	memcpy(e->words, rec->code + entry, e->total * sizeof *e->words);
	return true;
}

/* Cabecera del .lab: opciones, desplazamientos de struct salsa64_cpu, rutinas compartidas del
 * preámbulo (palabra de la caché) y el preámbulo mismo. */
static void lab_print_prologue(FILE *out, const struct salsa64_rec *rec, const uint32_t *code, uint32_t opts, int fr,
			       const char *source)
{
	fprintf(out, "# rec-lab v1 opts=%u fr=%d fpu_rtn=%d preambulo=%u fuente=%s\n", opts, fr, rec->fpu_rtn_ok, preamble_words(), source);
	fprintf(out, "O gpr=%d hi=%d lo=%d cycles=%d cur_pc=%d delay_slot=%d status=%d irq_pending=%d sched=%d fpr=%d "
		"rec_site=%d rec_tmp=%d rec_lr=%d fcr31=%d rec_fw=%d rec_tb=%d\n", OFF(gpr), OFF(hi), OFF(lo), OFF(cycles), OFF(cur_pc), OFF(delay_slot),
		OFF(status), OFF(irq_pending), OFF(sched), OFF(fpr), OFF(rec_site), OFF(rec_tmp), OFF(rec_lr), OFF(fcr31), OFF(rec_fw), OFF(rec_tb));
	fprintf(out, "L exit_link=%u exit_mid=%u exit_jump=%u exit_jr=%u exit_idle=%u exit_start=%u exit_fr=%u exit_end=%u slow_rtn=%u",
		rec->exit_link, rec->exit_mid, rec->exit_jump, rec->exit_jr, rec->exit_idle, rec->exit_start, rec->exit_fr,
		rec->exit_end, rec->slow_rtn);
	for (unsigned q = 0; q < 2; q++)
		for (unsigned r = 0; r < 4; r++)
			fprintf(out, " ktlb%u_%u=%u", q, r, rec->ktlb_rtn[q][r]);
	for (unsigned q = 0; q < SALSA64_FPR_N; q++)
		fprintf(out, " fpu%u=%u", q, rec->fpu_rtn[q]);
	fprintf(out, " h_load=%u h_store=%u h_exec=%u h_exception=%u h_tlbw=%u h_eret=%u\n", H_BASE + 0u, H_BASE + 1u, H_BASE + 2u,
		H_BASE + 3u, H_BASE + 4u, H_BASE + 5u);
	uint32_t last = 0;
	for (uint32_t q = 0; q < preamble_words(); q++)
		if (code[q])
			last = q + 1;
	fprintf(out, "R %u\n", last);
	for (uint32_t q = 0; q < last; q++)
		fprintf(out, "%08x%c", code[q], q % 16 == 15 || q + 1 == last ? '\n' : ' ');
}

static void lab_print_block(FILE *out, uint32_t vpc, uint64_t count, const struct lab_emitted *e)
{
	fprintf(out, "B %08x %llu n=%u bytes=%u fr=%u frdep=%d hot=%u total=%u entry=%u seg=%u loop=%d\n", vpc,
		(unsigned long long)count, e->n, e->bytes, e->fr, e->frdep, e->hot, e->total, e->entry, e->seg, e->loop);
	fprintf(out, "M");
	for (unsigned q = 0; q < e->bytes / 4 && q < SALSA64_REC_MAX_INSNS; q++)
		fprintf(out, " %08x", e->mips[q]);
	fprintf(out, "\nK");
	for (unsigned q = 0; q <= e->n; q++) {
		if (e->mh[q] == 0xFFFF)
			fprintf(out, " -");
		else
			fprintf(out, " %u/%u:%u", e->mh[q], e->mc[q], e->how[q]);
	}
	fprintf(out, "\nP");
	for (uint32_t q = 0; q < e->total; q++)
		fprintf(out, " %08x", e->words[q]);
	fprintf(out, "\n");
}

#endif
