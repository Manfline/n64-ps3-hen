/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/rec.h"

#include <stdio.h>
#include <stdlib.h>

#include "cpu/vr4300.h"

bool salsa64_rec_diag_alloc(struct salsa64_rec *rec)
{
	rec->diag = calloc(1, sizeof *rec->diag);
	return rec->diag != NULL;
}

void salsa64_rec_diag_free(struct salsa64_rec *rec)
{
	free(rec->diag);
	rec->diag = NULL;
}

uint64_t salsa64_rec_diag_hash(const struct salsa64_rec *rec, uint32_t pa, uint32_t bytes)
{
	uint64_t h = 14695981039346656037ull;
	for (uint32_t i = 0; i < bytes; i++) {
		h ^= pa < rec->rdram_size ? rec->cpu->rdram[pa + i] :
		     (salsa64_rec_fetch(rec, (pa + i) & ~3u) >> (8 * (3 - ((pa + i) & 3))) & 255u);
		h *= 1099511628211ull;
	}
	return h;
}

void salsa64_rec_diag_compiled(struct salsa64_rec *rec, unsigned id, uint64_t ticks)
{
	struct salsa64_rec_diag *d = rec->diag;
	const struct salsa64_rec_block *b = &rec->blocks[id];
	uint32_t pc = rec->x.xb[id].vpc;
	uint64_t hash = salsa64_rec_diag_hash(rec, b->pa, b->bytes);
	d->hash[id] = hash;
	d->ticks += ticks;
	unsigned h = ((pc >> 2) * 2654435761u) & (SALSA64_REC_DIAG_COMPILES - 1);
	for (unsigned k = 0; k < 64; k++) {
		struct salsa64_rec_diag_compile *e = &d->compiles[(h + k) & (SALSA64_REC_DIAG_COMPILES - 1)];
		if (!e->count || (e->pc == pc && e->hash == hash && e->bytes == b->bytes)) {
			if (e->count) {
				d->repeated++;
				d->repeat_ticks += ticks;
			}
			e->pc = pc;
			e->hash = hash;
			e->bytes = b->bytes;
			e->count++;
			e->ticks += ticks;
			return;
		}
	}
	d->compile_full++;
}

void salsa64_rec_diag_hit(struct salsa64_rec *rec, unsigned id, uint32_t pa, uint32_t len)
{
	struct salsa64_rec_diag *d = rec->diag;
	const struct salsa64_rec_block *b = &rec->blocks[id];
	unsigned source = d->source;
	bool same = d->hash[id] == salsa64_rec_diag_hash(rec, b->pa, b->bytes);
	d->hits[source]++;
	d->same[source] += same;
	uint32_t pc = (uint32_t)rec->cpu->cur_pc, block = rec->x.xb[id].vpc;
	unsigned h = ((pc >> 2) * 2654435761u ^ block * 2246822519u ^ pa * 3266489917u) &
		     (SALSA64_REC_DIAG_SITES - 1);
	for (unsigned k = 0; k < 64; k++) {
		struct salsa64_rec_diag_site *e = &d->sites[(h + k) & (SALSA64_REC_DIAG_SITES - 1)];
		if (!e->hits || (e->pc == pc && e->pa == pa && e->len == len && e->block == block && e->source == source)) {
			e->pc = pc;
			e->pa = pa;
			e->len = len;
			e->block = block;
			e->bytes = b->bytes;
			e->source = source;
			e->hits++;
			e->same += same;
			return;
		}
	}
	d->site_full++;
}

static bool selected(const unsigned *ids, unsigned n, unsigned id)
{
	for (unsigned j = 0; j < n; j++)
		if (ids[j] == id)
			return true;
	return false;
}

void salsa64_rec_diag_report(const struct salsa64_rec *rec, uint64_t hz, bool details, void (*log)(const char *, ...))
{
	const struct salsa64_rec_diag *d = rec->diag;
	if (!d)
		return;
	char s[512];
	snprintf(s, sizeof s, "invalidaciones: llamadas CPU/PI/otros %llu/%llu/%llu; bloques %llu/%llu/%llu; "
		 "iguales (huella) %llu/%llu/%llu; compilando %llu ticks, repetidas %llu (%llu ticks); "
		 "sin sitio %llu, sin compilacion %llu",
		 (unsigned long long)d->calls[1], (unsigned long long)d->calls[2], (unsigned long long)d->calls[0],
		 (unsigned long long)d->hits[1], (unsigned long long)d->hits[2], (unsigned long long)d->hits[0],
		 (unsigned long long)d->same[1], (unsigned long long)d->same[2], (unsigned long long)d->same[0],
		 (unsigned long long)d->ticks, (unsigned long long)d->repeated, (unsigned long long)d->repeat_ticks,
		 (unsigned long long)d->site_full, (unsigned long long)d->compile_full);
	log("%s", s);
	snprintf(s, sizeof s, "espera: %s; consultas quitadas %llu, invalidaciones %llu (%llu iguales), "
		 "retirados por desalojo %llu", rec->opts & SALSA64_REC_OPT_IDLE_RECOMPILE ? "recompilar" : "parche PPC",
		 (unsigned long long)d->idle_patched, (unsigned long long)d->hits[3], (unsigned long long)d->same[3],
		 (unsigned long long)d->evicted);
	log("%s", s);
	if (hz) {
		snprintf(s, sizeof s, "compilacion: %.3f ms, repetidas %.3f ms (timebase %llu Hz)",
			 d->ticks * 1e3 / hz, d->repeat_ticks * 1e3 / hz, (unsigned long long)hz);
		log("%s", s);
	}
	if (!details)
		return;
	unsigned ids[8], n = 0;
	for (unsigned j = 0; j < 8; j++) {
		unsigned best = SALSA64_REC_DIAG_COMPILES;
		for (unsigned k = 0; k < SALSA64_REC_DIAG_COMPILES; k++)
			if (!selected(ids, n, k) && d->compiles[k].count &&
			    (best == SALSA64_REC_DIAG_COMPILES || d->compiles[k].count > d->compiles[best].count))
				best = k;
		if (best == SALSA64_REC_DIAG_COMPILES)
			break;
		ids[n++] = best;
		const struct salsa64_rec_diag_compile *e = &d->compiles[best];
		snprintf(s, sizeof s, "compilacion sitio: pc=%08x bytes=%u veces=%llu ticks=%llu", e->pc, e->bytes,
			 (unsigned long long)e->count, (unsigned long long)e->ticks);
		log("%s", s);
	}
	n = 0;
	for (unsigned j = 0; j < 8; j++) {
		unsigned best = SALSA64_REC_DIAG_SITES;
		for (unsigned k = 0; k < SALSA64_REC_DIAG_SITES; k++)
			if (!selected(ids, n, k) && d->sites[k].hits &&
			    (best == SALSA64_REC_DIAG_SITES || d->sites[k].hits > d->sites[best].hits))
				best = k;
		if (best == SALSA64_REC_DIAG_SITES)
			break;
		ids[n++] = best;
		const struct salsa64_rec_diag_site *e = &d->sites[best];
		snprintf(s, sizeof s, "invalidacion sitio: %s pc=%08x rango=%08x+%u bloque=%08x+%u veces=%llu iguales=%llu",
			 e->source == 1 ? "CPU" : e->source == 2 ? "PI" : e->source == 3 ? "espera" : "otro",
			 e->pc, e->pa, e->len, e->block, e->bytes, (unsigned long long)e->hits, (unsigned long long)e->same);
		log("%s", s);
	}
}
