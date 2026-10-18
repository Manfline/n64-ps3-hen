/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tarea JPEG del RSP en un SPE. Ver jpeg_spu.h.
 */
#include "platform/ps3/jpeg_spu.h"

#include <string.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "core/rom.h"
#include "platform/ps3/log.h"
#include "rsp/jpeg/jpeg.h"
#include "rsp/jpeg/re2.h"
#include "rsp/jpeg/selftest.h"
#include "spu_jpeg_bin.h"

#define POLL_SLEEP_US 100

static void spu_start(void *ctx, const struct salsa64_task *task, unsigned kind)
{
	struct salsa64_jpeg_spu *s = ctx;
	memset(&s->job, 0, sizeof s->job);
	s->job.rdram_ea = (uint64_t)(uintptr_t)s->bus->rdram;
	s->job.rdram_size = s->bus->rdram_size;
	s->job.data_ptr = task->data_ptr;  /* en las de video, rsp.c ya puso ucode_data aquí */
	s->job.kind = kind;
	s->job.dram_stack = task->dram_stack;
	s->job.dram_stack_size = task->dram_stack_size;
	s->seq = spukit_post(s->worker, JPEG_JOB_TASK, &s->job, sizeof s->job);
	s->pending = true;
}

/* Como el backend gráfico: si el SPE no terminó y el CPU está en un bucle
 * de espera, se duerme un poco antes de contestar (el PPE es SMT y el reloj
 * emulado no debe adelantarse al real). */
static bool spu_done(void *ctx, bool idle)
{
	struct salsa64_jpeg_spu *s = ctx;
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

static void spu_finish(void *ctx, struct salsa64_jpeg_result *res)
{
	struct salsa64_jpeg_spu *s = ctx;
	memset(res, 0, sizeof *res);
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
	const enum salsa64_jpeg_kind kind = (enum salsa64_jpeg_kind)s->job.kind;
	const bool video = salsa64_jpeg_is_re2(kind);
	const unsigned vk = video ? (unsigned)(kind - SALSA64_JPEG_RE2_RESIZE) : 0;
	if (video) {
		s->v_tasks[vk]++;
		s->v_ticks[vk] += r->ticks;
		if (r->ticks > s->v_max[vk])
			s->v_max[vk] = r->ticks;
	} else {
		s->tasks++;
		s->spu_ticks += r->ticks;
		if (r->ticks > s->spu_max)
			s->spu_max = r->ticks;
	}
	if (r->error) {
		/* Sin tocar la RDRAM: la hace el PPE (y marca los parámetros
		 * malos, si lo eran). */
		if (video) {
			s->v_on_ppe[vk]++;
			salsa64_re2_run(kind, s->bus->rdram, s->bus->rdram_size, s->job.data_ptr, s->job.dram_stack,
					s->job.dram_stack_size, res);
		} else {
			s->on_ppe++;
			salsa64_jpeg_run(kind, s->bus->rdram, s->bus->rdram_size, s->job.data_ptr, res);
		}
	} else {
		struct jpeg_job_result jr;
		memcpy(&jr, r->data, sizeof jr);
		res->macroblocks = jr.macroblocks;
		res->out_addr = jr.out_addr;
		res->out_bytes = jr.out_bytes;
		res->mb_stride = jr.mb_stride;
		res->mb_out = jr.mb_out;
		res->bad = jr.bad != 0;
	}
	if (video)
		s->v_rows[vk] += res->macroblocks;
	else
		s->macroblocks += res->macroblocks;
}

/* Al arrancar: las dos tareas de prueba en el SPE y en el PPE sobre copias
 * iguales de una RDRAM de mentira; deben quedar iguales entre sí y con el
 * CRC que fija la prueba del host. En RPCS3, además, el programa del SPE se
 * compila aquí y no en la primera sala. */
static bool check_tasks(spukit_worker *w, uint32_t *crc_spe, uint32_t *crc_ppe)
{
	static uint8_t ram_spe[JPEG_SELFTEST_RAM] __attribute__((aligned(128)));
	static uint8_t ram_ppe[JPEG_SELFTEST_RAM] __attribute__((aligned(128)));
	bool ok = true;
	jpeg_selftest_build(ram_spe, JPEG_SELFTEST_SEED);
	memcpy(ram_ppe, ram_spe, sizeof ram_ppe);
	for (uint32_t t = 0; t < 2; t++) {
		struct jpeg_job job;
		struct jpeg_job_result jr;
		struct salsa64_jpeg_result res;
		memset(&job, 0, sizeof job);
		job.rdram_ea = (uint64_t)(uintptr_t)ram_spe;
		job.rdram_size = JPEG_SELFTEST_RAM;
		job.data_ptr = jpeg_selftest_task(t);
		job.kind = SALSA64_JPEG_OOT;
		const spukit_result *r = spukit_wait(w, spukit_post(w, JPEG_JOB_TASK, &job, sizeof job));
		memcpy(&jr, r->data, sizeof jr);
		salsa64_jpeg_run(SALSA64_JPEG_OOT, ram_ppe, JPEG_SELFTEST_RAM, job.data_ptr, &res);
		if (r->error || jr.bad || res.bad || jr.macroblocks != res.macroblocks || jr.out_bytes != res.out_bytes) {
			salsa64_log("JPEG: tarea de prueba %u: el SPE dio error %d, %u macrobloques (%u bytes, malos %u); el PPE "
				    "%u (%u bytes, malos %u)", t, r->error, jr.macroblocks, jr.out_bytes, jr.bad,
				    res.macroblocks, res.out_bytes, res.bad);
			ok = false;
		}
	}
	*crc_spe = salsa64_crc32(ram_spe, sizeof ram_spe);
	*crc_ppe = salsa64_crc32(ram_ppe, sizeof ram_ppe);
	return ok && *crc_spe == *crc_ppe && *crc_ppe == JPEG_SELFTEST_CRC;
}

/* Lo mismo con las cuatro tareas de video (rsp/jpeg/selftest.c): una RDRAM de 64 KB para cada
 * lado, el SPE y el PPE, y el CRC que fija la prueba del host. */
static bool check_video(spukit_worker *w, uint32_t *crc_spe, uint32_t *crc_ppe)
{
	static uint8_t ram_spe[RE2_SELFTEST_RAM] __attribute__((aligned(128)));
	static uint8_t ram_ppe[RE2_SELFTEST_RAM] __attribute__((aligned(128)));
	static const enum salsa64_jpeg_kind kinds[RE2_SELFTEST_TASKS] = { SALSA64_JPEG_RE2_RESIZE, SALSA64_JPEG_RE2_DECODE,
									  SALSA64_JPEG_RE2_FILL, SALSA64_JPEG_RE2_FILL };
	bool ok = true;
	re2_selftest_build(ram_spe, RE2_SELFTEST_SEED);
	memcpy(ram_ppe, ram_spe, sizeof ram_ppe);
	for (uint32_t t = 0; t < RE2_SELFTEST_TASKS; t++) {
		struct jpeg_job job;
		struct jpeg_job_result jr;
		struct salsa64_jpeg_result res;
		memset(&job, 0, sizeof job);
		job.rdram_ea = (uint64_t)(uintptr_t)ram_spe;
		job.rdram_size = RE2_SELFTEST_RAM;
		job.data_ptr = re2_selftest_task(t);
		job.kind = kinds[t];
		job.dram_stack = RE2_SELFTEST_DRAM_STACK;
		job.dram_stack_size = RE2_SELFTEST_DRAM_STACK_SIZE;
		const spukit_result *r = spukit_wait(w, spukit_post(w, JPEG_JOB_TASK, &job, sizeof job));
		memcpy(&jr, r->data, sizeof jr);
		salsa64_re2_run(kinds[t], ram_ppe, RE2_SELFTEST_RAM, job.data_ptr, job.dram_stack, job.dram_stack_size, &res);
		if (r->error || jr.bad || res.bad || jr.macroblocks != res.macroblocks || jr.out_bytes != res.out_bytes ||
		    jr.macroblocks == 0) {
			salsa64_log("JPEG: tarea de video de prueba %u: el SPE dio error %d, %u unidades (%u bytes, malos %u); el "
				    "PPE %u (%u bytes, malos %u)", t, r->error, jr.macroblocks, jr.out_bytes, jr.bad,
				    res.macroblocks, res.out_bytes, res.bad);
			ok = false;
		}
	}
	*crc_spe = salsa64_crc32(ram_spe, sizeof ram_spe);
	*crc_ppe = salsa64_crc32(ram_ppe, sizeof ram_ppe);
	return ok && *crc_spe == *crc_ppe && *crc_ppe == RE2_SELFTEST_CRC;
}

int salsa64_jpeg_spu_init(struct salsa64_jpeg_spu *s, struct salsa64_bus *bus, bool *same)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = spu_start;
	s->backend.done = spu_done;
	s->backend.finish = spu_finish;
	s->bus = bus;
	*same = false;
	if ((uintptr_t)bus->rdram & 15) {
		salsa64_log("JPEG: la RDRAM no está alineada a 16 bytes: sin SPE");
		return SPUKIT_EARG;
	}
	s->worker = spukit_worker_start(spu_jpeg_bin, "salsa64_jpeg", 0);
	if (!s->worker)
		return spukit_last_error();
	uint64_t t0 = sysGetSystemTime();
	uint32_t crc_spe, crc_ppe;
	*same = check_tasks(s->worker, &crc_spe, &crc_ppe);
	salsa64_log("JPEG: tareas de prueba en el SPE: %s (SPE %08X, PPE %08X, host %08X; %.1f ms)",
		    *same ? "iguales al PPE y al host" : "DIFIEREN", crc_spe, crc_ppe, JPEG_SELFTEST_CRC,
		    (sysGetSystemTime() - t0) / 1e3);
	t0 = sysGetSystemTime();
	bool video_same = check_video(s->worker, &crc_spe, &crc_ppe);
	salsa64_log("JPEG: tareas de video de prueba en el SPE: %s (SPE %08X, PPE %08X, host %08X; %.1f ms)",
		    video_same ? "iguales al PPE y al host" : "DIFIEREN", crc_spe, crc_ppe, RE2_SELFTEST_CRC,
		    (sysGetSystemTime() - t0) / 1e3);
	*same = *same && video_same;
	return 0;
}

int salsa64_jpeg_spu_shutdown(struct salsa64_jpeg_spu *s)
{
	int rc = 0;
	if (s->pending) {
		struct salsa64_jpeg_result res;
		spu_finish(s, &res);
	}
	if (s->worker)
		rc = spukit_worker_stop(s->worker);
	s->worker = NULL;
	return rc;
}
