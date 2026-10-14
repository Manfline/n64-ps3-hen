/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — banco de pruebas del emparejamiento de la interpolación
 * (src/rdp/vmatch.c) sobre dos tareas volcadas por el prototipo del host
 * (SALSA64_INTERP_VOLCAR=1 con salsa64-run --interp DIR: DIR/vm-20.* y
 * vm-21.*): arma las dos tablas, empareja y da las posiciones anteriores
 * muchas veces y dice cuánto tarda cada paso.
 *
 *   cc -O2 -Isrc -o build/vmatch-bench tools/vmatch-bench.c src/rdp/vmatch.c
 *   build/vmatch-bench DIR [VECES]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdp/vmatch.h"

static void *load(const char *dir, unsigned task, const char *ext, size_t *len)
{
	char path[600];
	snprintf(path, sizeof path, "%s/vm-%u.%s", dir, task, ext);
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "no puedo abrir %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = malloc(*len ? *len : 1);
	if (fread(p, 1, *len, f) != *len)
		exit(1);
	fclose(f);
	return p;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "uso: vmatch-bench DIR [VECES]\n");
		return 1;
	}
	int reps = argc > 2 ? atoi(argv[2]) : 1000;
	size_t cl[2], vl[2], rl[2];
	uint8_t *cmd[2], *vtx[2], *ram[2];
	for (int i = 0; i < 2; i++) {
		cmd[i] = load(argv[1], 20 + i, "cmd", &cl[i]);
		vtx[i] = load(argv[1], 20 + i, "vtx", &vl[i]);
		ram[i] = load(argv[1], 20 + i, "ram", &rl[i]);
	}
	const uint32_t gcap = 2048, ccap = 32768;
	struct vmatch_table t[2];
	for (int i = 0; i < 2; i++)
		vmatch_table_init(&t[i], aligned_alloc(16, vmatch_table_bytes(gcap, ccap)), gcap, ccap);
	uint32_t nv = (uint32_t)(vl[1] / sizeof(struct gfx_vertex));
	float(*out)[4] = malloc(nv * sizeof *out);
	struct vmatch_stats st;
	double tb = 0, tp = 0, tq = 0;
	uint32_t paired = 0;
	for (int r = 0; r < reps; r++) {
		double a = now();
		vmatch_build(&t[0], cmd[0], (uint32_t)cl[0]);
		vmatch_build(&t[1], cmd[1], (uint32_t)cl[1]);
		double b = now();
		vmatch_pair(&t[0], &t[1], 120.0f, &st);
		vmatch_atomic(&t[1], (const struct gfx_vertex *)vtx[1], nv, &st);
		double c = now();
		paired = vmatch_prev(&t[0], &t[1], &st, (const struct gfx_vertex *)vtx[1], nv, out);
		double d = now();
		tb += (b - a) / 2, tp += c - b, tq += d - c;
	}
	printf("%u grupos, %u vértices cargados, %u de salida (%u con pareja); por tarea: armar %.1f µs, emparejar %.1f µs, "
	       "posiciones %.1f µs\n",
	       t[1].ngroups, t[1].nclip, nv, paired, tb / reps * 1e6, tp / reps * 1e6, tq / reps * 1e6);
	return 0;
}
