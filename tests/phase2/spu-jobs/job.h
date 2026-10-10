/*
 * Salsa64 — Fase 2: parámetros y resultado de los trabajos de prueba de
 * lib/spukit (viajan dentro del descriptor y del resultado de 128 bytes).
 */
#ifndef SALSA64_PHASE2_SPU_JOBS_H
#define SALSA64_PHASE2_SPU_JOBS_H

#include <stdint.h>

enum { JOB_SCALE = 1, JOB_PING = 2 };

/* JOB_SCALE: dst[i] = src[i] * mul + add (palabras de 32 bits) y la suma de
 * la salida. JOB_PING: nada; mide la ida y vuelta. */
typedef struct {
	uint64_t src_ea, dst_ea;
	uint32_t words;  /* múltiplo de 4 (16 bytes) */
	uint32_t mul, add;
	uint32_t pad;
} job_scale;

typedef struct {
	uint32_t sum;
	uint32_t words;
	uint32_t worker;  /* argumento con que arrancó el SPU */
	uint32_t pad;
} job_scale_result;

static inline uint32_t job_scale_ref(uint32_t x, uint32_t mul, uint32_t add)
{
	return x * mul + add;
}

#endif
