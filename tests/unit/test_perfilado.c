/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas del archivo de perfilado (frontend/perfilado.h): la
 * configuración, los registros big-endian por el anillo (con pérdidas
 * contadas y el vaciado partido en la vuelta), la sesión numerada y la
 * rotación por tope.
 */
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "cpu/rec_trace.h"
#include "frontend/perfilado.h"

static void cfg_defaults_and_parse(void)
{
	struct perf_cfg c;
	perf_cfg_defaults(&c);
	CHECK(c.on && c.trace && c.jr && c.blocks && c.samples && c.tasks);
	CHECK_EQ(c.cap_mb, 200);
	CHECK_EQ(c.stall_ms, 25);
	perf_cfg_parse(&c, "# perfilado.cfg\ntraza = no\n jr=0\nvolcados = 3 # pocos\ntope_mb = 50\n"
			   "traza_largo = 999999\ntiron_ms = 0\nbasura\nmuestreo = si\n");
	CHECK(!c.trace);
	CHECK(!c.jr);
	CHECK(c.samples);
	CHECK_EQ(c.dumps, 3);
	CHECK_EQ(c.cap_mb, 50);
	CHECK_EQ(c.trace_len, SALSA64_RT_WIN_MAX);
	CHECK_EQ(c.stall_ms, 25);  /* 0 no vale */
	char text[512];
	perf_cfg_text(&c, text, sizeof text);
	CHECK(strstr(text, "traza=no") != NULL);
	CHECK(strstr(text, "volcados=3") != NULL);
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t ring_mem[64];

/* Un registro que da la vuelta al anillo sale entero y en orden; uno que no
 * cabe se cuenta como perdido y el archivo lo dice. */
static void ring_records_round_trip(void)
{
	if (!t_tmp_dir)
		return;
	static unsigned runs;  /* la suite corre dos veces (intérprete y recompilador): otra carpeta */
	char root[256];
	snprintf(root, sizeof root, "%s/perf1-%u", t_tmp_dir, runs++);
	struct perf_ring ring;
	perf_ring_init(&ring, ring_mem, sizeof ring_mem);
	struct perf_session s;
	memset(&s, 0, sizeof s);
	s.rings[0] = &ring;
	CHECK(perf_session_open(&s, root, "JUEGO-12345678", 1u << 20, 0));
	CHECK_EQ(s.n, 1);
	uint32_t v[3] = { 7, 0x01020304, 0xFFFFFFFF };
	CHECK(perf_put_values(&ring, PERF_R_WINDOW, 1000, v, 3));  /* 4 + 20 */
	CHECK(perf_put_text(&ring, PERF_R_EVENT, 1001, "hola"));   /* 4 + 8 */
	perf_drain(&s);
	CHECK(perf_put_values(&ring, PERF_R_STALL, 1002, v, 3));   /* da la vuelta */
	CHECK(!perf_put_values(&ring, PERF_R_STALL, 1003, v, 12)); /* no cabe */
	CHECK_EQ(ring.lost, 4 + 8 + 48);
	perf_session_close(&s);
	CHECK_EQ(s.write_errors, 0);

	FILE *f = fopen(s.path, "rb");
	CHECK(f != NULL);
	if (!f)
		return;
	uint8_t buf[256];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	CHECK_EQ(n, 16 + 24 + 12 + 24 + 12);
	CHECK(!memcmp(buf, "S64P", 4));
	CHECK_EQ(buf[5], PERF_VERSION);
	const uint8_t *p = buf + 16;
	CHECK_EQ(rd32(p), (uint32_t)PERF_R_WINDOW << 24 | 20);
	CHECK_EQ(rd32(p + 4), 1000);
	CHECK_EQ(rd32(p + 8), 3u << 16);
	CHECK_EQ(rd32(p + 16), 0x01020304);
	p += 24;
	CHECK_EQ(rd32(p), (uint32_t)PERF_R_EVENT << 24 | 8);
	CHECK(!memcmp(p + 8, "hola", 4));
	p += 12;
	CHECK_EQ(rd32(p), (uint32_t)PERF_R_STALL << 24 | 20);
	CHECK_EQ(rd32(p + 20), 0xFFFFFFFF);
	p += 24;
	CHECK_EQ(rd32(p), (uint32_t)PERF_R_LOST << 24 | 8);
	CHECK_EQ(rd32(p + 8), 60);

	/* La siguiente sesión del mismo juego es la 2. */
	perf_ring_init(&ring, ring_mem, sizeof ring_mem);
	CHECK(perf_session_open(&s, root, "JUEGO-12345678", 1u << 20, 0));
	CHECK_EQ(s.n, 2);
	perf_session_close(&s);
}

static uint8_t ring_big[8192];
static struct salsa64_rec_trace win_trace;
static struct salsa64_rec_trace_prev win_prev;

/* La ventana del camino lento: las cuentas como resta, los PCs de la tabla
 * propia de la ventana y el desglose de COP0 y el MI; una ventana sin nada
 * lento no repite los PCs de la anterior ni escribe COP0. */
static void slow_window_records(void)
{
	struct salsa64_rec_trace *t = &win_trace;
	salsa64_rec_trace_init(t);
	memset(&win_prev, 0, sizeof win_prev);
	struct perf_ring ring;
	for (int w = 0; w < 2; w++) {
		perf_ring_init(&ring, ring_big, sizeof ring_big);
		if (w == 0) {
			for (int k = 0; k < 3; k++)
				salsa64_rec_trace_slow_add(t, SALSA64_RT_SLOW_EXEC_COP0, 10, 0x80042968u);
			salsa64_rec_trace_cop0_add(t, SALSA64_RT_COP0_MT + 12, 30);
		}
		struct salsa64_rec_trace_slow_stats delta;
		CHECK(perf_put_slow_window(&ring, 5000 * (w + 1), t, &win_prev, &delta));
		CHECK_EQ(delta.calls[SALSA64_RT_SLOW_EXEC_COP0], w == 0 ? 3u : 0u);
		const uint8_t *p = ring_big;
		uint32_t len = 8 + 12 * SALSA64_RT_SLOW_N + 16 * SALSA64_RT_TOP_PC;
		CHECK_EQ(rd32(p), (uint32_t)PERF_R_SLOW << 24 | len);
		const uint8_t *cause = p + 12 + 12 * SALSA64_RT_SLOW_EXEC_COP0, *pcs = p + 12 + 12 * SALSA64_RT_SLOW_N;
		CHECK_EQ(rd32(cause), w == 0 ? 3u : 0u);
		CHECK_EQ(rd32(pcs), w == 0 ? 0x80042968u : 0u);
		CHECK_EQ(rd32(pcs + 12), w == 0 ? 3u : 0u);
		p += 4 + len;
		if (w == 0) {
			CHECK_EQ(rd32(p), (uint32_t)PERF_R_COP0 << 24 | 24);
			CHECK_EQ(rd32(p + 8) >> 16, 1u);
			CHECK_EQ(rd32(p + 12) >> 16, SALSA64_RT_COP0_MT + 12u);
			CHECK_EQ(rd32(p + 16), 1u);
			CHECK_EQ(rd32(p + 24), 30u);
		} else {
			CHECK_EQ(ring.head, 4 + len);  /* sin registro COP0 */
		}
	}
}

static void put_file(const char *path, size_t bytes)
{
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (!f)
		return;
	for (size_t k = 0; k < bytes; k++)
		fputc('x', f);
	fclose(f);
	struct utimbuf t = { 1000000, 1000000 };  /* todas con la misma fecha: decide el número */
	utime(path, &t);
}

/* El tope borra las sesiones más viejas (y sus volcados), nunca el perfil
 * persistente ni la que se indica. */
static void rotation_keeps_newest(void)
{
	if (!t_tmp_dir)
		return;
	static unsigned runs;
	char root[256], dir[300], path[400];
	snprintf(root, sizeof root, "%s/perf2-%u", t_tmp_dir, runs++);
	mkdir(root, 0777);
	snprintf(dir, sizeof dir, "%s/A-00000001", root);
	mkdir(dir, 0777);
	snprintf(path, sizeof path, "%s/volcados", dir);
	mkdir(path, 0777);
	for (unsigned k = 1; k <= 3; k++) {
		snprintf(path, sizeof path, "%s/%u.sesion", dir, k);
		put_file(path, 1000);
	}
	snprintf(path, sizeof path, "%s/juego.prf", dir);
	put_file(path, 5000);
	snprintf(path, sizeof path, "%s/volcados/1-300-desconocido.bin", dir);
	put_file(path, 500);
	unsigned sessions;
	CHECK_EQ(perf_dir_bytes(root, &sessions), 8500);
	CHECK_EQ(sessions, 3);
	/* Misma fecha (el sistema de archivos redondea): decide el número. */
	CHECK_EQ(perf_rotate(root, 7000, NULL), 2);  /* 1.sesion y el volcado 1 */
	snprintf(path, sizeof path, "%s/1.sesion", dir);
	CHECK(access(path, F_OK) != 0);
	snprintf(path, sizeof path, "%s/2.sesion", dir);
	CHECK(access(path, F_OK) == 0);
	snprintf(path, sizeof path, "%s/3.sesion", dir);
	CHECK_EQ(perf_rotate(root, 0, path), 1);  /* solo la 2: la 3 se conserva */
	CHECK(access(path, F_OK) == 0);
	snprintf(path, sizeof path, "%s/juego.prf", dir);
	CHECK(access(path, F_OK) == 0);
}

static void names_cover_all_fields(void)
{
	char text[4096];
	perf_names_text(text, sizeof text);
	CHECK(!strncmp(text, "ventana=campo,dur_ms,", 21));
	CHECK(strstr(text, ",perdidos\ntiron=campo,emu_us,") != NULL);
	unsigned commas = 0;
	const char *t = strstr(text, "tiron=");
	for (const char *p = t; *p && *p != '\n'; p++)
		commas += *p == ',';
	CHECK_EQ(commas + 1, PERF_S_N);
}

void t_suite_perfilado(void)
{
	RUN(cfg_defaults_and_parse);
	RUN(ring_records_round_trip);
	RUN(slow_window_records);
	RUN(rotation_keeps_newest);
	RUN(names_cover_all_fields);
}
