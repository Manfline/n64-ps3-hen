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
 * Se enlaza una vez por imagen (-DGFX_SPE0_IMG=<n>, rsp/gfx/spe0.h): cada una
 * con el código común y solo sus familias de microcódigos; el PPE carga la de
 * cada tarea (docs/spe0-familias.md).
 *
 * Presupuesto de local store (256 KB) de cada imagen; los números exactos
 * los imprime el build (spu-size y tools/spu-stack.py). 2026-10-17, con la
 * caché de texturas en 1024 entradas en las tres:
 *   imagen 0 (F3D, F3DEX 1.x y las de Rare sobre F3D): texto 92,3 KB,
 *     hasta _end 238 992 bytes, holgura de pila 14 080
 *   imagen 1 (F3DEX2, F3DZEX2, S2DEX2 y F3DFLX): texto 92,8 KB, hasta _end
 *     239 632 bytes, holgura 13 024
 *   imagen 2 (Conker): texto 91,5 KB, hasta _end 238 352 bytes, holgura
 *     14 784
 *   imagen 3 (Factor 5: Rogue Squadron y lo común de F3D): texto 101,9 KB
 *     (14 KB son de f5rogue.c con -Os), hasta _end 248 720 bytes, holgura
 *     3 472 (2026-10-18)
 *   imagen 4 (Factor 5: Indiana Jones y Battle for Naboo; sin F3D): texto
 *     118,5 KB (40 KB de f5indy.c con -Os, 18 KB de ellos de Naboo), bss
 *     125,7 KB con la caché de texturas en 224 entradas y la DMEM de 4 KB de
 *     f5indy.c, hasta _end 251 040 bytes, holgura 1 808 (2026-10-18)
 * Lo de todas:
 *   código común (recorrido, geometría, RDP, combiner con -Os 11 KB, TMEM,
 *     caché de texturas, interpolación, listas del CPU, runtime) ~ 83 KB
 *   gfx_state (RDP, TMEM 4 KB, matrices, búfer de vértices, 32 texturas del
 *     arena de la tarea 1,5 KB, directorio de la caché de texturas 32,7 KB
 *     con 1024 entradas, luces de Conker, último resultado del combiner
 *     0,4 KB, color images dibujadas 128 bytes)  ~ 47 KB
 *   lecturas por DMA (vértices, matrices, luces, cargas de TMEM) ~ 5,8 KB
 *   display list 2 × 1 KB                    2 KB
 *   vértices 2 × 8 KB, comandos 2 × 4 KB    24 KB
 *   texturas 2 × 32 KB                      64 KB
 *   spukit (descriptor, resultado…)        < 1 KB
 *   trabajo de los estados guardados y de GFX_JOB_INFO ~ 0,3 KB
 *   pila                                    el resto (de _end a __stack);
 *                                         la cadena más honda usa 9–9,5
 *                                         KB según -fstack-usage y el
 *                                         grafo de llamadas del binario.
 *                                         El enlace falla si la holgura
 *                                         baja de 1,5 KB (gfx<n>_STACK_MIN
 *                                         en src/frontend/Makefile): pasada
 *                                         la pila, el SPE pisa sin aviso
 *                                         las lecturas por DMA del final
 *                                         del bss. GFX_JOB_INFO dice cuánto
 *                                         no se tocó nunca (STACK_FILL).
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "rsp/gfx/hle.h"

#ifndef GFX_SPE0_IMG
#error "src/spu/gfx se compila una vez por imagen, con -DGFX_SPE0_IMG=<n> (rsp/gfx/spe0.h)"
#endif

static struct gfx_state state __attribute__((aligned(128)));
static struct gfx_job job __attribute__((aligned(16)));

/* Patrón con que se llena la pila libre al arrancar si el PPE lo pide (arg
 * 1: el archivo spe0-pila, gfx_spu.c), para GFX_JOB_INFO. Sin pedirlo no se
 * llena: con la pila libre llena de algo distinto de cero, el recompilador
 * de SPU de RPCS3 ejecutaba mal la imagen de F3D en algunos builds (con su
 * intérprete, bien; docs/spe0-familias.md). Sin llenar se cuentan las
 * palabras en cero (aproximado). */
#define STACK_FILL 0x5A5A5A5Au
static uint32_t stack_fill;
extern char _end[], __stack[];

/* Estados guardados, entre tareas: el tramo con lo que persiste (registros
 * del RDP, tiles y TMEM) tal cual, en una DMA (< 16 KB); el PPE lo pasa al
 * formato portable. Contesta dónde está el tramo para que el PPE compruebe
 * la disposición. Al cambiar de imagen, también las color images dibujadas
 * y, al cargar, la barrera de la caché de texturas (gfx.h). Aparte y chica:
 * el local store no sobra. */
static __attribute__((noinline, cold)) int state_job(bool save, const struct gfx_state_job *sj, spukit_result *res)
{
	uint8_t *p = (uint8_t *)&state + GFX_RAW_START;
	uint32_t n = GFX_RAW_END - GFX_RAW_START;
	uint32_t *where = (uint32_t *)(void *)res->data;
	where[0] = GFX_RAW_START;
	where[1] = GFX_RAW_END;
	if (save)
		mfc_put(p, sj->ea, n, 0, 0, 0);
	else
		mfc_get(p, sj->ea, n, 0, 0, 0);
	if (sj->fb_ea) {
		if (save)
			mfc_put(state.fbimg, sj->fb_ea, GFX_FB_BYTES, 0, 0, 0);
		else
			mfc_get(state.fbimg, sj->fb_ea, GFX_FB_BYTES, 0, 0, 0);
	}
	spukit_dma_wait(1u << 0);
	if (!save && sj->fb_ea && sj->cache_ea)
		gfx_tc_fence(&state, sj->cache_ea, sj->cache_cap, sj->fence_task);
	return 0;
}

static __attribute__((noinline, cold)) int info_job(spukit_result *res)
{
	struct gfx_info *in = (struct gfx_info *)(void *)res->data;
	const uint32_t *p = (const uint32_t *)(void *)(((uintptr_t)_end + 15) & ~(uintptr_t)15);
	const uint32_t *top = (const uint32_t *)(void *)__stack;
	uint32_t n = 0;
	while (p + n < top && p[n] == stack_fill)
		n++;
	in->image = GFX_SPE0_IMG;
	in->tc_entries = GFX_TC_ENTRIES;
	in->end = (uint32_t)(uintptr_t)_end;
	in->stack_top = (uint32_t)(uintptr_t)__stack;
	in->untouched = 4 * n;
	in->raw_start = GFX_RAW_START;
	in->raw_end = GFX_RAW_END;
	in->filled = stack_fill != 0;
	return 0;
}

/* La pila libre, con el patrón: lo que siga con él al pedir GFX_JOB_INFO
 * no lo usó nunca nadie. Desde _end hasta 1 KB bajo este marco. */
static __attribute__((noinline, cold)) void fill_stack(void)
{
	volatile uint32_t here = 0;
	uint32_t *p = (uint32_t *)(void *)(((uintptr_t)_end + 15) & ~(uintptr_t)15);
	uint32_t *stop = (uint32_t *)(void *)(((uintptr_t)&here - 1024) & ~(uintptr_t)15);
	while (p < stop)
		*p++ = STACK_FILL;
	(void)here;
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
		return state_job(j->kind == GFX_JOB_STATE_SAVE, (const struct gfx_state_job *)(const void *)j->data, res);
	if (j->kind == GFX_JOB_INFO)
		return info_job(res);
	if (j->kind != GFX_JOB_TASK)
		return -1;
	memcpy(&job, j->data, sizeof job);
	/* Una tarea de una familia que esta imagen no trae (el PPE no la
	 * manda): error, sin tocar nada. */
	if (!(job.flags & GFX_JOB_DPC) && gfx_spe0_image(job.gbi) != GFX_SPE0_IMG)
		return -2;
	gfx_run(&state, &job, (struct gfx_result *)res->data);
	return 0;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)a2, (void)a3;
	if (arg & 1) {
		stack_fill = STACK_FILL;
		fill_stack();
	}
	gfx_init(&state);
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
