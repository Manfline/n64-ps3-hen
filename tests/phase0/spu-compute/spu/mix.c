/*
 * Cell64 — Fase 0: cálculo SIMD en una SPU con DMA de doble búfer.
 *
 * Lee su spu_job_t por DMA, transforma su porción del búfer de entrada en
 * bloques de 16 KB (mientras procesa un bloque llega el siguiente) y devuelve
 * un spu_result_t con la suma de comprobación.
 *
 * Presupuesto de local store (256 KB):
 *   entrada   2 × 16 KB    32 KB
 *   salida    2 × 16 KB    32 KB
 *   job + resultado        < 1 KB
 *   código + pila          el build imprime text/data/bss (spu-size)
 *   total                  ~ 70 KB
 */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include "job.h"

#define VECS_PER_CHUNK (SPU_CHUNK_BYTES / sizeof(vec_uint4))

static vec_uint4 in_buf[2][VECS_PER_CHUNK] __attribute__((aligned(128)));
static vec_uint4 out_buf[2][VECS_PER_CHUNK] __attribute__((aligned(128)));
static spu_job_t job __attribute__((aligned(128)));
static spu_result_t result __attribute__((aligned(128)));

static void dma_wait(uint32_t tag_mask)
{
	mfc_write_tag_mask(tag_mask);
	mfc_read_tag_status_all();
}

int main(uint64_t job_ea, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	(void)arg1;
	(void)arg2;
	(void)arg3;

	mfc_get(&job, job_ea, sizeof job, 0, 0, 0);
	dma_wait(1 << 0);

	const vec_uint4 add = spu_splats(SPU_MIX_ADD);
	vec_uint4 sum = spu_splats(0u);
	const uint32_t n = job.chunks;

	/* La etiqueta DMA de cada búfer es su índice (0 o 1): esperar por ella
	 * cubre la llegada de su entrada y la salida del bloque anterior. */
	if (n > 0)
		mfc_get(in_buf[0], job.src_ea, SPU_CHUNK_BYTES, 0, 0, 0);

	for (uint32_t i = 0; i < n; i++) {
		const uint32_t cur = i & 1, nxt = cur ^ 1;

		if (i + 1 < n)
			mfc_get(in_buf[nxt], job.src_ea + (uint64_t)(i + 1) * SPU_CHUNK_BYTES,
				SPU_CHUNK_BYTES, nxt, 0, 0);
		dma_wait(1 << cur);

		for (uint32_t k = 0; k < VECS_PER_CHUNK; k++) {
			const vec_uint4 x = in_buf[cur][k];
			const vec_uint4 y = spu_xor(spu_rl(x, SPU_MIX_ROT), spu_add(x, add));
			out_buf[cur][k] = y;
			sum = spu_add(sum, y);
		}

		mfc_put(out_buf[cur], job.dst_ea + (uint64_t)i * SPU_CHUNK_BYTES,
			SPU_CHUNK_BYTES, cur, 0, 0);
	}
	dma_wait((1 << 0) | (1 << 1));

	result.magic = SPU_RESULT_MAGIC;
	result.index = job.index;
	result.chunks_done = n;
	result.checksum = spu_extract(sum, 0) + spu_extract(sum, 1) +
			  spu_extract(sum, 2) + spu_extract(sum, 3);
	mfc_put(&result, job.result_ea, sizeof result, 0, 0, 0);
	dma_wait(1 << 0);

	spu_thread_exit(0);
	return 0;
}
