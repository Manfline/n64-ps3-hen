/*
 * Salsa64 — pruebas unitarias en el host (`make test-host`).
 * Con el argumento "bench" también corre el micro-benchmark.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "test.h"

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
	int failed = salsa64_unit_run(print_line);
	if (argc > 1 && strcmp(argv[1], "bench") == 0)
		salsa64_unit_bench(print_line, now_us);
	return failed ? 1 : 0;
}
