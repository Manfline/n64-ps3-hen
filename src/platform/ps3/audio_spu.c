/*
 * Salsa64 — backend de la HLE de audio en un SPE. Ver audio_spu.h.
 */
#include "platform/ps3/audio_spu.h"

#include <string.h>
#include <sys/systime.h>
#include <sys/thread.h>

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

int salsa64_audio_spu_init(struct salsa64_audio_spu *s, const struct salsa64_bus *bus)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = spu_start;
	s->backend.finish = spu_finish;
	s->backend.done = spu_done;
	s->bus = bus;
	s->worker = spukit_worker_start(spu_audio_bin, "salsa64_audio", 0);
	return s->worker ? 0 : spukit_last_error();
}

void salsa64_audio_spu_shutdown(struct salsa64_audio_spu *s)
{
	if (s->pending)
		spu_finish(s, NULL);
	if (s->worker)
		spukit_worker_stop(s->worker);
	s->worker = NULL;
}
