/*
 * Salsa64 — backends de la HLE de audio para el núcleo
 * (salsa64_audio_backend).
 *
 * El backend síncrono ejecuta la lista entera al arrancar la tarea, en el
 * hilo del CPU (host, pruebas, y el PPE si el SPE no arranca); el de la
 * PS3 la manda al SPE 1 (src/platform/ps3/audio_spu.c). Los dos avisan al
 * terminar la tarea qué partes de la RDRAM escribió.
 */
#ifndef SALSA64_RSP_AUDIO_BACKEND_H
#define SALSA64_RSP_AUDIO_BACKEND_H

#include "core/bus.h"
#include "rsp/audio/audio.h"

/* Trabajo para una tarea. */
void salsa64_audio_job(struct aud_job *job, const struct salsa64_bus *bus, const struct salsa64_task *task,
		       enum salsa64_abi abi);
/* Avisa al bus de lo que escribió la tarea. */
void salsa64_audio_written(struct salsa64_bus *bus, const struct aud_result *res);

struct salsa64_audio_stats {
	uint32_t tasks;          /* tareas con HLE */
	uint32_t commands;
	uint32_t unknown;        /* comandos sin implementar */
	uint32_t last_unknown;
	uint32_t errors;
};

struct salsa64_audio_sync {
	struct salsa64_audio_backend backend;
	struct aud_state *state;
	const struct salsa64_bus *bus;
	struct aud_job job;
	struct aud_result res;
	bool pending;
	struct salsa64_audio_stats stats;
};

/* `state` lo pone quien llama (~8,7 KB) y queda inicializado. */
void salsa64_audio_sync_init(struct salsa64_audio_sync *s, struct aud_state *state, const struct salsa64_bus *bus);
void salsa64_audio_add_stats(struct salsa64_audio_stats *st, const struct aud_result *res);

#endif
