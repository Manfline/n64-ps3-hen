/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caché de código del recompilador en la PS3. Ver rec_cache.h.
 */
#include "platform/ps3/rec_cache.h"

#include <ppu-lv2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/systime.h>
#include <sys/thread.h>

/* "ax" + @nobits: ejecutable y sin contenido en el archivo; la ubica
 * mk/rec-cache.ld al final del segmento de texto. */
__asm__("	.section .salsa64_rec_cache,\"ax\",@nobits\n"
	"	.balign 65536\n"
	"	.globl salsa64_rec_cache_mem\n"
	"salsa64_rec_cache_mem:\n"
	"	.space 33554432\n"  /* SALSA64_REC_CACHE_BYTES */
	"	.previous\n");

extern unsigned char salsa64_rec_cache_mem[];
_Static_assert(SALSA64_REC_CACHE_BYTES == 33554432, "actualizar el .space de arriba");

void *salsa64_ps3_rec_cache(void)
{
	return salsa64_rec_cache_mem;
}

/* ---- Prueba de la caché ---------------------------------------------------------- */

/* sys_memory_get_page_attribute (syscall 351), sin envoltura en PSL1GHT;
 * sys_page_attr_t como la documenta RPCS3. */
int32_t salsa64_ps3_page_attr(const void *p, struct salsa64_ps3_page_attr *out)
{
	static struct {
		uint64_t attribute, access_right;
		uint32_t page_size, pad;
		uint64_t reserve[4];
	} a __attribute__((aligned(16)));
	/* Un hilo a la vez: la prueba corre antes de que nadie más pregunte. */
	lv2syscall2(351, (u64)(uintptr_t)p, (u64)(uintptr_t)&a);
	int32_t rc = (int32_t)p1;
	if (rc == 0 && out) {
		out->attribute = a.attribute;
		out->access_right = a.access_right;
		out->page_size = a.page_size;
	}
	return rc;
}

enum {
	ST_START, ST_ATTR, ST_READ, ST_WRITE, ST_FLUSH, ST_EXEC, ST_REWRITE, ST_END_WRITE, ST_END_EXEC, ST_PAGES,
	ST_RESTORE, ST_DONE
};

/* Para el log: «… (la <paso> no volvió en 2 s)». */
static const char *const stage_name[] = {
	[ST_START] = "creación del hilo de prueba",
	[ST_ATTR] = "consulta de la página (syscall 351)",
	[ST_READ] = "lectura de la caché",
	[ST_WRITE] = "escritura en la caché",
	[ST_FLUSH] = "limpieza de cachés (dcbst/icbi)",
	[ST_EXEC] = "ejecución en la caché",
	[ST_REWRITE] = "reescritura y segunda ejecución",
	[ST_END_WRITE] = "escritura al final de la caché",
	[ST_END_EXEC] = "ejecución al final de la caché",
	[ST_PAGES] = "escritura en cada página de la caché",
	[ST_RESTORE] = "limpieza de lo escrito",
	[ST_DONE] = "prueba",
};

static struct {
	volatile int stage;
	volatile int done;      /* el hilo terminó (bien o mal) */
	bool started, decided, ok;
	const char *marker, *tag;
	uint64_t t0, us;
	int32_t attr_rc;
	struct salsa64_ps3_page_attr attr;
	char why[128];
} probe;

/* Opd ELFv1 propio para llamar al código escrito: entrada y el TOC actual. */
static uint64_t probe_opd[3];

#define PPC_MULLI_R3_R3(imm) (0x1C630000u | ((uint32_t)(imm) & 0xFFFF))
#define PPC_ADDI_R3_R3(imm)  (0x38630000u | ((uint32_t)(imm) & 0xFFFF))
#define PPC_BLR              0x4E800020u

static void flush_lines(const void *p, size_t n)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)127, e = (uintptr_t)p + n;
	for (uintptr_t x = a; x < e; x += 128)
		__asm__ volatile("dcbst 0,%0" : : "r"(x) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uintptr_t x = a; x < e; x += 128)
		__asm__ volatile("icbi 0,%0" : : "r"(x) : "memory");
	__asm__ volatile("isync" : : : "memory");
}

/* r3 = r3 * 3 + add; f(5) = 15 + add. */
static int write_and_call(volatile uint32_t *code, int add, int stage_write, int stage_exec)
{
	probe.stage = stage_write;
	code[0] = PPC_MULLI_R3_R3(3);
	code[1] = PPC_ADDI_R3_R3(add);
	code[2] = PPC_BLR;
	probe.stage = ST_FLUSH;
	flush_lines((const void *)code, 12);
	probe.stage = stage_exec;
	uint64_t toc;
	__asm__ volatile("mr %0,2" : "=r"(toc));
	probe_opd[0] = (uint64_t)(uintptr_t)code;
	probe_opd[1] = toc;
	probe_opd[2] = 0;
	__asm__ volatile("" : : : "memory");
	int (*fn)(int) = (int (*)(int))(void *)probe_opd;
	return fn(5);
}

static bool marker_says_closed(void)
{
	char line[96];
	FILE *f = fopen(probe.marker, "r");
	if (!f)
		return false;
	bool closed = false;
	if (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		const char *v = !strncmp(line, "probando ", 9) ? line + 9 : !strncmp(line, "cerró ", 7) ? line + 7 : NULL;
		closed = v && !strcmp(v, probe.tag);
	}
	fclose(f);
	return closed;
}

static void marker_write(const char *what)
{
	FILE *f = fopen(probe.marker, "w");
	if (f) {
		fprintf(f, "%s %s\n", what, probe.tag);
		fclose(f);
	}
}

static void probe_fail(const char *fmt, int a, int b)
{
	snprintf(probe.why, sizeof probe.why, fmt, a, b);
	probe.ok = false;
}

static void probe_thread(void *arg)
{
	(void)arg;
	volatile uint32_t *cache = salsa64_ps3_rec_cache();
	volatile uint32_t *end = (volatile uint32_t *)(salsa64_rec_cache_mem + SALSA64_REC_CACHE_BYTES - 128);

	if (probe.marker) {
		if (marker_says_closed()) {
			marker_write("cerró");
			snprintf(probe.why, sizeof probe.why, "una prueba anterior cerró la aplicación; borrar %s para repetirla",
				 probe.marker);
			goto out;
		}
		marker_write("probando");
	}

	probe.stage = ST_ATTR;
	probe.attr_rc = salsa64_ps3_page_attr((const void *)cache, &probe.attr);
	probe.stage = ST_READ;
	if (cache[0] != 0 || end[0] != 0) {
		/* Cola NOBITS: el cargador la da en ceros. Otra cosa es otro mapeo. */
		probe_fail("la caché no llegó en ceros (0x%08x, 0x%08x)", (int)cache[0], (int)end[0]);
		goto done;
	}
	int r = write_and_call(cache, 7, ST_WRITE, ST_EXEC);
	if (r != 22) {
		probe_fail("la ejecución dio %d en vez de %d", r, 22);
		goto done;
	}
	r = write_and_call(cache, 9, ST_REWRITE, ST_REWRITE);
	if (r != 24) {
		probe_fail("la reescritura dio %d en vez de %d (caché de instrucciones vieja)", r, 24);
		goto done;
	}
	r = write_and_call(end, 4, ST_END_WRITE, ST_END_EXEC);
	if (r != 19) {
		probe_fail("la ejecución al final dio %d en vez de %d", r, 19);
		goto done;
	}
	probe.stage = ST_PAGES;
	for (size_t off = 0; off < SALSA64_REC_CACHE_BYTES; off += 65536) {
		volatile uint32_t *w = (volatile uint32_t *)(salsa64_rec_cache_mem + off + 64);
		*w = *w;
	}
	probe.stage = ST_RESTORE;
	for (int k = 0; k < 3; k++)
		cache[k] = end[k] = 0;
	flush_lines((const void *)cache, 12);
	flush_lines((const void *)end, 12);
	probe.ok = true;
done:
	if (probe.marker)
		remove(probe.marker);
out:
	probe.us = sysGetSystemTime() - probe.t0;
	probe.stage = ST_DONE;
	__asm__ volatile("sync" : : : "memory");
	probe.done = 1;
	sysThreadExit(0);
}

void salsa64_ps3_rec_probe_start(const char *marker, const char *tag)
{
	sys_ppu_thread_t tid;
	if (probe.started)
		return;
	probe.started = true;
	probe.marker = marker;
	probe.tag = tag ? tag : "";
	probe.stage = ST_START;
	probe.t0 = sysGetSystemTime();
	/* Prioridad baja: si el fallo deja el hilo girando, que no le quite el
	 * PPE a nadie. Sin unirse: si no vuelve, se queda donde quedó. */
	if (sysThreadCreate(&tid, probe_thread, NULL, SALSA64_REC_PROBE_PRIO, 0x10000, 0, (char *)"salsa64_jit_prueba") != 0) {
		snprintf(probe.why, sizeof probe.why, "no se pudo crear el hilo de prueba");
		probe.done = 1;
	}
}

bool salsa64_ps3_rec_probe_wait(char *why, size_t n, bool *first)
{
	*first = !probe.decided;
	if (!probe.started)
		salsa64_ps3_rec_probe_start(NULL, NULL);
	if (!probe.decided) {
		while (!probe.done && sysGetSystemTime() - probe.t0 < SALSA64_REC_PROBE_US)
			sysUsleep(1000);
		__asm__ volatile("lwsync" : : : "memory");
		if (!probe.done) {
			int st = probe.stage;
			snprintf(probe.why, sizeof probe.why, "la %s no volvió en %u s",
				 stage_name[st >= ST_START && st <= ST_DONE ? st : ST_DONE], SALSA64_REC_PROBE_US / 1000000u);
			probe.ok = false;
			/* El proceso sigue vivo: el archivo de marca no hace falta. */
			if (probe.marker)
				remove(probe.marker);
		}
		probe.decided = true;
	}
	if (probe.ok) {
		const struct salsa64_ps3_page_attr *a = &probe.attr;
		if (probe.attr_rc == 0)
			snprintf(why, n, "%u µs; página de %u KB, atributo 0x%llx, acceso 0x%llx", (unsigned)probe.us,
				 a->page_size >> 10, (unsigned long long)a->attribute, (unsigned long long)a->access_right);
		else
			snprintf(why, n, "%u µs; syscall 351: 0x%08x", (unsigned)probe.us, (unsigned)probe.attr_rc);
	} else {
		snprintf(why, n, "%s", probe.why);
	}
	return probe.ok;
}
