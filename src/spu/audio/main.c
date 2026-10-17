/* SPDX-License-Identifier: GPL-2.0-or-later */
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
 * build (spu-size; 2026-10-16, con naudio, MP3 y MusyX: text 59 296, data
 * 800, bss 34 960 bytes; sin MusyX eran 46 232, 800 y 34 448):
 *   código y datos de solo lectura       ~58 KB (MusyX, 12,8 KB)
 *   ventana de síntesis del MP3            2 KB
 *   aud_state (DMEM 4 KB + margen 4 KB,
 *     estado de ABI1, nead y naudio,
 *     memoria de trabajo del MP3 2 KB,
 *     tabla de interpolación de MusyX)    12,2 KB
 *   lecturas y escrituras 2 × (4 KB + 32)  8,1 KB
 *   bloque de la lista                     1 KB
 *   paso del estado guardado              11,8 KB
 *   spukit (descriptor, resultado…)      < 1 KB
 *   pila                                  el resto (~167 KB libres; la
 *                                          cadena de llamadas más honda,
 *                                          tools/spu-stack.py, usa 8,4 KB:
 *                                          main → aud_run → musyx_v2_task
 *                                          → voice_stage → aud_load)
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "rsp/audio/hle.h"

_Static_assert(sizeof(struct aud_job) <= SPUKIT_DATA_SIZE, "aud_job no cabe en el descriptor de spukit");
_Static_assert(sizeof(struct aud_result) <= SPUKIT_DATA_SIZE, "aud_result no cabe en el resultado de spukit");

static struct aud_state state __attribute__((aligned(128)));
static struct aud_job job __attribute__((aligned(16)));
/* Estados guardados: el estado en el formato portable, de paso al DMA. */
static uint8_t blob[AUD_STATE_BYTES] __attribute__((aligned(128)));

static int handler(void *ctx, const spukit_job *j, spukit_result *res)
{
	(void)ctx;
	if (j->kind == AUD_JOB_RESET) {
		aud_init(&state);
		return 0;
	}
	if (j->kind == AUD_JOB_STATE_SAVE || j->kind == AUD_JOB_STATE_LOAD) {
		struct aud_state_job sj;
		memcpy(&sj, j->data, sizeof sj);
		if (sj.ea & 127)
			return -1;
		if (j->kind == AUD_JOB_STATE_SAVE) {
			aud_state_save(&state, blob);
			spukit_dma_put(blob, sj.ea, sizeof blob, 0);
			spukit_dma_wait(1u << 0);
			return 0;
		}
		spukit_dma_get(blob, sj.ea, sizeof blob, 0);
		spukit_dma_wait(1u << 0);
		return aud_state_load(&state, blob) ? 0 : -1;
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
