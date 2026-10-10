/*
 * Salsa64 — pruebas unitarias del núcleo (CPU, bus) en la PS3, con el
 * intérprete y con el recompilador (Fase 3).
 *
 * Son las mismas de tests/unit que corren en el host con `make test-host`;
 * aquí validan el camino big-endian (accesos directos a la RDRAM sin
 * byte-swap), el código que genera ppu-gcc y, en la PS3 real, el código que
 * genera el recompilador en la caché NOBITS (docs/rec-ppe.md). Después de
 * cada pasada corre el micro-benchmark. Al final, nemu64-test completo con
 * los dos (rombench.c): mismas pruebas fallidas y tiempo con código real.
 * Tiempos en el log: solo valen en la PS3 real (en RPCS3 no; ver
 * docs/proyecto.md). En RPCS3 el recompilador queda apagado
 * (platform/ps3/rec_cache.h).
 */
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "cpu/rec.h"
#include "platform/ps3/log.h"
#include "platform/ps3/rec_cache.h"
#include "rombench.h"
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

static void log_rombench(const char *who, const struct rombench_result *r)
{
	salsa64_log("nemu64-test %s: %s; %u pruebas con falla (hash %016llx), %llu ciclos en %llu ms = %.1f MIPS",
		    who, r->finished ? r->summary : "SIN TERMINAR", r->failed, (unsigned long long)r->names_hash,
		    (unsigned long long)r->cycles, (unsigned long long)(r->us / 1000),
		    r->us ? (double)r->cycles / (double)r->us : 0.0);
}

int main(void)
{
	salsa64_log_init("cpu-tests");

	u64 t0 = sysGetSystemTime();
	int failed = salsa64_unit_run(print_line);
	u64 us = sysGetSystemTime() - t0;
	salsa64_unit_bench(print_line, now_us);

	/* El recompilador con cada combinación de opciones (rec.h): pruebas en
	 * modo estricto, bench y nemu64-test completo contra el intérprete. */
	static const struct { uint32_t opts; const char *name; } variants[] = {
		{ 0, "recompilador" },
		{ SALSA64_REC_OPT_EAGER, "recompilador + escritura temprana" },
		{ SALSA64_REC_OPT_PIN, "recompilador + registros fijos" },
		{ SALSA64_REC_OPT_PIN | SALSA64_REC_OPT_EAGER, "recompilador + registros fijos + escritura temprana" },
	};
	const char *rec_state = "apagado (" SALSA64_NOREC_FLAG ")", *rom_state = "sin comparar";
	int rec_failed = 0;
	u64 rec_us = 0;
	static struct rombench_result ri, rr;
	if (salsa64_rombench(&ri, NULL, NULL, 0, now_us))
		log_rombench("intérprete", &ri);
	for (unsigned v = 0; v < sizeof variants / sizeof variants[0] && salsa64_ps3_rec_wanted(); v++) {
		const char *name = variants[v].name;
		salsa64_log("---- %s", name);
		rec.opts = variants[v].opts;
		t0 = sysGetSystemTime();
		int r = salsa64_unit_run_rec(print_line, &rec, salsa64_ps3_rec_cache(), SALSA64_REC_CACHE_BYTES);
		rec_us += sysGetSystemTime() - t0;
		if (r < 0) {
			rec_state = "no disponible";
			break;
		}
		rec_state = rec.calls_near ? "activo, llamadas con bl" : "activo, llamadas por mtctr";
		rec_failed += r;
		salsa64_unit_bench(print_line, now_us);
		salsa64_rec_detach(&rec);

		if (!salsa64_rombench(&rr, &rec, salsa64_ps3_rec_cache(), SALSA64_REC_CACHE_BYTES, now_us))
			continue;
		log_rombench(name, &rr);
		const struct salsa64_rec_stats *st = &rec.stats;
		salsa64_log("%s: %llu bloques (%llu fuera de KSEG0), %u KB de caché, %llu entradas desde C, %llu enlaces, "
			    "%llu invalidados, %llu vaciados, %llu instr. interpretadas", name,
			    (unsigned long long)st->compiled, (unsigned long long)st->compiled_x,
			    salsa64_rec_used(&rec) / 256,
			    (unsigned long long)st->entries, (unsigned long long)st->linked,
			    (unsigned long long)st->invalidated, (unsigned long long)st->flushes,
			    (unsigned long long)st->interp_steps);
		bool same = ri.finished && rr.finished && ri.failed == rr.failed &&
			    ri.names_hash == rr.names_hash && ri.cycles == rr.cycles;
		if (!same || v == 0)
			rom_state = same ? "iguales" : "DISTINTOS";
		if (!same)
			rec_failed++;
		if (rr.us)
			salsa64_log("nemu64-test: %s %.2fx más rápido que el intérprete", name,
				    (double)ri.us / (double)rr.us);
	}

	salsa64_log("RESULTADO cpu-tests: %s (intérprete: %d pruebas fallidas, %u ms; recompilador %s: "
		    "%d fallidas, %u ms; nemu64-test %s)", failed || rec_failed ? "FALLO" : "OK", failed,
		    (unsigned)(us / 1000), rec_state, rec_failed, (unsigned)(rec_us / 1000), rom_state);
	salsa64_log_shutdown();
	return 0;
}
