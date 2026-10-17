/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — programa SPU de la HLE gráfica (SPE 0).
 *
 * Trabajador de lib/spukit: cada trabajo es una tarea gráfica completa
 * (struct gfx_job: la display list de un cuadro y los búferes de salida);
 * la ejecuta con el mismo código que el host (src/rsp/gfx) y una E/S por
 * DMA (io_spu.c), y devuelve un struct gfx_result. El estado del RDP y la
 * TMEM viven en el local store y persisten entre tareas.
 *
 * Presupuesto de local store (256 KB); los números exactos los imprime el
 * build (spu-size y tools/spu-stack.py, 2026-10-12, con el combiner y
 * S2DEX en -Os, el resultado del combiner memorizado y las texturas que se
 * leen de un destino sin decodificar: texto 103,5 KB, datos 0,8 KB, bss
 * 138,8 KB; con Diddy Kong Racing y la caché de 768 entradas, 2026-10-17:
 * texto 108,2 KB, datos 0,8 KB, bss 135,4 KB y 11,5 KB libres):
 *   código (HLE + runtime)              ~ 99 KB (F3D y F3DEX: 3,6 KB;
 *                                         Perfect Dark 1,9 KB, Diddy Kong
 *                                         Racing 2,9 KB y Conker
 *                                         4,4 KB; el combiner con sus
 *                                         términos para el generador de
 *                                         fragment programs, 11 KB con -Os
 *                                         y 16,8 KB con -O2; el tramo S2DEX
 *                                         de los fondos de Zelda OoT,
 *                                         3,9 KB con -Os)
 *   gfx_state (RDP, TMEM 4 KB, matrices,
 *     búfer de vértices, 32 texturas del
 *     arena de la tarea 1,5 KB, directorio
 *     de la caché de texturas 25 KB con
 *     768 entradas (antes 28,5), luces
 *     de Conker, último resultado del
 *     combiner 0,4 KB, color images
 *     dibujadas 128 bytes)                 ~ 41 KB
 *   lecturas por DMA (vértices, matrices,
 *     luces, cargas de TMEM)               ~ 5,8 KB
 *   display list 2 × 1 KB                    2 KB
 *   vértices 2 × 8 KB, comandos 2 × 4 KB    24 KB
 *   texturas 2 × 32 KB                      64 KB
 *   spukit (descriptor, resultado…)        < 1 KB
 *   trabajo de los estados guardados        ~ 0,2 KB
 *   pila                                    el resto (12,7 KB: de _end a
 *                                         __stack); la cadena más honda,
 *                                         fondo S2DEX → TEXRECT → estado →
 *                                         combiner, usa 9,3 KB según
 *                                         -fstack-usage y el grafo de
 *                                         llamadas del binario: holgura
 *                                         3,4 KB. El enlace falla si baja de
 *                                         1,5 KB (gfx_STACK_MIN en
 *                                         src/frontend/Makefile): pasada la
 *                                         pila, el SPE pisa sin aviso las
 *                                         lecturas por DMA del final del bss.
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "rsp/gfx/hle.h"

static struct gfx_state state __attribute__((aligned(128)));
static struct gfx_job job __attribute__((aligned(16)));

/* Estados guardados, entre tareas: el tramo con lo que persiste (registros
 * del RDP, tiles y TMEM) tal cual, en una DMA (< 16 KB); el PPE lo pasa al
 * formato portable. Contesta dónde está el tramo para que el PPE compruebe
 * la disposición. Aparte y chica: el local store no sobra. */
static __attribute__((noinline, cold)) int state_job(bool save, uint64_t ea, spukit_result *res)
{
	uint8_t *p = (uint8_t *)&state + GFX_RAW_START;
	uint32_t n = GFX_RAW_END - GFX_RAW_START;
	uint32_t *where = (uint32_t *)(void *)res->data;
	where[0] = GFX_RAW_START;
	where[1] = GFX_RAW_END;
	if (save)
		mfc_put(p, ea, n, 0, 0, 0);
	else
		mfc_get(p, ea, n, 0, 0, 0);
	spukit_dma_wait(1u << 0);
	return 0;
}

static int handler(void *ctx, const spukit_job *j, spukit_result *res)
{
	(void)ctx;
	if (j->kind == GFX_JOB_HASH) {
		/* Antes de cualquier tarea: la TMEM sirve de búfer y queda en cero. */
		struct gfx_hash_job hj;
		memcpy(&hj, j->data, sizeof hj);
		if (hj.size > sizeof state.tmem || (hj.size & (hj.size - 1)) || hj.size < 16)
			return -1;
		spukit_dma_get(state.tmem, hj.ea, hj.size, 0);
		spukit_dma_wait(1u << 0);
		struct gfx_hash h;
		gfx_hash_init(&h);
		gfx_hash_ring(&h, state.tmem, hj.size, hj.start, hj.len);
		gfx_hash_final(&h, (uint32_t *)res->data);
		memset(state.tmem, 0, sizeof state.tmem);
		return 0;
	}
	if (j->kind == GFX_JOB_STATE_SAVE || j->kind == GFX_JOB_STATE_LOAD)
		return state_job(j->kind == GFX_JOB_STATE_SAVE, ((const struct gfx_state_job *)(const void *)j->data)->ea,
				 res);
	if (j->kind != GFX_JOB_TASK)
		return -1;
	memcpy(&job, j->data, sizeof job);
	gfx_run(&state, &job, (struct gfx_result *)res->data);
	return 0;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg, (void)a2, (void)a3;
	gfx_init(&state);
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
