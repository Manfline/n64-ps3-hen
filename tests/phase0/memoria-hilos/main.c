/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diagnóstico aislado: reservas de newlib que sobreviven a sysThreadExit.
 * Sin ROMs, RSX ni SPE; las cifras del heap son bytes, no KB redondeados. */
#include <malloc.h>
#include <reent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <unistd.h>
#include "platform/ps3/log.h"
#include "platform/ps3/thread_cleanup.h"

SYS_PROCESS_PARAM(1001, 0x100000);

struct probe { bool decimal, clean, log; int ok; };
static volatile double input = 1.0 / 7.0;

static void worker(void *arg)
{
	struct probe *p = arg;
	char text[128];
	int n = p->decimal ? snprintf(text, sizeof text, "%.6f %.9e %.12g", input, input * 1234.0, input) :
			    snprintf(text, sizeof text, "%d %x %s", 1234, 5678, "prueba");
	p->ok = n > 0 && (unsigned)n < sizeof text;
	if (p->log)
		salsa64_log("memoria: salida del hilo %s", text);
	if (p->clean)
		salsa64_thread_reclaim();
	sysThreadExit(0);
}

static int batch(const char *name, bool decimal, bool clean, bool log, int *growth)
{
	/* El mismo hilo principal registra todos los valores. Un registro de
	 * calentamiento evita incluir su primera reserva de stdio en el lote. */
	salsa64_log("memoria: inicio %s", name);
	int start = mallinfo().uordblks, prev = start;
	for (unsigned k = 0; k < 8; k++) {
		struct probe p = { decimal, clean, log, 0 };
		sys_ppu_thread_t tid;
		int rc = sysThreadCreate(&tid, worker, &p, 1001, 0x10000, THREAD_JOINABLE, (char *)"memoria_prueba");
		if (rc) {
			salsa64_log("FALLO: crear hilo %s: %d", name, rc);
			return 1;
		}
		u64 result;
		rc = sysThreadJoin(tid, &result);
		if (rc || result || !p.ok) {
			salsa64_log("FALLO: cerrar hilo %s: %d", name, rc);
			return 1;
		}
		int now = mallinfo().uordblks;
		/* La salida compartida debe seguir abierta después de cada hijo. */
		if (write(1, "", 0) < 0) {
			salsa64_log("FALLO: el hilo cerró la TTY compartida");
			return 1;
		}
		salsa64_log("memoria: %s ciclo %u usados %d delta %d acumulado %d bytes", name, k + 1,
			    now, now - prev, now - start);
		prev = now;
	}
	*growth = prev - start;
	return 0;
}

int main(void)
{
	salsa64_log_init("salsa64-memoria-hilos");
	int integer = 0, ref = 0, clean = 0, logged = 0, logged_clean = 0;
	int failed = batch("enteros", false, false, false, &integer);
	if (!failed) failed = batch("decimal-referencia", true, false, false, &ref);
	if (!failed) failed = batch("decimal-limpio", true, true, false, &clean);
	if (!failed) failed = batch("registro-referencia", true, false, true, &logged);
	if (!failed) failed = batch("registro-limpio", true, true, true, &logged_clean);
	/* No exigir que la referencia fugue: una libc que ya recoja sus
	 * reservas también es válida. Sí exigir que la limpieza no acumule. */
	if (clean != 0 || logged_clean != 0) {
		salsa64_log("FALLO: los lotes limpios conservan %d/%d bytes", clean, logged_clean);
		failed = 1;
	}
	salsa64_log("memoria: lotes enteros %d referencia %d limpio %d bytes", integer, ref, clean);
	salsa64_log("memoria: registro referencia %d limpio %d bytes", logged, logged_clean);
	salsa64_log("RESULTADO memoria-hilos: %s", failed ? "FALLO" : "OK");
	salsa64_log_shutdown();
	return failed;
}
