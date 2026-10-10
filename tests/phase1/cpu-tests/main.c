/*
 * Retro64 — Fase 1: pruebas unitarias del núcleo (CPU, bus) en la PS3.
 *
 * Son las mismas de tests/unit que corren en el host con `make test-host`;
 * aquí validan el camino big-endian (accesos directos a la RDRAM sin
 * byte-swap) y el código que genera ppu-gcc. Tiempos en el log: solo
 * orientativos (en RPCS3 no valen; ver docs/proyecto.md).
 */
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "platform/ps3/log.h"
#include "test.h"

SYS_PROCESS_PARAM(1001, 0x40000);

static void print_line(const char *line)
{
	retro64_log("%s", line);
}

static uint64_t now_us(void)
{
	return sysGetSystemTime();
}

int main(void)
{
	retro64_log_init("cpu-tests");

	u64 t0 = sysGetSystemTime();
	int failed = retro64_unit_run(print_line);
	u64 us = sysGetSystemTime() - t0;
	retro64_unit_bench(print_line, now_us);

	retro64_log("RESULTADO cpu-tests: %s (%d pruebas fallidas, %u ms)",
		    failed ? "FALLO" : "OK", failed, (unsigned)(us / 1000));
	retro64_log_shutdown();
	return 0;
}
