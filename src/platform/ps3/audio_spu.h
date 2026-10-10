/*
 * Salsa64 — backend de la HLE de audio en un SPE (PS3): cada tarea de audio
 * del RSP se publica como un trabajo de lib/spukit al programa src/spu/audio
 * y se espera cuando el núcleo da la tarea por terminada (un evento del
 * planificador); ahí se avisa al bus qué partes de la RDRAM escribió. Entre
 * medias el PPE sigue emulando el CPU.
 */
#ifndef SALSA64_PLATFORM_PS3_AUDIO_SPU_H
#define SALSA64_PLATFORM_PS3_AUDIO_SPU_H

#include <spukit/spukit.h>

#include "rsp/audio/backend.h"

struct salsa64_audio_spu {
	struct salsa64_audio_backend backend;
	spukit_worker *worker;
	const struct salsa64_bus *bus;
	struct aud_job job;
	uint32_t seq;
	bool pending;
	/* Estadísticas desde el arranque. */
	struct salsa64_audio_stats stats;
	uint64_t spu_ticks;  /* tiempo de la HLE en el SPE (ticks del timebase) */
	uint64_t spu_max;    /* la tarea más larga */
	uint64_t wait_us;    /* PPE esperando al SPE al terminar una tarea */
	uint32_t waits;
	uint32_t failed;     /* trabajos que el SPE devolvió con error */
	uint32_t polls;      /* sondeos con la tarea sin terminar */
	uint64_t sleep_us;   /* dormido en sondeos con el CPU en bucle de espera */
};

/* Arranca el trabajador (spukit_init ya llamado). Devuelve 0 o el error de
 * spukit. */
int salsa64_audio_spu_init(struct salsa64_audio_spu *s, const struct salsa64_bus *bus);
void salsa64_audio_spu_shutdown(struct salsa64_audio_spu *s);

#endif
