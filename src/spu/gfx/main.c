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
 * build (spu-size: texto 59 KB, datos 0,8 KB, bss 144 KB):
 *   código (HLE + runtime)              ~ 59 KB (F3D y F3DEX: 3,3 KB)
 *   gfx_state (RDP, TMEM 4 KB, matrices,
 *     búfer de vértices, texturas de la
 *     tarea, directorio de la caché de
 *     texturas 32 KB)                      ~ 53 KB
 *   lecturas por DMA (vértices, matrices,
 *     luces, cargas de TMEM)               ~ 5,8 KB
 *   display list 2 × 1 KB                    2 KB
 *   vértices 2 × 8 KB, comandos 2 × 4 KB    24 KB
 *   texturas 2 × 32 KB                      64 KB
 *   spukit (descriptor, resultado…)        < 1 KB
 *   pila                                    el resto (~ 52 KB)
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "rsp/gfx/hle.h"

static struct gfx_state state __attribute__((aligned(128)));
static struct gfx_job job __attribute__((aligned(16)));

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
