/*
 * Salsa64 — Fase 2: trabajador SPU de prueba para lib/spukit.
 *
 * JOB_SCALE transforma un búfer en bloques de 16 KB con doble búfer (llega
 * el bloque siguiente mientras se procesa el actual) y devuelve la suma.
 *
 * Presupuesto de local store (256 KB):
 *   entrada   2 × 16 KB    32 KB
 *   salida    2 × 16 KB    32 KB
 *   runtime de spukit      < 1 KB (descriptor, resultado, cabeza, estado)
 *   código + pila          el build imprime text/data/bss (spu-size)
 *   total                  ~ 70 KB
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "../job.h"

#define CHUNK_BYTES 16384u
#define CHUNK_VECS  (CHUNK_BYTES / 16)

static vec_uint4 in_buf[2][CHUNK_VECS] __attribute__((aligned(128)));
static vec_uint4 out_buf[2][CHUNK_VECS] __attribute__((aligned(128)));
static uint32_t worker_id;

static int scale(const job_scale *p, job_scale_result *r)
{
	const uint32_t bytes = p->words * 4;
	const uint32_t n = (bytes + CHUNK_BYTES - 1) / CHUNK_BYTES;
	const vec_uint4 mul = spu_splats(p->mul), add = spu_splats(p->add);
	vec_uint4 sum = spu_splats(0u);

	if ((p->src_ea | p->dst_ea | bytes) & 15)
		return -2;
	/* Etiqueta = índice del búfer: esperarla cubre su entrada y la salida
	 * del bloque anterior que usó el mismo búfer. */
	if (n)
		spukit_dma_get(in_buf[0], p->src_ea, bytes < CHUNK_BYTES ? bytes : CHUNK_BYTES, 0);
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t cur = i & 1, nxt = cur ^ 1;
		const uint32_t off = i * CHUNK_BYTES;
		const uint32_t len = bytes - off < CHUNK_BYTES ? bytes - off : CHUNK_BYTES;
		if (i + 1 < n) {
			uint32_t noff = off + CHUNK_BYTES;
			uint32_t nlen = bytes - noff < CHUNK_BYTES ? bytes - noff : CHUNK_BYTES;
			spukit_dma_get(in_buf[nxt], p->src_ea + noff, nlen, nxt);
		}
		spukit_dma_wait(1u << cur);
		for (uint32_t k = 0; k < len / 16; k++) {
			/* x * mul con multiplicaciones de 16 bits (el SPU no tiene mul de 32). */
			vec_uint4 x = in_buf[cur][k];
			vec_uint4 lo = spu_mulo((vec_ushort8)x, (vec_ushort8)mul);
			vec_uint4 c1 = spu_mulo((vec_ushort8)spu_rl(x, 16), (vec_ushort8)mul);
			vec_uint4 c2 = spu_mulo((vec_ushort8)x, (vec_ushort8)spu_rl(mul, 16));
			vec_uint4 y = spu_add(spu_add(lo, spu_sl(spu_add(c1, c2), 16)), add);
			out_buf[cur][k] = y;
			sum = spu_add(sum, y);
		}
		spukit_dma_put(out_buf[cur], p->dst_ea + off, len, cur);
	}
	r->sum = spu_extract(sum, 0) + spu_extract(sum, 1) + spu_extract(sum, 2) + spu_extract(sum, 3);
	r->words = p->words;
	r->worker = worker_id;
	return 0;
}

static int handler(void *ctx, const spukit_job *job, spukit_result *res)
{
	(void)ctx;
	switch (job->kind) {
	case JOB_SCALE:
		return scale((const job_scale *)job->data, (job_scale_result *)res->data);
	case JOB_PING:
		return 0;
	default:
		return -1;
	}
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)a2, (void)a3;
	worker_id = (uint32_t)arg;
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
