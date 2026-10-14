/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — registro del recompilador para el perfilado (rec_trace.h). */
#include "cpu/rec_trace.h"

#include <string.h>

#include "cpu/rec.h"
#include "cpu/vr4300_internal.h"

void salsa64_rec_trace_init(struct salsa64_rec_trace *t)
{
	bool seq = t->seq, jr = t->jr;
	uint32_t win_len = t->win_len;
	void (*on_block)(void *, const struct salsa64_rec_trace_block *) = t->on_block;
	void (*on_evict)(void *, unsigned, unsigned, unsigned) = t->on_evict;
	void *ctx = t->ctx;
	memset(t, 0, sizeof *t);
	t->seq = seq;
	t->jr = jr;
	t->win_len = win_len > SALSA64_RT_WIN_MAX ? SALSA64_RT_WIN_MAX : win_len ? win_len : 16384;
	t->on_block = on_block;
	t->on_evict = on_evict;
	t->ctx = ctx;
}

void salsa64_rec_trace_arm(struct salsa64_rec *rec)
{
	struct salsa64_rec_trace *t = rec->trace;
	struct salsa64_cpu *cpu = rec->cpu;
	t->arm_at = 0;
	if (t->armed)
		return;
	t->armed = true;
	t->win_cycles = cpu->cycles;
	cpu->rec_tpos = t->win;
	cpu->rec_tend = t->win + t->win_len;
	cpu->rec_tron = 1;
}

/* Datos de un bloque vivo para la tabla de una ventana. */
static void tab_add(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_trace *t = rec->trace;
	if (t->in_tab[id >> 3] & (1u << (id & 7)))
		return;
	t->in_tab[id >> 3] |= (uint8_t)(1u << (id & 7));
	if (t->ntab >= SALSA64_RT_TAB_MAX) {
		t->tab_full++;
		return;
	}
	struct salsa64_rec_trace_id *e = &t->tab[t->ntab++];
	e->id = (uint16_t)id;
	e->vpc = rec->x.xb[id].vpc;
	e->code = rec->blocks[id].code;
	e->n = rec->blocks[id].n;
	e->end = t->end[id];
	e->hot = t->hot[id];
	e->cold = t->cold[id];
	e->target = t->target[id];
	e->nbr = t->nbr[id];
}

unsigned salsa64_rec_trace_take(struct salsa64_rec *rec, bool force)
{
	struct salsa64_rec_trace *t = rec->trace;
	struct salsa64_cpu *cpu = rec->cpu;
	if (!t || !t->armed || (cpu->rec_tron && !force))
		return 0;
	cpu->rec_tron = 0;
	t->armed = false;
	unsigned len = (unsigned)(cpu->rec_tpos - t->win);
	t->ntab = t->tab_full = 0;
	memset(t->in_tab, 0, sizeof t->in_tab);
	for (unsigned k = 0; k < len; k++) {
		uint32_t v = t->win[k];
		if (v && v < SALSA64_RT_IDS)
			tab_add(rec, v);
		else if ((v & 0xFF000000u) == SALSA64_RT_M_COMPILE && (v & 0xFFFF))
			tab_add(rec, v & 0xFFFF);
	}
	return len;
}

/* ---- Compilaciones ----------------------------------------------------------- */

static bool seen_insert(struct salsa64_rec_trace *t, uint64_t key)
{
	if (!key)
		key = 1;
	if (t->seen_count >= SALSA64_RT_SEEN - SALSA64_RT_SEEN / 4) {
		t->seen_full++;
		return false;
	}
	uint32_t h = (uint32_t)(key ^ (key >> 29)) & (SALSA64_RT_SEEN - 1);
	for (;;) {
		if (t->seen[h] == key)
			return true;
		if (!t->seen[h]) {
			t->seen[h] = key;
			t->seen_count++;
			return false;
		}
		h = (h + 1) & (SALSA64_RT_SEEN - 1);
	}
}

void salsa64_rec_trace_compiled(struct salsa64_rec *rec, unsigned id, uint64_t pc, uint32_t pa, unsigned seg,
				uint32_t code, unsigned n, uint64_t ticks)
{
	struct salsa64_rec_trace *t = rec->trace;
	struct salsa64_rec_trace_block *b = &t->cur;
	b->vpc = (uint32_t)pc;
	b->pa = pa;
	b->code = code;
	b->ticks = (uint32_t)(ticks > 0xFFFFFFFFull ? 0xFFFFFFFFull : ticks);
	b->id = (uint16_t)id;
	b->n = (uint8_t)n;
	b->seg = (uint8_t)seg;
	if (!code) {
		b->flags |= SALSA64_RT_F_NOCODE;
		b->hot = b->cold = 0;
	}
	uint64_t key = b->hash ^ ((uint64_t)b->vpc * 0x9E3779B97F4A7C15ull) ^ n;
	if (seen_insert(t, key)) {
		b->flags |= SALSA64_RT_F_RETRANS;
		t->retrans++;
		t->field.retrans++;
	}
	if (id < SALSA64_RT_IDS) {
		t->end[id] = b->end;
		t->nbr[id] = b->nbr;
		t->target[id] = b->target;
		t->hot[id] = b->hot;
		t->cold[id] = b->cold;
		memset(&t->jr_id[id], 0, sizeof t->jr_id[id]);
	}
	t->compiled++;
	t->words += (uint32_t)b->hot + b->cold;
	t->field.compiled++;
	t->field.compile_ticks += ticks;
	t->field.words += (uint32_t)b->hot + b->cold;
	if (t->on_block)
		t->on_block(t->ctx, b);
	memset(b, 0, sizeof *b);
}

void salsa64_rec_trace_evicted(struct salsa64_rec *rec, unsigned kind, unsigned seg, unsigned killed)
{
	struct salsa64_rec_trace *t = rec->trace;
	t->killed += killed;
	t->field.killed += killed;
	if (kind == 2) {
		t->flushes++;
		t->field.flushes++;
	} else {
		t->evictions++;
		t->field.evictions++;
	}
	if (t->on_evict)
		t->on_evict(t->ctx, kind, seg, killed);
}

/* ---- JR ---------------------------------------------------------------------- */

static struct salsa64_rec_trace_jrsite *jr_site(struct salsa64_rec_trace *t, uint32_t vpc)
{
	uint32_t h = (vpc >> 2) * 2654435761u;
	for (unsigned k = 0; k < SALSA64_RT_JR_SITES; k++) {
		struct salsa64_rec_trace_jrsite *e = &t->jr_sites[(h + k) & (SALSA64_RT_JR_SITES - 1)];
		if (e->vpc == vpc)
			return e;
		if (!e->vpc) {
			e->vpc = vpc;
			t->jr_nsites++;
			return e;
		}
	}
	return NULL;
}

void salsa64_rec_trace_fold(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_trace *t = rec->trace;
	if (!t || !id || id >= SALSA64_RT_IDS)
		return;
	struct salsa64_rec_trace_jr *j = &t->jr_id[id];
	if (!j->count)
		return;
	struct salsa64_rec_trace_jrsite *e = jr_site(t, rec->x.xb[id].vpc);
	if (!e) {
		t->jr_full += j->count;
	} else {
		e->count += j->count;
		e->repeat += j->repeat;
		e->end = t->end[id];
		e->n = rec->blocks[id].n;
	}
	j->count = j->repeat = 0;
}

void salsa64_rec_trace_fold_all(struct salsa64_rec *rec)
{
	if (!rec->trace)
		return;
	for (unsigned id = 1; id < rec->next_block && id < SALSA64_RT_IDS; id++)
		salsa64_rec_trace_fold(rec, id);
}

unsigned salsa64_rec_trace_class(uint32_t i)
{
	unsigned op = I_OP(i);
	if (op == 0x11)
		return SALSA64_RT_C_FPU;
	if (op == 0x10 || op == 0x2F || (op == 0 && (I_FUNCT(i) == 0x0C || I_FUNCT(i) == 0x0D)))
		return SALSA64_RT_C_OTHER;  /* COP0, CACHE, SYSCALL, BREAK */
	if (op >= 0x20)
		return SALSA64_RT_C_MEM;
	return SALSA64_RT_C_ALU;
}
