/*
 * Salsa64 — marco y arnés de las pruebas unitarias. Ver test.h.
 */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/endian.h"

struct salsa64_n64 t_n64;
int t_checks;

static uint8_t t_rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static void (*t_print)(const char *line);
static const char *t_current;
static int t_current_failed;
static int t_tests, t_failed_tests;

void t_fail(const char *file, int line, const char *fmt, ...)
{
	char msg[512];
	int n = snprintf(msg, sizeof msg, "FALLO %s (%s:%d): ", t_current, file, line);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg + n, sizeof msg - (size_t)n, fmt, ap);
	va_end(ap);
	t_print(msg);
	t_current_failed = 1;
}

void t_set_status(uint32_t status)
{
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_STATUS, status);
}

static void t_reset(void)
{
	salsa64_n64_reset(&t_n64);
	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE);
}

void t_load(const uint32_t *code, unsigned n)
{
	for (unsigned k = 0; k < n; k++)
		t_wr32(T_CODE_PA + 4 * k, code[k]);
}

void t_step(unsigned n)
{
	while (n--)
		salsa64_cpu_step(CPU);
}

uint32_t t_rd32(uint32_t pa) { return salsa64_rd32(t_rdram + pa); }
uint64_t t_rd64(uint32_t pa) { return salsa64_rd64(t_rdram + pa); }
void t_wr32(uint32_t pa, uint32_t v) { salsa64_wr32(t_rdram + pa, v); }
void t_wr64(uint32_t pa, uint64_t v) { salsa64_wr64(t_rdram + pa, v); }

uint32_t t_fbits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
uint64_t t_dbits(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }

void t_run(const char *name, t_test_fn fn)
{
	t_current = name;
	t_current_failed = 0;
	t_reset();
	fn();
	t_tests++;
	t_failed_tests += t_current_failed;
}

int salsa64_unit_run(void (*print)(const char *line))
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} suites[] = {
		{ "core", t_suite_core },
		{ "alu", t_suite_alu },
		{ "muldiv", t_suite_muldiv },
		{ "branch", t_suite_branch },
		{ "mem", t_suite_mem },
		{ "cop0", t_suite_cop0 },
		{ "cop1", t_suite_cop1 },
		{ "boot", t_suite_boot },
		{ "vi", t_suite_vi },
		{ "pif", t_suite_pif },
		{ "idle", t_suite_idle },
	};
	char line[160];

	t_print = print;
	t_tests = t_failed_tests = t_checks = 0;
	salsa64_n64_init(&t_n64, t_rdram, sizeof t_rdram);

	for (unsigned s = 0; s < sizeof suites / sizeof suites[0]; s++) {
		int tests0 = t_tests, failed0 = t_failed_tests;
		suites[s].fn();
		snprintf(line, sizeof line, "suite %-7s %3d pruebas, %d fallidas",
			 suites[s].name, t_tests - tests0, t_failed_tests - failed0);
		print(line);
	}
	snprintf(line, sizeof line, "total: %d pruebas, %d verificaciones, %d fallidas (%s)",
		 t_tests, t_checks, t_failed_tests,
		 SALSA64_HOST_BIG_ENDIAN ? "big-endian" : "little-endian");
	print(line);
	return t_failed_tests;
}
