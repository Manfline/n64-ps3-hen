/*
 * Retro64 — Fase 0: cálculo en los 6 SPEs.
 *
 * Reparte un búfer de NUM_SPUS × CHUNKS_PER_SPU × 16 KB entre los hilos de un
 * grupo SPU; cada SPU aplica spu_mix_ref() con SIMD y DMA de doble búfer
 * (spu/mix.c). El PPE verifica la salida palabra por palabra y las sumas de
 * comprobación. Los tiempos son informativos: en RPCS3 no significan nada.
 */
#include <malloc.h>
#include <ppu-types.h>
#include <stdint.h>
#include <string.h>
#include <sys/process.h>
#include <sys/spu.h>
#include <sys/systime.h>

#include "job.h"
#include "platform/ps3/log.h"
#include "spu_mix_bin.h"

#define NUM_SPUS       6
#define CHUNKS_PER_SPU 64 /* 64 × 16 KB = 1 MB por SPU */
#define WORDS_PER_SPU  (CHUNKS_PER_SPU * SPU_CHUNK_WORDS)
#define TOTAL_WORDS    (NUM_SPUS * WORDS_PER_SPU)

#define ptr2ea(x) ((u64)(uintptr_t)(x))

SYS_PROCESS_PARAM(1001, 0x100000);

static spu_job_t jobs[NUM_SPUS] __attribute__((aligned(128)));
static spu_result_t results[NUM_SPUS] __attribute__((aligned(128)));
static const char group_name[] = "retro64_spu_compute";
static const char thread_name[] = "retro64_mix";

static int run_spus(const u32 *src, u32 *dst, u64 *elapsed_us)
{
	sysSpuImage image;
	sys_spu_group_t group;
	sys_spu_thread_t threads[NUM_SPUS];
	sysSpuThreadGroupAttribute gattr = { sizeof group_name, group_name, 0, { 0 } };
	sysSpuThreadAttribute tattr = { thread_name, sizeof thread_name, SPU_THREAD_ATTR_NONE };
	s32 rc;

	if ((rc = sysSpuImageImport(&image, spu_mix_bin, 0)) != 0) {
		retro64_log("sysSpuImageImport: 0x%08x", rc);
		return -1;
	}
	if ((rc = sysSpuThreadGroupCreate(&group, NUM_SPUS, 100, &gattr)) != 0) {
		retro64_log("sysSpuThreadGroupCreate(%d): 0x%08x", NUM_SPUS, rc);
		sysSpuImageClose(&image);
		return -1;
	}

	for (u32 i = 0; i < NUM_SPUS; i++) {
		jobs[i].src_ea = ptr2ea(src + i * WORDS_PER_SPU);
		jobs[i].dst_ea = ptr2ea(dst + i * WORDS_PER_SPU);
		jobs[i].result_ea = ptr2ea(&results[i]);
		jobs[i].chunks = CHUNKS_PER_SPU;
		jobs[i].index = i;

		sysSpuThreadArgument arg = { ptr2ea(&jobs[i]), 0, 0, 0 };
		if ((rc = sysSpuThreadInitialize(&threads[i], group, i, &image, &tattr, &arg)) != 0) {
			retro64_log("sysSpuThreadInitialize(%u): 0x%08x", i, rc);
			sysSpuThreadGroupDestroy(group);
			sysSpuImageClose(&image);
			return -1;
		}
	}

	u64 t0 = sysGetSystemTime();
	rc = sysSpuThreadGroupStart(group);
	u32 cause = 0, status = 0;
	if (rc == 0)
		rc = sysSpuThreadGroupJoin(group, &cause, &status);
	*elapsed_us = sysGetSystemTime() - t0;

	if (rc != 0)
		retro64_log("start/join del grupo: 0x%08x", rc);
	else if (cause != SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT)
		retro64_log("el grupo terminó con causa 0x%x, estado 0x%x", cause, status);

	for (u32 i = 0; i < NUM_SPUS; i++) {
		s32 exit_status = -1;
		sysSpuThreadGetExitStatus(threads[i], &exit_status);
		if (exit_status != 0)
			retro64_log("SPU %u salió con %d", i, exit_status);
	}

	sysSpuThreadGroupDestroy(group);
	sysSpuImageClose(&image);
	return (rc == 0 && cause == SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT) ? 0 : -1;
}

int main(void)
{
	retro64_log_init("spu-compute");

	const size_t bytes = (size_t)TOTAL_WORDS * 4;
	u32 *src = memalign(128, bytes);
	u32 *dst = memalign(128, bytes);
	u32 *ref = memalign(128, bytes);
	if (!src || !dst || !ref) {
		retro64_log("RESULTADO spu-compute: FALLO sin memoria (%u bytes × 3)", (unsigned)bytes);
		return 1;
	}

	u32 x = 0x12345678u; /* xorshift32 */
	for (u32 i = 0; i < TOTAL_WORDS; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		src[i] = x;
	}
	memset(dst, 0, bytes);

	u64 t0 = sysGetSystemTime();
	for (u32 i = 0; i < TOTAL_WORDS; i++)
		ref[i] = spu_mix_ref(src[i]);
	u64 ppe_us = sysGetSystemTime() - t0;

	s32 rc = sysSpuInitialize(NUM_SPUS, 0);
	if (rc != 0)
		retro64_log("sysSpuInitialize(%d, 0): 0x%08x (se continúa)", NUM_SPUS, rc);

	u64 spu_us = 0;
	int ok = run_spus(src, dst, &spu_us) == 0;

	u32 mismatches = 0, first_bad = 0;
	for (u32 i = 0; ok && i < TOTAL_WORDS; i++) {
		if (dst[i] != ref[i] && mismatches++ == 0)
			first_bad = i;
	}
	if (mismatches) {
		retro64_log("%u palabras distintas; primera en %u: spu=0x%08x ref=0x%08x",
			   mismatches, first_bad, dst[first_bad], ref[first_bad]);
		ok = 0;
	}

	for (u32 s = 0; ok && s < NUM_SPUS; s++) {
		u32 sum = 0;
		for (u32 i = 0; i < WORDS_PER_SPU; i++)
			sum += ref[s * WORDS_PER_SPU + i];
		const spu_result_t *r = &results[s];
		if (r->magic != SPU_RESULT_MAGIC || r->index != s ||
		    r->chunks_done != CHUNKS_PER_SPU || r->checksum != sum) {
			retro64_log("SPU %u: magic=0x%08x index=%u chunks=%u suma=0x%08x (esperada 0x%08x)",
				   s, r->magic, r->index, r->chunks_done, r->checksum, sum);
			ok = 0;
		}
	}

	const unsigned mb = (unsigned)(bytes >> 20);
	retro64_log("PPE referencia: %u MB en %u us; %d SPUs: %u us (inicio + cálculo + join)",
		   mb, (unsigned)ppe_us, NUM_SPUS, (unsigned)spu_us);
	retro64_log("RESULTADO spu-compute: %s", ok ? "OK" : "FALLO");

	free(ref);
	free(dst);
	free(src);
	retro64_log_shutdown();
	return ok ? 0 : 1;
}
