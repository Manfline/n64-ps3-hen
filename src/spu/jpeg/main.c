/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — programa SPU de la tarea JPEG (fondos prerrenderizados de
 * Zelda OoT), en uno de los SPEs libres.
 *
 * Trabajador de lib/spukit: cada trabajo es una tarea JPEG completa (struct
 * jpeg_job: dónde está la RDRAM y la estructura de la tarea). Trae la
 * estructura y las tres tablas de cuantización, y los macrobloques con
 * doble búfer: mientras uno se decodifica llega el siguiente y sale el
 * anterior, a su mismo lugar de la RDRAM. Devuelve un struct
 * jpeg_job_result, igual al de salsa64_jpeg_run. La decodificación es la
 * versión en 4 carriles (src/rsp/jpeg/jpeg_v.c): lo mismo byte a byte que
 * la escalar del PPE y el host (prueba unitaria y prueba del arranque),
 * unas 15 veces menos operaciones.
 *
 * Etiquetas DMA: 0 y 1 entrada de cada búfer, 2 y 3 su salida, 4 la
 * estructura y las tablas. La 31 es de spukit.
 *
 * Presupuesto de local store (256 KB); los números exactos los imprime el
 * build (spu-size):
 *   código (jpeg.c, jpeg_v.c, bucle)     ~12 KB
 *   constantes de la IDCT (jpeg_v.c)      1,3 KB
 *   macrobloques 2 × 768 + 2 × 512        2,5 KB
 *   estructura y tablas (3 × 160 + 64)   < 1 KB
 *   spukit (descriptor, resultado…)      < 1 KB
 *   tablas de cuantización preparadas    0,75 KB
 *   pila                                 el resto (un macrobloque usa
 *                                        ~2 KB: 6 bloques de 16 v4 y dos
 *                                        de trabajo)
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "rsp/jpeg/jpeg.h"
#include "rsp/jpeg/jpeg_v.h"
#include "rsp/jpeg/job.h"

#define TAG_IN(b)  (b)
#define TAG_OUT(b) (2u + (b))
#define TAG_PARAM  4u

_Static_assert(sizeof(struct jpeg_job) <= SPUKIT_DATA_SIZE, "jpeg_job no cabe en el descriptor de spukit");
_Static_assert(sizeof(struct jpeg_job_result) <= SPUKIT_RESULT_SIZE, "jpeg_job_result no cabe en el resultado");

static uint8_t in_buf[2][SALSA64_JPEG_MB_IN(2)] __attribute__((aligned(128)));
static uint8_t out_buf[2][SALSA64_JPEG_MB_OUT(2)] __attribute__((aligned(128)));
static uint8_t task_buf[SALSA64_JPEG_TASK_BYTES + 32] __attribute__((aligned(16)));
static uint8_t q_buf[3][128 + 32] __attribute__((aligned(16)));
static struct salsa64_jpeg_qv qv;

static bool in_rdram(const struct jpeg_job *j, uint32_t addr, uint32_t size)
{
	return addr <= j->rdram_size && size <= j->rdram_size - addr;
}

static int run(const struct jpeg_job *j, struct jpeg_job_result *res)
{
	struct salsa64_jpeg_task t;
	uint32_t data = j->data_ptr & 0x1FFFFFFF;
	if (j->kind != SALSA64_JPEG_OOT)
		return JPEG_ERR_KIND;
	if (!in_rdram(j, data, SALSA64_JPEG_TASK_BYTES)) {
		res->bad = 1;
		return 0;
	}
	if (!salsa64_jpeg_task_read(spukit_get_unaligned(task_buf, j->rdram_ea + data, SALSA64_JPEG_TASK_BYTES,
							 TAG_PARAM),
				    &t)) {
		res->bad = 1;
		return 0;
	}
	int16_t q[3][64];
	for (int i = 0; i < 3; i++) {
		if (!in_rdram(j, t.qaddr[i], 128)) {
			res->bad = 1;
			return 0;
		}
		salsa64_jpeg_qtable(spukit_get_unaligned(q_buf[i], j->rdram_ea + t.qaddr[i], 128, TAG_PARAM), q[i]);
	}
	salsa64_jpeg_qv_init(&qv, q);
	if ((t.addr & 15) || (j->rdram_ea & 15))
		return JPEG_ERR_ALIGN;  /* que la haga el PPE */
	uint32_t in_bytes = SALSA64_JPEG_MB_IN(t.mode), out_bytes = SALSA64_JPEG_MB_OUT(t.mode);
	res->out_addr = t.addr;
	res->mb_stride = in_bytes;
	res->mb_out = out_bytes;
	/* Los que caben en la RDRAM, como salsa64_jpeg_run: se para en el
	 * primero que se sale. */
	uint32_t n = 0;
	while (n < t.count && in_rdram(j, t.addr + n * in_bytes, in_bytes))
		n++;
	if (n < t.count)
		res->bad = 1;
	if (n)
		spukit_dma_get(in_buf[0], j->rdram_ea + t.addr, in_bytes, TAG_IN(0));
	for (uint32_t m = 0; m < n; m++) {
		unsigned b = m & 1;
		uint64_t ea = j->rdram_ea + t.addr + m * in_bytes;
		if (m + 1 < n)
			spukit_dma_get(in_buf[b ^ 1], ea + in_bytes, in_bytes, TAG_IN(b ^ 1));
		spukit_dma_wait(1u << TAG_IN(b) | 1u << TAG_OUT(b));  /* el suyo llegó y la salida de m − 2 salió */
		salsa64_jpeg_mb_v(t.mode, &qv, in_buf[b], out_buf[b]);
		spukit_dma_put(out_buf[b], ea, out_bytes, TAG_OUT(b));
	}
	spukit_dma_wait(1u << TAG_OUT(0) | 1u << TAG_OUT(1));
	res->macroblocks = n;
	res->out_bytes = n ? (n - 1) * in_bytes + out_bytes : 0;
	return 0;
}

static int handler(void *ctx, const spukit_job *j, spukit_result *res)
{
	(void)ctx;
	struct jpeg_job job;
	struct jpeg_job_result r;
	if (j->kind != JPEG_JOB_TASK)
		return JPEG_ERR_KIND;
	memcpy(&job, j->data, sizeof job);
	memset(&r, 0, sizeof r);
	int err = run(&job, &r);
	memcpy(res->data, &r, sizeof r);
	return err;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg, (void)a2, (void)a3;
	salsa64_jpeg_v_init();
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
