/*
 * Salsa64 — programa SPU de la HLE de audio (SPE 1).
 *
 * Trabajador de lib/spukit: cada trabajo es una tarea de audio completa
 * (struct aud_job: la lista de comandos de un cuadro de audio); la ejecuta
 * con el mismo código que el host (src/rsp/audio) y una E/S por DMA
 * (io_spu.c), y devuelve un struct aud_result. El estado de cada ABI
 * persiste entre tareas, como en el host.
 *
 * Presupuesto de local store (256 KB); los números exactos los imprime el
 * build (spu-size; 2026-10-10: text 27 432, data 832, bss 19 344 bytes):
 *   código (HLE + runtime)               27 KB
 *   aud_state (DMEM 4 KB + margen 4 KB,
 *     estado de ABI1 y nead)               8,7 KB
 *   lecturas y escrituras 2 × (4 KB + 32)  8,1 KB
 *   bloque de la lista                     1 KB
 *   spukit (descriptor, resultado…)      < 1 KB
 *   pila                                  el resto (~208 KB; la HLE usa
 *                                          < 2 KB)
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "rsp/audio/hle.h"

_Static_assert(sizeof(struct aud_job) <= SPUKIT_DATA_SIZE, "aud_job no cabe en el descriptor de spukit");
_Static_assert(sizeof(struct aud_result) <= SPUKIT_DATA_SIZE, "aud_result no cabe en el resultado de spukit");

static struct aud_state state __attribute__((aligned(128)));
static struct aud_job job __attribute__((aligned(16)));

static int handler(void *ctx, const spukit_job *j, spukit_result *res)
{
	(void)ctx;
	if (j->kind == AUD_JOB_RESET) {
		aud_init(&state);
		return 0;
	}
	if (j->kind != AUD_JOB_TASK)
		return -1;
	memcpy(&job, j->data, sizeof job);
	aud_run(&state, &job, (struct aud_result *)res->data);
	return 0;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg, (void)a2, (void)a3;
	aud_init(&state);
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
