/*
 * Salsa64 — pruebas unitarias del núcleo (CPU, bus) en la PS3, con el
 * intérprete y con el recompilador (Fase 3).
 *
 * Son las mismas de tests/unit que corren en el host con `make test-host`;
 * aquí validan el camino big-endian (accesos directos a la RDRAM sin
 * byte-swap), el código que genera ppu-gcc y, en la PS3 real, el código que
 * genera el recompilador en la caché NOBITS (docs/rec-ppe.md). Después de
 * cada pasada corre el micro-benchmark. Tiempos en el log: solo valen en la
 * PS3 real (en RPCS3 no; ver docs/proyecto.md). En RPCS3 el recompilador queda
 * apagado (platform/ps3/rec_cache.h).
 */
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "cpu/rec.h"
#include "platform/ps3/log.h"
#include "platform/ps3/rec_cache.h"
#include "test.h"

SYS_PROCESS_PARAM(1001, 0x40000);

static struct salsa64_rec rec;

static void print_line(const char *line)
{
	salsa64_log("%s", line);
}

static uint64_t now_us(void)
{
	return sysGetSystemTime();
}

int main(void)
{
	salsa64_log_init("cpu-tests");

	u64 t0 = sysGetSystemTime();
	int failed = salsa64_unit_run(print_line);
	u64 us = sysGetSystemTime() - t0;
	salsa64_unit_bench(print_line, now_us);

	const char *rec_state = "apagado (" SALSA64_NOREC_FLAG ")";
	int rec_failed = 0;
	u64 rec_us = 0;
	if (salsa64_ps3_rec_wanted()) {
		t0 = sysGetSystemTime();
		int r = salsa64_unit_run_rec(print_line, &rec, salsa64_ps3_rec_cache(), SALSA64_REC_CACHE_BYTES);
		rec_us = sysGetSystemTime() - t0;
		if (r < 0) {
			rec_state = "no disponible";
		} else {
			rec_state = rec.calls_near ? "activo, llamadas con bl" : "activo, llamadas por mtctr";
			rec_failed = r;
			salsa64_unit_bench(print_line, now_us);
		}
	}

	salsa64_log("RESULTADO cpu-tests: %s (intérprete: %d pruebas fallidas, %u ms; recompilador %s: "
		    "%d fallidas, %u ms)", failed || rec_failed ? "FALLO" : "OK", failed, (unsigned)(us / 1000),
		    rec_state, rec_failed, (unsigned)(rec_us / 1000));
	salsa64_log_shutdown();
	return 0;
}
