/*
 * Salsa64 — nemu64-test completo en la PS3 como prueba y medición del
 * recompilador. Ver rombench.c.
 */
#ifndef SALSA64_CPU_TESTS_ROMBENCH_H
#define SALSA64_CPU_TESTS_ROMBENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct salsa64_rec;

struct rombench_result {
	bool finished;        /* apareció el resumen de nemu64-test */
	uint32_t failed;      /* pruebas con falla */
	uint64_t names_hash;  /* FNV-1a de sus nombres, en orden */
	uint64_t cycles;
	uint64_t us;
	char summary[256];
};

/* Corre nemu64-test hasta su resumen con el intérprete (rec = NULL) o con
 * el recompilador sobre la caché `code`. */
bool salsa64_rombench(struct rombench_result *r, struct salsa64_rec *rec, void *code, size_t bytes,
		      uint64_t (*now_us)(void));

#endif
