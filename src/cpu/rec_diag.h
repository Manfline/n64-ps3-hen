/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSA64_REC_DIAG_H
#define SALSA64_REC_DIAG_H

#include "core/types.h"

struct salsa64_rec;
#define SALSA64_REC_DIAG_SITES 8192
#define SALSA64_REC_DIAG_COMPILES 16384

/* Tablas acotadas. Los desbordamientos se cuentan: el total por causa
 * sigue siendo exacto aunque no quepa el detalle de un sitio. Sin código
 * del juego ni asignaciones en ejecución. La huella solo sirve para medir. */
struct salsa64_rec_diag_site {
	uint32_t pc, pa, len, block, bytes, source;
	uint64_t hits, same;
};
struct salsa64_rec_diag_compile {
	uint32_t pc, bytes;
	uint64_t hash, count, ticks;
};
struct salsa64_rec_diag {
	uint64_t hash[65536];
	struct salsa64_rec_diag_site sites[SALSA64_REC_DIAG_SITES];
	struct salsa64_rec_diag_compile compiles[SALSA64_REC_DIAG_COMPILES];
	/* Causas: 0 otros escritores, 1 CPU, 2 PI, 3 descarte de espera. */
	uint64_t calls[4], hits[4], same[4], ticks, repeated, repeat_ticks, site_full, compile_full;
	uint64_t idle_patched, evicted;
	unsigned source;
};
uint64_t salsa64_rec_diag_hash(const struct salsa64_rec *rec, uint32_t pa, uint32_t bytes);
void salsa64_rec_diag_compiled(struct salsa64_rec *rec, unsigned id, uint64_t ticks);
void salsa64_rec_diag_hit(struct salsa64_rec *rec, unsigned id, uint32_t pa, uint32_t len);
void salsa64_rec_diag_report(const struct salsa64_rec *rec, uint64_t hz, bool details, void (*log)(const char *, ...));
bool salsa64_rec_diag_alloc(struct salsa64_rec *rec);
void salsa64_rec_diag_free(struct salsa64_rec *rec);
#endif
