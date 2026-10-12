/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — backend de la HLE de audio en un SPE. Ver audio_spu.h.
 */
#include "platform/ps3/audio_spu.h"

#include <string.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "platform/ps3/log.h"
#include "rsp/audio/selftest.h"
#include "spu_audio_bin.h"

static void spu_start(void *ctx, const struct salsa64_task *task, enum salsa64_abi abi)
{
	struct salsa64_audio_spu *s = ctx;
	salsa64_audio_job(&s->job, s->bus, task, abi);
	s->seq = spukit_post(s->worker, AUD_JOB_TASK, &s->job, sizeof s->job);
	s->pending = true;
}

#define POLL_SLEEP_US 100

/* Sondeo del fin de la tarea (rsp.c): si el SPE no terminó, el núcleo
 * vuelve a preguntar más tarde y el CPU sigue emulando. Si el CPU está en
 * un bucle de espera, se duerme un poco antes de contestar para que el
 * reloj emulado no se adelante al real (como el backend gráfico). */
static bool spu_done(void *ctx, bool idle)
{
	struct salsa64_audio_spu *s = ctx;
	if (!s->pending || spukit_done(s->worker, s->seq))
		return true;
	s->polls++;
	if (idle) {
		sysUsleep(POLL_SLEEP_US);
		s->sleep_us += POLL_SLEEP_US;
		return spukit_done(s->worker, s->seq);
	}
	return false;
}

/* Espera la tarea en vuelo; avisa al bus de lo que escribió si `bus`. */
static void spu_finish(void *ctx, struct salsa64_bus *bus)
{
	struct salsa64_audio_spu *s = ctx;
	if (!s->pending)
		return;
	s->pending = false;
	uint64_t t0 = 0;
	if (!spukit_done(s->worker, s->seq)) {
		t0 = sysGetSystemTime();
		s->waits++;
	}
	const spukit_result *r = spukit_wait(s->worker, s->seq);
	if (t0)
		s->wait_us += sysGetSystemTime() - t0;
	s->spu_ticks += r->ticks;
	if (r->ticks > s->spu_max)
		s->spu_max = r->ticks;
	if (r->error) {
		s->failed++;
		return;
	}
	struct aud_result res;
	memcpy(&res, r->data, sizeof res);
	salsa64_audio_add_stats(&s->stats, &res);
	if (bus)
		salsa64_audio_written(bus, &res);
}

/* Estados guardados: el SPE deja o toma el estado de los ABI en un búfer
 * propio (alineado para el DMA), sin tarea en vuelo. */
static uint8_t state_buf[AUD_STATE_BYTES] __attribute__((aligned(128)));

static bool spu_state(struct salsa64_audio_spu *s, uint32_t kind)
{
	if (s->pending || !s->worker)
		return false;
	struct aud_state_job sj = { (uint64_t)(uintptr_t)state_buf };
	const spukit_result *r = spukit_wait(s->worker, spukit_post(s->worker, kind, &sj, sizeof sj));
	if (r->error)
		salsa64_log("estado de la HLE de audio: el SPE no lo dio (error %d)", r->error);
	return !r->error;
}

static bool spu_save_state(void *ctx, uint8_t *buf)
{
	if (!spu_state(ctx, AUD_JOB_STATE_SAVE))
		return false;
	memcpy(buf, state_buf, sizeof state_buf);
	return true;
}

static bool spu_load_state(void *ctx, const uint8_t *buf)
{
	memcpy(state_buf, buf, sizeof state_buf);
	return spu_state(ctx, AUD_JOB_STATE_LOAD);
}

/* Al arrancar: listas de prueba (selftest.c) en el SPE y en el PPE sobre
 * copias iguales de una RDRAM de mentira; deben quedar iguales. Además, en
 * RPCS3, el programa del SPE se compila aquí y no en la primera tarea del
 * juego. Devuelve cuántas listas difieren; el estado del SPE queda en cero. */
static int check_lists(spukit_worker *w)
{
	static uint8_t ram_spe[AUD_SELFTEST_RAM] __attribute__((aligned(128)));
	static uint8_t ram_ppe[AUD_SELFTEST_RAM] __attribute__((aligned(128)));
	static struct aud_state ppe;
	static const uint32_t abis[] = { SALSA64_ABI_NEAD_OOT, SALSA64_ABI_AUDIO, SALSA64_ABI_NEAD_MK,
					 SALSA64_ABI_NAUDIO_MP3, SALSA64_ABI_NAUDIO_BK };
	int bad = 0;
	aud_init(&ppe);
	spukit_wait(w, spukit_post(w, AUD_JOB_RESET, NULL, 0));
	for (unsigned i = 0; i < sizeof abis / sizeof abis[0]; i++) {
		struct aud_job job;
		struct aud_result res_spe, res_ppe;
		memset(&job, 0, sizeof job);
		job.rdram_size = AUD_SELFTEST_RAM;
		job.abi = abis[i];
		job.alist_size = aud_selftest_build(ram_spe, abis[i], 12345u + i);
		memcpy(ram_ppe, ram_spe, sizeof ram_ppe);
		job.rdram_ea = (uint64_t)(uintptr_t)ram_spe;
		const spukit_result *r = spukit_wait(w, spukit_post(w, AUD_JOB_TASK, &job, sizeof job));
		memcpy(&res_spe, r->data, sizeof res_spe);
		job.rdram_ea = (uint64_t)(uintptr_t)ram_ppe;
		aud_run(&ppe, &job, &res_ppe);
		uint32_t diff = 0, first = 0;
		for (uint32_t k = 0; k < AUD_SELFTEST_RAM; k++)
			if (ram_spe[k] != ram_ppe[k] && !diff++)
				first = k;
		if (r->error || diff || res_spe.commands != res_ppe.commands || res_spe.errors || res_ppe.errors) {
			salsa64_log("audio: lista de prueba %s: el SPE difiere del PPE (%u bytes, el primero en 0x%04X; "
				    "%u/%u comandos, %u/%u errores)", salsa64_abi_name(abis[i]), diff, first,
				    res_spe.commands, res_ppe.commands, res_spe.errors, res_ppe.errors);
			bad++;
		}
	}
	spukit_wait(w, spukit_post(w, AUD_JOB_RESET, NULL, 0));
	return bad;
}

int salsa64_audio_spu_init(struct salsa64_audio_spu *s, const struct salsa64_bus *bus)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = spu_start;
	s->backend.finish = spu_finish;
	s->backend.done = spu_done;
	s->backend.state_size = AUD_STATE_BYTES;
	s->backend.save_state = spu_save_state;
	s->backend.load_state = spu_load_state;
	s->bus = bus;
	s->worker = spukit_worker_start(spu_audio_bin, "salsa64_audio", 0);
	if (!s->worker)
		return spukit_last_error();
	uint64_t t0 = sysGetSystemTime();
	int bad = check_lists(s->worker);
	salsa64_log("audio: listas de prueba en el SPE: %s (%.1f ms)", bad ? "DIFIEREN del PPE" : "iguales al PPE",
		    (sysGetSystemTime() - t0) / 1e3);
	return 0;
}

int salsa64_audio_spu_shutdown(struct salsa64_audio_spu *s)
{
	int rc = 0;
	if (s->pending)
		spu_finish(s, NULL);
	if (s->worker)
		rc = spukit_worker_stop(s->worker);
	s->worker = NULL;
	return rc;
}
