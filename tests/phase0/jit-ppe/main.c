/*
 * Cell64 — Fase 0: prueba de JIT en el PPE (bloqueante para la Fase 3).
 *
 * Para cada mecanismo de memoria de `mechs` escribe una función PowerPC
 * (r3 = r3 * 3 + 7; blr), limpia cachés (dcbst/sync/icbi/isync), la llama
 * con un descriptor de función ELFv1 propio y verifica el resultado. Luego
 * cambia la constante, vuelve a limpiar y a llamar: si sale el valor viejo,
 * el PPE ejecutó instrucciones viejas de la caché.
 *
 * Saltar a una página no ejecutable (o escribir en .text) mata el proceso,
 * así que antes de cada fase arriesgada se agrega una línea "PROBANDO" a
 * JIT_STATE. Al relanzar, un mecanismo que quedó en PROBANDO se registra
 * como CRASH y se sigue con el siguiente. Al terminar todos, el archivo se
 * renombra a JIT_RESULT y la próxima ejecución empieza de cero.
 *
 * Resultados y forma de correrla: docs/jit-ppe.md.
 */
#include <malloc.h>
#include <ppu-types.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/memory.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/systime.h>

#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"

#define JIT_STATE  CELL64_DATA_DIR "/jit_state.txt"
#define JIT_RESULT CELL64_DATA_DIR "/jit_result.txt"
#define JIT_BYTES  (64 * 1024)
#define CACHE_LINE 128
#define FLUSH_REPS 100

SYS_PROCESS_PARAM(1001, 0x100000);

/* --- Código generado ------------------------------------------------------- */

#define PPC_MULLI_R3_R3(imm) (0x1C630000u | ((u32)(imm) & 0xFFFF))
#define PPC_ADDI_R3_R3(imm)  (0x38630000u | ((u32)(imm) & 0xFFFF))
#define PPC_BLR              0x4E800020u

static void emit_func(volatile u32 *code, int add)
{
	code[0] = PPC_MULLI_R3_R3(3);
	code[1] = PPC_ADDI_R3_R3(add);
	code[2] = PPC_BLR;
}

/* Secuencia de la Book II de PowerPC para código modificado. */
static void flush_code(const void *p, size_t n)
{
	uintptr_t start = (uintptr_t)p & ~(uintptr_t)(CACHE_LINE - 1);
	uintptr_t end = (uintptr_t)p + n;

	for (uintptr_t a = start; a < end; a += CACHE_LINE)
		__asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uintptr_t a = start; a < end; a += CACHE_LINE)
		__asm__ volatile("icbi 0,%0" : : "r"(a) : "memory");
	__asm__ volatile("isync" : : : "memory");
}

/* ABI ELFv1 de 64 bits (PSL1GHT): un puntero a función apunta a un
 * descriptor {entrada, TOC, entorno}. Se reutiliza el TOC actual. */
typedef struct {
	u64 entry;
	u64 toc;
	u64 env;
} opd_t;

static opd_t jit_opd;

static int call_code(const volatile u32 *code, int arg)
{
	u64 toc;
	__asm__ volatile("mr %0,2" : "=r"(toc));
	jit_opd.entry = (u64)(uintptr_t)code;
	jit_opd.toc = toc;
	jit_opd.env = 0;
	__asm__ volatile("" : : : "memory");

	int (*fn)(int) = (int (*)(int))(void *)&jit_opd;
	return fn(arg);
}

/* --- Mecanismos de memoria -------------------------------------------------- */

static u32 data_buf[JIT_BYTES / 4] __attribute__((aligned(CACHE_LINE))) = { 1 };
static u32 bss_buf[JIT_BYTES / 4] __attribute__((aligned(CACHE_LINE)));

/* lv2.ld pone .data.rel.ro* en el segmento RW de 0x50000, dentro de los
 * mismos 256 MB que .text (.opd vive ahí): distingue si el no-ejecutable se
 * aplica por segmento (SLB) o por página. */
static u32 seg0_buf[JIT_BYTES / 4]
	__attribute__((aligned(CACHE_LINE), section(".data.rel.ro.cell64_jit"))) = { 1 };

/* Zona de nops dentro de .text: ejecutable, pero en principio de solo lectura. */
__asm__("	.section .text\n"
	"	.balign 128\n"
	"	.globl cell64_text_cave\n"
	"cell64_text_cave:\n"
	"	.rept 16384\n"
	"	nop\n"
	"	.endr\n"
	"	.previous\n");
extern u32 cell64_text_cave[];

static sys_mem_addr_t mm_addr;
static sys_mem_id_t mm_id;

static void *alloc_data(char *err, size_t n)
{
	(void)err, (void)n;
	return data_buf;
}

static void *alloc_bss(char *err, size_t n)
{
	(void)err, (void)n;
	return bss_buf;
}

static void *alloc_seg0(char *err, size_t n)
{
	(void)err, (void)n;
	return seg0_buf;
}

static void *alloc_text(char *err, size_t n)
{
	(void)err, (void)n;
	return cell64_text_cave;
}

static void *alloc_heap(char *err, size_t n)
{
	void *p = memalign(CACHE_LINE, n);
	if (!p)
		snprintf(err, 96, "memalign falló");
	return p;
}

static void *alloc_sysmem(char *err, size_t size, u64 flags)
{
	sys_mem_addr_t addr = 0;
	s32 rc = sysMemoryAllocate(size, flags, &addr);
	if (rc != 0) {
		snprintf(err, 96, "sysMemoryAllocate: 0x%08x", rc);
		return NULL;
	}
	return (void *)(uintptr_t)addr;
}

static void *alloc_sysmem_64k(char *err, size_t n)
{
	(void)n;
	return alloc_sysmem(err, 64 * 1024, SYS_MEMORY_PAGE_SIZE_64K);
}

static void *alloc_sysmem_1m(char *err, size_t n)
{
	(void)n;
	return alloc_sysmem(err, 1024 * 1024, SYS_MEMORY_PAGE_SIZE_1M);
}

static void *alloc_mmapper(char *err, size_t n)
{
	(void)n;
	s32 rc = sysMMapperAllocateAddress(0x10000000, SYS_MEMORY_PAGE_SIZE_1M, 0x10000000, &mm_addr);
	if (rc != 0) {
		snprintf(err, 96, "sysMMapperAllocateAddress: 0x%08x", rc);
		return NULL;
	}
	rc = sysMMapperAllocateMemory(1024 * 1024, SYS_MEMORY_PAGE_SIZE_1M, &mm_id);
	if (rc != 0) {
		snprintf(err, 96, "sysMMapperAllocateMemory: 0x%08x", rc);
		sysMMapperFreeAddress(mm_addr);
		return NULL;
	}
	rc = sysMMapperMapMemory(mm_addr, mm_id, SYS_MEMORY_PROT_READ_WRITE);
	if (rc != 0) {
		snprintf(err, 96, "sysMMapperMapMemory: 0x%08x", rc);
		sysMMapperFreeMemory(mm_id);
		sysMMapperFreeAddress(mm_addr);
		return NULL;
	}
	return (void *)(uintptr_t)mm_addr;
}

static void release_none(void *p)
{
	(void)p;
}

static void release_heap(void *p)
{
	free(p);
}

static void release_sysmem(void *p)
{
	sysMemoryFree((sys_mem_addr_t)(uintptr_t)p);
}

static void release_mmapper(void *p)
{
	sys_mem_id_t id;
	(void)p;
	sysMMapperUnmapMemory(mm_addr, &id);
	sysMMapperFreeMemory(mm_id);
	sysMMapperFreeAddress(mm_addr);
}

typedef struct {
	const char *name;
	const char *desc;
	void *(*alloc)(char *err, size_t n);
	void (*release)(void *p);
} jit_mech_t;

/* .text va al final: lo esperable es que la escritura mate el proceso. */
static const jit_mech_t mechs[] = {
	{ "data",      "arreglo estático en .data",                 alloc_data,       release_none },
	{ "bss",       "arreglo estático en .bss",                  alloc_bss,        release_none },
	{ "seg0",      "arreglo RW en el segmento de .text (0x50000)", alloc_seg0,    release_none },
	{ "heap",      "memalign() de newlib",                      alloc_heap,       release_heap },
	{ "sysmem64k", "sysMemoryAllocate, páginas de 64 KB",       alloc_sysmem_64k, release_sysmem },
	{ "sysmem1m",  "sysMemoryAllocate, páginas de 1 MB",        alloc_sysmem_1m,  release_sysmem },
	{ "mmapper",   "sys_mmapper: reservar, asignar y mapear",   alloc_mmapper,    release_mmapper },
	{ "text",      "zona de nops dentro de .text (escritura directa)", alloc_text, release_none },
};
#define NUM_MECHS ((int)(sizeof mechs / sizeof mechs[0]))

/* --- Estado persistente ----------------------------------------------------- */

enum { ST_PENDING, ST_TRYING, ST_DONE };

static struct {
	int st;
	char phase[24];
	char line[160];
} states[NUM_MECHS];

static int mech_index(const char *name)
{
	for (int i = 0; i < NUM_MECHS; i++)
		if (strcmp(mechs[i].name, name) == 0)
			return i;
	return -1;
}

static void state_append(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void state_append(const char *fmt, ...)
{
	FILE *f = fopen(JIT_STATE, "a");
	if (!f) {
		cell64_log("no se pudo abrir %s: el progreso no se guardará", JIT_STATE);
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static void state_load(void)
{
	FILE *f = fopen(JIT_STATE, "r");
	if (!f)
		return;

	char line[sizeof states[0].line];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char name[24], status[16];
		int off = 0;
		if (sscanf(line, "%23s %15s %n", name, status, &off) < 2)
			continue;
		int i = mech_index(name);
		if (i < 0)
			continue;
		if (strcmp(status, "PROBANDO") == 0) {
			states[i].st = ST_TRYING;
			snprintf(states[i].phase, sizeof states[i].phase, "%s", line + off);
		} else {
			states[i].st = ST_DONE;
			snprintf(states[i].line, sizeof states[i].line, "%s", line);
		}
	}
	fclose(f);
}

static void finish_mech(int i, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish_mech(int i, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(states[i].line, sizeof states[i].line, fmt, ap);
	va_end(ap);
	states[i].st = ST_DONE;
	state_append("%s", states[i].line);
	cell64_log("%s", states[i].line);
}

/* --- Prueba ----------------------------------------------------------------- */

static void run_mech(int i)
{
	const jit_mech_t *m = &mechs[i];
	char err[96] = "";

	cell64_log("== %s: %s", m->name, m->desc);
	void *mem = m->alloc(err, JIT_BYTES);
	if (!mem) {
		finish_mech(i, "%s NO_DISPONIBLE %s", m->name, err);
		return;
	}
	volatile u32 *code = mem;
	cell64_log("%s: código en %p", m->name, mem);

	state_append("%s PROBANDO escritura", m->name);
	emit_func(code, 7);

	state_append("%s PROBANDO ejecucion", m->name);
	flush_code(mem, 3 * sizeof(u32));
	int first = call_code(code, 5);

	state_append("%s PROBANDO reescritura", m->name);
	emit_func(code, 9);
	flush_code(mem, 3 * sizeof(u32));
	int second = call_code(code, 5);

	u64 t0 = sysGetSystemTime();
	for (int k = 0; k < FLUSH_REPS; k++)
		flush_code(mem, JIT_BYTES);
	u64 flush_ns = (sysGetSystemTime() - t0) * 1000 / FLUSH_REPS;

	m->release(mem);

	const int ok = first == 22 && second == 24;
	finish_mech(i, "%s %s f(5)=%d reescrita=%d%s flush64k=%uns", m->name,
		    ok ? "OK" : "FALLO", first, second,
		    (first == 22 && second == 22) ? " (icache vieja)" : "",
		    (unsigned)flush_ns);
}

int main(void)
{
	cell64_log_init("jit-ppe");
	mkdir(CELL64_DATA_DIR, 0777);
	state_load();

	for (int i = 0; i < NUM_MECHS; i++) {
		if (states[i].st == ST_TRYING) {
			cell64_log("%s: el intento anterior terminó el proceso en la fase '%s'",
				   mechs[i].name, states[i].phase);
			finish_mech(i, "%s CRASH %s", mechs[i].name, states[i].phase);
		}
	}

	for (int i = 0; i < NUM_MECHS; i++)
		if (states[i].st == ST_PENDING)
			run_mech(i);

	int executable = 0;
	cell64_log("== resumen");
	for (int i = 0; i < NUM_MECHS; i++) {
		cell64_log("  %s", states[i].line);
		if (strstr(states[i].line, " OK "))
			executable++;
	}

	remove(JIT_RESULT);
	if (rename(JIT_STATE, JIT_RESULT) != 0)
		cell64_log("no se pudo renombrar %s; borrarlo a mano para repetir la prueba", JIT_STATE);
	else
		cell64_log("resultado guardado en %s", JIT_RESULT);

	cell64_log("RESULTADO jit-ppe: %d de %d mecanismos ejecutan código generado",
		   executable, NUM_MECHS);
	cell64_log_shutdown();
	return 0;
}
