/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — backend síncrono de la HLE de audio. Ver backend.h.
 */
#include <string.h>

#include "rsp/audio/backend.h"

void salsa64_audio_job(struct aud_job *job, const struct salsa64_bus *bus, const struct salsa64_task *task,
		       enum salsa64_abi abi)
{
	memset(job, 0, sizeof *job);
	job->rdram_ea = (uint64_t)(uintptr_t)bus->rdram;
	job->rdram_size = bus->rdram_size;
	job->abi = abi;
	job->alist = task->data_ptr & 0x00FFFFFF;
	job->alist_size = task->data_size;
	job->ucode_data = task->ucode_data & 0x00FFFFFF;
}

void salsa64_audio_written(struct salsa64_bus *bus, const struct aud_result *res)
{
	uint32_t n = res->nranges < AUD_MAX_RANGES ? res->nranges : AUD_MAX_RANGES;
	for (uint32_t i = 0; i < n; i++)
		salsa64_bus_rdram_written(bus, res->range_addr[i], res->range_len[i]);
}

void salsa64_audio_add_stats(struct salsa64_audio_stats *st, const struct aud_result *res)
{
	st->tasks++;
	st->commands += res->commands;
	st->unknown += res->unknown;
	if (res->unknown)
		st->last_unknown = res->last_unknown;
	st->errors += res->errors;
}

static void sync_start(void *ctx, const struct salsa64_task *task, enum salsa64_abi abi)
{
	struct salsa64_audio_sync *s = ctx;
	salsa64_audio_job(&s->job, s->bus, task, abi);
	aud_run(s->state, &s->job, &s->res);
	s->pending = true;
}

static void sync_finish(void *ctx, struct salsa64_bus *bus)
{
	struct salsa64_audio_sync *s = ctx;
	if (!s->pending)
		return;
	s->pending = false;
	salsa64_audio_add_stats(&s->stats, &s->res);
	salsa64_audio_written(bus, &s->res);
}

static bool sync_save_state(void *ctx, uint8_t *buf)
{
	struct salsa64_audio_sync *s = ctx;
	aud_state_save(s->state, buf);
	return true;
}

static bool sync_load_state(void *ctx, const uint8_t *buf)
{
	struct salsa64_audio_sync *s = ctx;
	return aud_state_load(s->state, buf);
}

void salsa64_audio_sync_init(struct salsa64_audio_sync *s, struct aud_state *state, const struct salsa64_bus *bus)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = sync_start;
	s->backend.finish = sync_finish;
	s->backend.state_size = AUD_STATE_BYTES;
	s->backend.save_state = sync_save_state;
	s->backend.load_state = sync_load_state;
	s->state = state;
	s->bus = bus;
	aud_init(state);
}
