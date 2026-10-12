/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — perfil del recompilador: el texto del desglose. Ver rec_prof.h.
 */
#include "cpu/rec_prof.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cpu/rec.h"
#include "cpu/vr4300_internal.h"

struct out {
	char *buf;
	size_t size, len;
};

static void put(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(struct out *o, const char *fmt, ...)
{
	if (o->len >= o->size)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(o->buf + o->len, o->size - o->len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	o->len += (size_t)n < o->size - o->len ? (size_t)n : o->size - o->len - 1;
}

struct scale {
	uint64_t hz;
	double per;      /* divisor de las cuentas y los tiempos (campos, o 1) */
};

static double ms(const struct scale *s, uint64_t ticks)
{
	return s->hz ? ticks * 1e3 / (double)s->hz / s->per : 0.0;
}

/* "nombre cuenta (ms)" si la cuenta no es 0; con `sep` delante si ya hubo. */
static void item(struct out *o, const struct scale *s, bool *any, const char *name, uint64_t n, uint64_t t)
{
	if (!n)
		return;
	put(o, "%s%s %.*f", *any ? ", " : "", name, s->per > 1 ? 1 : 0, n / s->per);
	if (s->hz)
		put(o, " (%.2f ms)", ms(s, t));
	*any = true;
}

static void group(struct out *o, const struct scale *s, const struct salsa64_rec_prof *d, const char *title,
		  const char *const *names, unsigned first, unsigned last)
{
	bool any = false;
	put(o, "\n%s: ", title);
	for (unsigned k = first; k <= last; k++)
		item(o, s, &any, names[k - first], d->n[k], d->t[k]);
	if (!any)
		put(o, "nada");
}

static uint64_t sum_t(const struct salsa64_rec_prof *d, unsigned first, unsigned last)
{
	uint64_t t = 0;
	for (unsigned k = first; k <= last; k++)
		t += d->t[k];
	return t;
}

static uint64_t sum_n(const struct salsa64_rec_prof *d, unsigned first, unsigned last)
{
	uint64_t n = 0;
	for (unsigned k = first; k <= last; k++)
		n += d->n[k];
	return n;
}

void salsa64_rec_prof_report(const struct salsa64_rec_prof *now, struct salsa64_rec_prof *prev, uint64_t hz,
			     unsigned fields, char *buf, size_t size)
{
	static const char *const load_names[] = { "TLB→RDRAM", "TLB→E/S", "falla de TLB", "E/S", "dirección" };
	static const char *const store_names[] = { "TLB→RDRAM", "TLB→E/S", "falla de TLB", "E/S", "código", "dirección" };
	static const char *const exec_names[] = { "COP0", "FPU", "LL/SC", "CACHE", "trap", "otras" };
	static const char *const interp_names[] = { "inicio", "mitad", "irq", "delay slot", "modo", "segmento",
						    "sin código", "no cabe" };
	static const char *const ev_names[] = { "parada", "Compare", "VI intr", "VI campo", "RSP", "DP", "AI", "RSP LLE",
						"varios" };
	static const char *const exit_names[] = { "enlace", "mitad", "salto", "espera", "inicio", "fin", "redirect" };
	struct salsa64_rec_prof d;
	struct out o = { buf, size, 0 };
	struct scale s = { hz, fields ? fields : 1 };

	if (!size)
		return;
	buf[0] = '\0';
	for (unsigned k = 0; k < SALSA64_PROF_N; k++) {
		d.t[k] = now->t[k] - prev->t[k];
		d.n[k] = now->n[k] - prev->n[k];
	}
	d.nested = now->nested - prev->nested;
	d.total = now->total - prev->total;
	*prev = *now;

	uint64_t t_calls = sum_t(&d, SALSA64_PROF_LOAD_TLB_RDRAM, SALSA64_PROF_EXCEPTION);
	uint64_t n_calls = sum_n(&d, SALSA64_PROF_LOAD_TLB_RDRAM, SALSA64_PROF_EXCEPTION);
	uint64_t t_interp = sum_t(&d, SALSA64_PROF_INTERP_START, SALSA64_PROF_INTERP_NOFIT);
	uint64_t n_interp = sum_n(&d, SALSA64_PROF_INTERP_START, SALSA64_PROF_INTERP_NOFIT);
	uint64_t t_ev = sum_t(&d, SALSA64_PROF_EV_STOP, SALSA64_PROF_EV_MANY);
	uint64_t n_ev = sum_n(&d, SALSA64_PROF_EV_STOP, SALSA64_PROF_EV_MANY);
	uint64_t t_known = d.t[SALSA64_PROF_NATIVE] + t_calls + t_interp + t_ev + d.t[SALSA64_PROF_COMPILE] +
			   d.t[SALSA64_PROF_XSYNC] + d.t[SALSA64_PROF_IDLE];
	uint64_t t_rest = d.total > t_known ? d.total - t_known : 0;
	double tot = d.total ? (double)d.total : 1.0;

	put(&o, "perfil JIT%s: ", fields ? " por campo" : "");
	if (hz) {
		put(&o, "emulación %.2f ms = nativo %.2f (%.0f %%), llamadas a C %.2f (%.0f %%), intérprete %.2f (%.0f %%), "
			"eventos %.2f (%.0f %%), compilar %.2f, vtlb %.2f, bucles de espera %.2f, despachador %.2f (%.0f %%); ",
		    ms(&s, d.total), ms(&s, d.t[SALSA64_PROF_NATIVE]), 100.0 * d.t[SALSA64_PROF_NATIVE] / tot,
		    ms(&s, t_calls), 100.0 * t_calls / tot, ms(&s, t_interp), 100.0 * t_interp / tot, ms(&s, t_ev),
		    100.0 * t_ev / tot, ms(&s, d.t[SALSA64_PROF_COMPILE]), ms(&s, d.t[SALSA64_PROF_XSYNC]),
		    ms(&s, d.t[SALSA64_PROF_IDLE]), ms(&s, t_rest), 100.0 * t_rest / tot);
	}
	put(&o, "%.*f entradas, %.*f llamadas a C, %.*f instr. interpretadas, %.*f eventos, %.*f bloques compilados, "
		"%.*f revisiones de vtlb, %.*f bucles de espera",
	    s.per > 1 ? 1 : 0, d.n[SALSA64_PROF_NATIVE] / s.per, s.per > 1 ? 1 : 0, n_calls / s.per, s.per > 1 ? 1 : 0,
	    n_interp / s.per, s.per > 1 ? 1 : 0, n_ev / s.per, s.per > 1 ? 2 : 0, d.n[SALSA64_PROF_COMPILE] / s.per,
	    s.per > 1 ? 1 : 0, d.n[SALSA64_PROF_XSYNC] / s.per, s.per > 1 ? 1 : 0, d.n[SALSA64_PROF_IDLE] / s.per);
	group(&o, &s, &d, "perfil cargas lentas", load_names, SALSA64_PROF_LOAD_TLB_RDRAM, SALSA64_PROF_LOAD_ADDR);
	group(&o, &s, &d, "perfil almacenamientos lentos", store_names, SALSA64_PROF_STORE_TLB_RDRAM,
	      SALSA64_PROF_STORE_ADDR);
	group(&o, &s, &d, "perfil al intérprete", exec_names, SALSA64_PROF_EXEC_COP0, SALSA64_PROF_EXEC_OTHER);
	{
		bool any = false;
		put(&o, "\nperfil excepciones: ");
		item(&o, &s, &any, "desde el código", d.n[SALSA64_PROF_EXCEPTION], d.t[SALSA64_PROF_EXCEPTION]);
		if (!any)
			put(&o, "nada");
	}
	group(&o, &s, &d, "perfil intérprete (instrucciones)", interp_names, SALSA64_PROF_INTERP_START,
	      SALSA64_PROF_INTERP_NOFIT);
	group(&o, &s, &d, "perfil eventos", ev_names, SALSA64_PROF_EV_STOP, SALSA64_PROF_EV_MANY);
	group(&o, &s, &d, "perfil salidas", exit_names, SALSA64_PROF_EXIT_LINK, SALSA64_PROF_EXIT_REDIRECT);
}

/* ---- Perfil por bloque ------------------------------------------------------------ */

static struct salsa64_rec_prof_pc *pc_entry(struct salsa64_rec_prof_blocks *b, uint32_t pc)
{
	uint32_t h = (pc >> 2) * 2654435761u >> 16 & (SALSA64_PROF_PCS - 1);
	while (b->pcs[h].n && b->pcs[h].pc != pc)
		h = (h + 1) & (SALSA64_PROF_PCS - 1);
	if (!b->pcs[h].n) {
		if (b->npcs >= SALSA64_PROF_PCS - 1024)
			return NULL;  /* llena: lo que sigue no se cuenta */
		b->npcs++;
		b->pcs[h].pc = pc;
	}
	return &b->pcs[h];
}

void salsa64_rec_prof_fold(struct salsa64_rec_prof *p, unsigned id, bool alive)
{
	struct salsa64_rec_prof_blocks *b = p->blocks;
	const struct salsa64_rec_prof_block *i = &b->info[id];
	if (!i->n)
		return;
	struct salsa64_rec_prof_pc *e = pc_entry(b, i->pc);
	if (e) {
		e->hits += b->hits[id];
		e->n = i->n;
		e->regs = i->regs;
		e->fpu = i->fpu;
		e->mem = i->mem;
		e->id = alive ? (uint16_t)id : 0;
	}
	b->hits[id] = 0;
}

void salsa64_rec_prof_blocks_report(struct salsa64_rec *rec, unsigned top, void (*line)(void *ctx, const char *text),
				    void *ctx)
{
	static const char *const names[34] = { "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
					       "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
					       "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra", "hi", "lo" };
	struct salsa64_rec_prof_blocks *b = rec->prof ? rec->prof->blocks : NULL;
	char text[1600];
	if (!b)
		return;
	for (unsigned id = 1; id < rec->next_block && id < SALSA64_PROF_BLOCKS; id++)
		if (rec->blocks[id].code)
			salsa64_rec_prof_fold(rec->prof, id, true);

	uint64_t hits = 0, instr = 0, fpu = 0, mem = 0, reg[34] = { 0 };
	for (uint32_t k = 0; k < SALSA64_PROF_PCS; k++) {
		const struct salsa64_rec_prof_pc *e = &b->pcs[k];
		if (!e->n)
			continue;
		hits += e->hits;
		instr += e->hits * e->n;
		fpu += e->hits * e->fpu;
		mem += e->hits * e->mem;
		for (unsigned r = 1; r < 34; r++)
			if (e->regs & (1ull << r))
				reg[r] += e->hits;
	}
	snprintf(text, sizeof text,
		 "bloques: %u distintos, %llu ejecuciones, %llu instrucciones (%.1f por bloque), %.2f de COP1 y %.2f de "
		 "memoria por bloque",
		 b->npcs, (unsigned long long)hits, (unsigned long long)instr, hits ? (double)instr / hits : 0.0,
		 hits ? (double)fpu / hits : 0.0, hits ? (double)mem / hits : 0.0);
	line(ctx, text);
	size_t len = snprintf(text, sizeof text, "registros (%% de las ejecuciones de bloques que los usan):");
	for (unsigned k = 0; k < 20 && len < sizeof text; k++) {
		unsigned best = 0;
		for (unsigned r = 1; r < 34; r++)
			if (reg[r] > reg[best])
				best = r;
		if (!reg[best])
			break;
		len += snprintf(text + len, sizeof text - len, " %s %.0f", names[best], hits ? 100.0 * reg[best] / hits : 0.0);
		reg[best] = 0;
	}
	line(ctx, text);
	for (unsigned k = 0; k < top; k++) {
		struct salsa64_rec_prof_pc *best = NULL;
		for (uint32_t j = 0; j < SALSA64_PROF_PCS; j++)
			if (b->pcs[j].n && b->pcs[j].hits && (!best || b->pcs[j].hits > best->hits))
				best = &b->pcs[j];
		if (!best)
			break;
		len = snprintf(text, sizeof text, "bloque %08x %llu ejecuciones (%.1f %%) %u instr. %u cop1 %u mem palabras:",
			       best->pc, (unsigned long long)best->hits, hits ? 100.0 * best->hits / hits : 0.0, best->n,
			       best->fpu, best->mem);
		if (best->id && rec->blocks[best->id].code) {
			uint32_t pa = rec->blocks[best->id].pa;
			for (unsigned i = 0; i < best->n && len < sizeof text - 12; i++)
				len += snprintf(text + len, sizeof text - len, " %08x", salsa64_rec_fetch(rec, pa + 4 * i));
		}
		line(ctx, text);
		best->hits = 0;
	}
}
