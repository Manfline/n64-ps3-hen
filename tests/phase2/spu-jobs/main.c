/*
 * Salsa64 — Fase 2: prueba de lib/spukit (trabajos por lotes en SPEs).
 *
 * Dos trabajadores persistentes (dos SPEs). Verifica:
 *   1. Trabajos grandes (1 MB) repartidos entre los dos, comparados palabra
 *      por palabra con la referencia del PPE, con la cola llena a propósito
 *      (más trabajos en vuelo que lugares en la cola).
 *   2. Ida y vuelta de un trabajo vacío (publicar + esperar), promedio de
 *      1000: el costo fijo de hablar con un SPE una vez por cuadro.
 *   3. Salida limpia de los hilos SPU.
 * Los tiempos son informativos: en RPCS3 no significan nada.
 */
#include <malloc.h>
#include <ppu-types.h>
#include <string.h>
#include <sys/process.h>
#include <sys/systime.h>

#include <spukit/spukit.h>

#include "job.h"
#include "platform/ps3/log.h"
#include "spu_scale_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define WORKERS    2
#define JOB_WORDS  (256u * 1024)  /* 1 MB */
#define BIG_JOBS   24             /* > 2 × SPUKIT_QUEUE_LEN */
#define PINGS      1000

#define ptr2ea(x) ((u64)(uintptr_t)(x))

int main(void)
{
	salsa64_log_init("spu-jobs");
	int ok = 1;

	u32 *src = memalign(128, JOB_WORDS * 4 * BIG_JOBS);
	u32 *dst = memalign(128, JOB_WORDS * 4 * BIG_JOBS);
	if (!src || !dst) {
		salsa64_log("RESULTADO spu-jobs: FALLO sin memoria");
		return 1;
	}
	u32 x = 0x2545F491u;
	for (u32 i = 0; i < JOB_WORDS * BIG_JOBS; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		src[i] = x;
	}
	memset(dst, 0, JOB_WORDS * 4 * BIG_JOBS);

	spukit_init(6);
	spukit_worker *w[WORKERS];
	for (int i = 0; i < WORKERS; i++) {
		w[i] = spukit_worker_start(spu_scale_bin, i ? "salsa64_jobs1" : "salsa64_jobs0", (u64)i);
		if (!w[i]) {
			salsa64_log("RESULTADO spu-jobs: FALLO al arrancar el trabajador %d (%d)", i,
				    spukit_last_error());
			return 1;
		}
	}

	/* 1. Trabajos grandes, todos publicados antes de esperar ninguno. */
	u32 seq[BIG_JOBS];
	job_scale p[BIG_JOBS];
	u64 t0 = sysGetSystemTime();
	for (u32 j = 0; j < BIG_JOBS; j++) {
		p[j].src_ea = ptr2ea(src + j * JOB_WORDS);
		p[j].dst_ea = ptr2ea(dst + j * JOB_WORDS);
		p[j].words = JOB_WORDS;
		p[j].mul = 0x9E3779B1u + j;
		p[j].add = j * 0x01000193u;
		seq[j] = spukit_post(w[j % WORKERS], JOB_SCALE, &p[j], sizeof p[j]);
	}
	for (u32 j = 0; j < BIG_JOBS; j++) {
		const spukit_result *r = spukit_wait(w[j % WORKERS], seq[j]);
		const job_scale_result *jr = (const job_scale_result *)r->data;
		u32 sum = 0, bad = 0, first = 0;
		for (u32 i = 0; i < JOB_WORDS; i++) {
			u32 ref = job_scale_ref(src[j * JOB_WORDS + i], p[j].mul, p[j].add);
			sum += ref;
			if (dst[j * JOB_WORDS + i] != ref && bad++ == 0)
				first = i;
		}
		/* El resultado vale hasta SPUKIT_QUEUE_LEN publicaciones más en el
		 * mismo trabajador: los de los primeros trabajos ya se reutilizaron
		 * (de esos solo cuenta la salida). */
		int fresh = j + WORKERS * SPUKIT_QUEUE_LEN >= BIG_JOBS;
		if (bad || (fresh && (r->error || r->seq != seq[j] || jr->sum != sum ||
				      jr->words != JOB_WORDS || jr->worker != j % WORKERS))) {
			salsa64_log("trabajo %u: error %d seq %u/%u suma 0x%08x/0x%08x SPU %u, %u palabras mal (primera %u)",
				    j, r->error, r->seq, seq[j], jr->sum, sum, jr->worker, bad, first);
			ok = 0;
		}
	}
	u64 big_us = sysGetSystemTime() - t0;
	salsa64_log("%u trabajos de %u KB en %d SPEs: %llu us (incluye verificar en el PPE)", BIG_JOBS,
		    JOB_WORDS * 4 / 1024, WORKERS, (unsigned long long)big_us);

	/* 2. Ida y vuelta. */
	u64 spu_ticks = 0;
	t0 = sysGetSystemTime();
	for (int i = 0; i < PINGS; i++) {
		const spukit_result *r = spukit_wait(w[0], spukit_post(w[0], JOB_PING, NULL, 0));
		spu_ticks += r->ticks;
		if (r->error) {
			ok = 0;
			break;
		}
	}
	u64 ping_us = sysGetSystemTime() - t0;
	salsa64_log("ida y vuelta de un trabajo vacío: %.2f us (promedio de %d); en el SPU %.2f us",
		    (double)ping_us / PINGS, PINGS, spu_ticks * 1e6 / spukit_tick_hz() / PINGS);

	for (int i = 0; i < WORKERS; i++) {
		spukit_stats st;
		spukit_get_stats(w[i], &st);
		salsa64_log("trabajador %d: %u publicados, %u timbres, %u despertares, %u esperas (%llu us), "
			    "cola llena %llu us", i, st.posted, st.doorbells, st.wakeups, st.waits,
			    (unsigned long long)st.wait_us, (unsigned long long)st.full_us);
		if (st.wakeups == 0 || st.wakeups > st.doorbells)
			ok = 0;
	}

	/* 3. Salida. */
	for (int i = 0; i < WORKERS; i++) {
		int rc = spukit_worker_stop(w[i]);
		if (rc != 0) {
			salsa64_log("el trabajador %d salió con %d", i, rc);
			ok = 0;
		}
	}

	salsa64_log("RESULTADO spu-jobs: %s", ok ? "OK" : "FALLO");
	free(dst);
	free(src);
	salsa64_log_shutdown();
	return ok ? 0 : 1;
}
