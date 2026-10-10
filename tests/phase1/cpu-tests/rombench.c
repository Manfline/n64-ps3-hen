/*
 * Salsa64 — nemu64-test completo en la PS3 (embebido en el paquete), con el
 * intérprete y con el recompilador.
 *
 * Es la prueba de "código de verdad" del recompilador en el hardware: miles
 * de funciones, llamadas y retornos, FPU, excepciones y E/S. Las dos
 * corridas tienen que terminar en el mismo ciclo y con exactamente las
 * mismas pruebas fallidas (hash de sus nombres en orden); el tiempo de cada
 * una es la medición de rendimiento con código real (docs/perf-log.md).
 */
#include "rombench.h"

#include <stdio.h>
#include <string.h>

#include "core/boot.h"
#include "cpu/rec.h"
#include "nemu64_rom_bin.h"

#define MAX_CYCLES 400000000ull
#define SLICE      (1ull << 22)

static struct salsa64_n64 rb;
static uint8_t rb_rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static uint8_t rb_isv[SALSA64_ISV_SIZE] __attribute__((aligned(16)));

static struct {
	struct rombench_result *r;
	char line[256];
	unsigned n;
} out;

/* Una línea del ISViewer: "Test 'nombre' failed…" o el resumen final. */
static void rb_line(const char *s)
{
	struct rombench_result *r = out.r;
	if (!strncmp(s, "Test '", 6)) {
		r->failed++;
		for (const char *p = s + 6; *p && *p != '\''; p++)
			r->names_hash = (r->names_hash ^ (uint8_t)*p) * 0x100000001B3ull;  /* FNV-1a */
		r->names_hash = (r->names_hash ^ '\n') * 0x100000001B3ull;
	} else if (strstr(s, "success rate)")) {
		snprintf(r->summary, sizeof r->summary, "%s", s);
		r->finished = true;
		salsa64_n64_stop(&rb);
	}
}

static void rb_isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	(void)ctx;
	for (uint32_t i = 0; i < len; i++) {
		if (text[i] == '\n' || out.n == sizeof out.line - 1) {
			out.line[out.n] = '\0';
			rb_line(out.line);
			out.n = 0;
		}
		if (text[i] != '\n')
			out.line[out.n++] = (char)text[i];
	}
}

bool salsa64_rombench(struct rombench_result *r, struct salsa64_rec *rec, void *code, size_t bytes,
		      uint64_t (*now_us)(void))
{
	memset(r, 0, sizeof *r);
	r->names_hash = 0xCBF29CE484222325ull;
	out.r = r;
	out.n = 0;

	salsa64_n64_init(&rb, rb_rdram, sizeof rb_rdram);
	salsa64_bus_set_isviewer(&rb.bus, rb_isv, rb_isv_out, NULL);
	salsa64_bus_set_rom(&rb.bus, nemu64_rom_bin, nemu64_rom_bin_size);
	struct salsa64_boot_info info;
	if (!salsa64_boot_hle(&rb, SALSA64_BOOT_TV_AUTO, &info))
		return false;
	if (rec) {
		if (!salsa64_rec_init(rec, &rb.cpu, code, bytes))
			return false;
		rec->strict = false;
	}

	uint64_t start = rb.cpu.cycles, t0 = now_us();
	while (!r->finished && rb.cpu.cycles - start < MAX_CYCLES)
		salsa64_n64_run(&rb, SLICE);
	r->us = now_us() - t0;
	r->cycles = rb.cpu.cycles - start;
	if (rec)
		salsa64_rec_detach(rec);
	return true;
}
