/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — programa SPU del RSP a bajo nivel (Fase 6).
 *
 * Trabajador de lib/spukit: cada trabajo (struct lle_job) ejecuta el
 * microcódigo real con el intérprete portable de src/rsp/lle sobre la DMEM
 * y la IMEM copiadas al local store, hasta que el RSP se detiene o se
 * agota el cupo de instrucciones; la E/S de COP0 (DMA con la RDRAM,
 * SP_STATUS, semáforo, registros del DP) va por io_spu.c. Devuelve un
 * struct lle_result; los tramos entregados al RDP y la interrupción del SP
 * los aplica el PPE al recogerlo (src/platform/ps3/rsp_spu.c).
 *
 * Presupuesto de local store (256 KB); los números exactos los imprime el
 * build (spu-size):
 *   código (intérprete + runtime)        ~ 40 KB
 *   rsp_lle (registros, tablas)           ~ 3 KB
 *   DMEM + IMEM                             8 KB
 *   búferes de DMA (lectura, escritura,
 *     captura xbus, línea atómica)        ~ 13 KB
 *   entradas del RDP (256 × 16)             4 KB
 *   spukit (descriptor, resultado…)       < 1 KB
 *   pila                                   el resto
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "spu/rsp/io_spu.h"

_Static_assert(sizeof(struct lle_job) <= SPUKIT_DATA_SIZE, "lle_job no cabe en el descriptor de spukit");
_Static_assert(sizeof(struct lle_result) <= SPUKIT_RESULT_SIZE, "lle_result no cabe en el resultado de spukit");

static struct rsp_lle rsp __attribute__((aligned(128)));
static uint8_t mem[2 * LLE_MEM_SIZE] __attribute__((aligned(128)));
static struct lle_job job __attribute__((aligned(16)));

static int handler(void *ctx, const spukit_job *j, spukit_result *res)
{
	(void)ctx;
	if (j->kind == LLE_JOB_RESET) {
		lle_reset(&rsp);
		return 0;
	}
	if (j->kind != LLE_JOB_RUN)
		return -1;
	memcpy(&job, j->data, sizeof job);
	return lle_spu_run(&rsp, &job, (struct lle_result *)res->data);
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg, (void)a2, (void)a3;
	lle_init(&rsp, mem, mem + LLE_MEM_SIZE);
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
