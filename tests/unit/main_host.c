/*
 * Salsa64 — pruebas unitarias en el host (`make test-host`).
 *
 *   salsa64-unit [rec] [bench]
 *
 * "rec" corre todas las pruebas también con el recompilador (solo en un host
 * ppc64 big-endian, p. ej. qemu-ppc64: tools/test-rec.sh); "bench" corre el
 * micro-benchmark al final (con el recompilador, si está activo).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu/rec.h"
#include "host/rec_host.h"
#include "test.h"

static struct salsa64_rec rec;

/* SALSA64_REC_DUMP=1: cada bloque compilado en hexadecimal, para
 * desensamblarlo (tools/rec-disasm.sh). */
static void dump_block(struct salsa64_rec *r, uint64_t pc, uint32_t entry, uint32_t words, bool temp)
{
	fprintf(stderr, "bloque %016llx%s entrada %u, %u palabras:", (unsigned long long)pc,
		temp ? " (recortado)" : "", entry, words);
	for (uint32_t k = 0; k < words; k++)
		fprintf(stderr, " %08x", r->code[entry + k]);
	fputc('\n', stderr);
}

static void print_line(const char *line)
{
	puts(line);
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
	bool use_rec = false, bench = false;
	for (int k = 1; k < argc; k++) {
		if (!strcmp(argv[k], "rec"))
			use_rec = true;
		else if (!strcmp(argv[k], "bench"))
			bench = true;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);  /* que se vea hasta dónde llegó si se cuelga */
	char tmp[] = "/tmp/salsa64-unit-XXXXXX";
	t_tmp_dir = mkdtemp(tmp);
	salsa64_unit_filter(getenv("SALSA64_TEST"));
	int failed = salsa64_unit_run(print_line);
	if (use_rec) {
		void *code = salsa64_host_rec_buffer();
		if (getenv("SALSA64_REC_DUMP"))
			rec.on_compile = dump_block;
		rec.opts = salsa64_host_rec_opts();
		int r = code ? salsa64_unit_run_rec(print_line, &rec, code, SALSA64_HOST_REC_BYTES) : -1;
		if (r < 0)
			return 2;
		printf("recompilador: caché en %p, llamadas a C %s%s\n", code,
		       rec.calls_near ? "con bl directo" : "por mtctr/bctrl",
		       rec.opts & SALSA64_REC_OPT_PIN ? ", registros fijos" : "");
		failed += r;
	}
	if (bench) {
		salsa64_unit_bench(print_line, now_us);
		if (!use_rec)
			salsa64_unit_bench_aligned(print_line, now_us);
	}
	if (t_tmp_dir) {
		char cmd[64];
		snprintf(cmd, sizeof cmd, "rm -rf %s", t_tmp_dir);
		if (system(cmd) != 0)
			fprintf(stderr, "no se pudo borrar %s\n", t_tmp_dir);
	}
	return failed ? 1 : 0;
}
