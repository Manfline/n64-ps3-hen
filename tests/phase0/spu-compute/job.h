/*
 * Retro64 — Fase 0: descriptor de trabajo compartido entre PPE y SPU.
 *
 * Las estructuras viajan por DMA: tamaño múltiplo de 16 bytes y alineación
 * de 16. Los búferes de datos se alinean a 128 (línea de caché del Cell).
 */
#ifndef RETRO64_PHASE0_SPU_JOB_H
#define RETRO64_PHASE0_SPU_JOB_H

#include <stdint.h>

#define SPU_CHUNK_BYTES  16384u /* máximo de una transferencia DMA */
#define SPU_CHUNK_WORDS  (SPU_CHUNK_BYTES / 4)
#define SPU_RESULT_MAGIC 0xC0DE0064u
#define SPU_MIX_ROT      7
#define SPU_MIX_ADD      0x9E3779B9u

typedef struct {
	uint64_t src_ea;    /* entrada: chunks * SPU_CHUNK_BYTES */
	uint64_t dst_ea;    /* salida, mismo tamaño */
	uint64_t result_ea; /* spu_result_t */
	uint32_t chunks;
	uint32_t index;     /* número de hilo SPU */
} __attribute__((aligned(16))) spu_job_t;

typedef struct {
	uint32_t magic;
	uint32_t index;
	uint32_t chunks_done;
	uint32_t checksum;  /* suma mod 2^32 de las palabras de salida */
} __attribute__((aligned(16))) spu_result_t;

/* Transformación de referencia (PPE); la SPU hace lo mismo con SIMD. */
static inline uint32_t spu_mix_ref(uint32_t x)
{
	return ((x << SPU_MIX_ROT) | (x >> (32 - SPU_MIX_ROT))) ^ (x + SPU_MIX_ADD);
}

#endif
