/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — prueba del JIT para cualquier consola (PRUEBA=cfw, «Salsa64 JIT
 * test»). La corre quien tenga CFW (issue #2: con Evilnat 4.91 el
 * recompilador no arrancaba) o HEN, para comparar.
 *
 * Por cada vía de memoria escribe r3 = r3 * 3 + 7; blr, limpia cachés, lo
 * llama, cambia la constante y lo vuelve a llamar (como tests/phase0/jit-ppe
 * main.c), y además:
 *   - la prueba que hace la aplicación antes de usar su caché
 *     (platform/ps3/rec_cache.h), sobre la misma caché: misma cola NOBITS
 *     de mk/rec-cache.ld;
 *   - escrituras por el sistema en vez de directas: PS3MAPI (syscall 8 de
 *     Cobra/Mamba y de HEN: escribir memoria del proceso y reservar páginas
 *     ejecutables) y sys_dbg_write_process_memory (syscall 905).
 *
 * Cada vía corre en un hilo aparte y el principal la espera con tiempo
 * límite por fase: con ese CFW un fallo de página deja el hilo trabado en
 * vez de matar el proceso, y entonces la vía queda COLGADO y se sigue con la
 * siguiente sin reiniciar. Si el fallo mata el proceso (así pasa con HEN) o
 * cuelga la consola, la fase quedó como PROBANDO en JIT_STATE: al volver a
 * abrir la prueba se anota CRASH y sigue. Al terminar, JIT_STATE pasa a
 * JIT_RESULT.
 *
 * En pantalla, el avance y las instrucciones (inglés y español). Ver
 * docs/jit-cfw.md.
 */
#include <io/pad.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/memory.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>

#include "rsxkit/rsxkit.h"
#include "rsxkit/text.h"

#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"
#include "platform/ps3/rec_cache.h"

#define TEST_VERSION "1"
#define JIT_STATE    SALSA64_DATA_DIR "/jit_state.txt"
#define JIT_RESULT   SALSA64_DATA_DIR "/jit_result.txt"
#define JIT_LOG      SALSA64_DATA_DIR "/jit-test.log"
#define JIT_BYTES    (64 * 1024)
#define CACHE_LINE   128
#define PHASE_US     4000000u  /* una fase que tarda más quedó trabada */
#define CHILD_PRIO   1500      /* por debajo del principal (1001): si gira, la pantalla sigue */
#define LINE_MAX     512

SYS_PROCESS_PARAM(1001, 0x100000);

/* ---- Código generado ------------------------------------------------------- */

#define PPC_MULLI_R3_R3(imm) (0x1C630000u | ((u32)(imm) & 0xFFFF))
#define PPC_ADDI_R3_R3(imm)  (0x38630000u | ((u32)(imm) & 0xFFFF))
#define PPC_BLR              0x4E800020u

static void make_func(u32 out[3], int add)
{
	out[0] = PPC_MULLI_R3_R3(3);
	out[1] = PPC_ADDI_R3_R3(add);
	out[2] = PPC_BLR;
}

static void flush_code(const void *p, size_t n)
{
	uintptr_t start = (uintptr_t)p & ~(uintptr_t)(CACHE_LINE - 1), end = (uintptr_t)p + n;
	for (uintptr_t a = start; a < end; a += CACHE_LINE)
		__asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uintptr_t a = start; a < end; a += CACHE_LINE)
		__asm__ volatile("icbi 0,%0" : : "r"(a) : "memory");
	__asm__ volatile("isync" : : : "memory");
}

/* Descriptor ELFv1 propio por hilo (entrada, TOC actual). */
static int call_code(u64 opd[3], const volatile u32 *code, int arg)
{
	u64 toc;
	__asm__ volatile("mr %0,2" : "=r"(toc));
	opd[0] = (u64)(uintptr_t)code;
	opd[1] = toc;
	opd[2] = 0;
	__asm__ volatile("" : : : "memory");
	int (*fn)(int) = (int (*)(int))(void *)opd;
	return fn(arg);
}

/* ---- Llamadas al sistema --------------------------------------------------- */

/* PS3MAPI por la syscall 8 (Cobra/Mamba en CFW, también en HEN). Opcodes y
 * argumentos como los usa ps3mapi-lib (MIT, github.com/bucanero/ps3mapi-lib). */
#define SC8_PS3MAPI              0x7777
#define PS3MAPI_GET_CORE_VERSION 0x0011
#define PS3MAPI_GET_FW_TYPE      0x0013
#define PS3MAPI_GET_FW_VERSION   0x0014
#define PS3MAPI_SET_PROC_MEM     0x0032
#define PS3MAPI_PAGE_ALLOCATE    0x0033
#define PS3MAPI_PAGE_FREE        0x0034
#define PS3MAPI_PAGE_FLAGS       0x2F   /* como webMAN MOD al cargar código en un juego */
#define PS3MAPI_PAGE_SIZE        0x100

static s32 mapi_core_version(void)
{
	lv2syscall2(8, SC8_PS3MAPI, PS3MAPI_GET_CORE_VERSION);
	return_to_user_prog(s32);
}

static s32 mapi_fw_type(char *buf)
{
	lv2syscall3(8, SC8_PS3MAPI, PS3MAPI_GET_FW_TYPE, (u64)(uintptr_t)buf);
	return_to_user_prog(s32);
}

static s32 mapi_fw_version(void)
{
	lv2syscall2(8, SC8_PS3MAPI, PS3MAPI_GET_FW_VERSION);
	return_to_user_prog(s32);
}

static s32 mapi_set_mem(u32 pid, u64 addr, const void *buf, u32 size)
{
	lv2syscall6(8, SC8_PS3MAPI, PS3MAPI_SET_PROC_MEM, pid, addr, (u64)(uintptr_t)buf, size);
	return_to_user_prog(s32);
}

static s32 mapi_page_alloc(u32 pid, u64 size, u64 *table)
{
	lv2syscall8(8, SC8_PS3MAPI, PS3MAPI_PAGE_ALLOCATE, pid, size, PS3MAPI_PAGE_SIZE, PS3MAPI_PAGE_FLAGS, 1,
		    (u64)(uintptr_t)table);
	return_to_user_prog(s32);
}

static s32 mapi_page_free(u32 pid, u64 *table)
{
	lv2syscall5(8, SC8_PS3MAPI, PS3MAPI_PAGE_FREE, pid, PS3MAPI_PAGE_FLAGS, (u64)(uintptr_t)table);
	return_to_user_prog(s32);
}

/* sys_dbg_write_process_memory (pid, dirección, tamaño, datos). */
static s32 dbg_write(u32 pid, u64 addr, u32 size, const void *buf)
{
	lv2syscall4(905, pid, addr, size, (u64)(uintptr_t)buf);
	return_to_user_prog(s32);
}

/* sys_memory_get_user_memory_size (syscall 352). */
static struct {
	u32 total, available;
} user_mem;

static s32 get_user_memory(void)
{
	lv2syscall1(352, (u64)(uintptr_t)&user_mem);
	return_to_user_prog(s32);
}

/* ---- Estado persistente ----------------------------------------------------- */

static void state_append(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void state_append(const char *fmt, ...)
{
	FILE *f = fopen(JIT_STATE, "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

/* ---- Vías ------------------------------------------------------------------ */

struct run {
	const char *name;
	const char *volatile phase;
	volatile u64 phase_t0;
	volatile int done;
	u64 opd[3];
	char line[LINE_MAX];
};

static void phase(struct run *r, const char *ph)
{
	state_append("%s PROBANDO %s", r->name, ph);
	r->phase = ph;
	r->phase_t0 = sysGetSystemTime();
}

static void result(struct run *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result(struct run *r, const char *fmt, ...)
{
	int n = snprintf(r->line, sizeof r->line, "%s ", r->name);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r->line + n, sizeof r->line - n, fmt, ap);
	va_end(ap);
}

/* Cómo llega el código a la memoria: directo o por una llamada al sistema.
 * Devuelve 0 o el código de error. */
typedef s32 (*writer_fn)(volatile u32 *dst, const void *src, u32 bytes);

static s32 write_direct(volatile u32 *dst, const void *src, u32 bytes)
{
	const u32 *s = src;
	for (u32 k = 0; k < bytes / 4; k++)
		dst[k] = s[k];
	return 0;
}

static s32 write_mapi(volatile u32 *dst, const void *src, u32 bytes)
{
	return mapi_set_mem(sysProcessGetPid(), (u64)(uintptr_t)dst, src, bytes);
}

static s32 write_dbg(volatile u32 *dst, const void *src, u32 bytes)
{
	return dbg_write(sysProcessGetPid(), (u64)(uintptr_t)dst, bytes, src);
}

static u32 zeros64k[JIT_BYTES / 4] __attribute__((aligned(CACHE_LINE)));

/* Escribe, ejecuta, reescribe y vuelve a ejecutar en `code`; mide escribir
 * 12 bytes, escribir 64 KB en `scratch` (si no es NULL) y limpiar 64 KB. */
static void test_code(struct run *r, volatile u32 *code, volatile u32 *scratch, writer_fn wr, const char *note)
{
	u32 fn[3];
	make_func(fn, 7);
	phase(r, "escritura");
	s32 rc = wr(code, fn, sizeof fn);
	if (rc != 0) {
		result(r, "FALLO escritura=0x%08x en %p%s", (unsigned)rc, (void *)code, note);
		return;
	}
	if (code[0] != fn[0] || code[1] != fn[1] || code[2] != fn[2]) {
		result(r, "FALLO lectura=%08x %08x %08x en %p%s", (unsigned)code[0], (unsigned)code[1],
		       (unsigned)code[2], (void *)code, note);
		return;
	}
	phase(r, "ejecucion");
	flush_code((const void *)code, sizeof fn);
	int first = call_code(r->opd, code, 5);

	phase(r, "reescritura");
	make_func(fn, 9);
	rc = wr(code, fn, sizeof fn);
	flush_code((const void *)code, sizeof fn);
	int second = rc == 0 ? call_code(r->opd, code, 5) : -1;

	phase(r, "medida");
	u64 t0 = sysGetSystemTime();
	for (int k = 0; k < 100; k++)
		wr(code, fn, sizeof fn);
	unsigned w12 = (unsigned)((sysGetSystemTime() - t0) * 10);  /* ns por escritura */
	unsigned w64 = 0;
	if (scratch) {
		t0 = sysGetSystemTime();
		for (int k = 0; k < 10; k++)
			wr(scratch, zeros64k, JIT_BYTES);
		w64 = (unsigned)((sysGetSystemTime() - t0) / 10);
	}
	t0 = sysGetSystemTime();
	for (int k = 0; k < 100; k++)
		flush_code((const void *)code, JIT_BYTES);
	unsigned fl = (unsigned)((sysGetSystemTime() - t0) * 10);

	bool ok = first == 22 && second == 24;
	result(r, "%s f(5)=%d reescrita=%d%s en %p escribir12=%uns escribir64k=%uus limpiar64k=%uns%s",
	       ok ? "OK" : "FALLO", first, second, first == 22 && second == 22 ? " (icache vieja)" : "", (void *)code,
	       w12, w64, fl, note);
}

/* Zona de nops dentro de .text (ocupa archivo, a diferencia de la cola). */
__asm__("	.section .text\n"
	"	.balign 128\n"
	"	.globl jit_text_cave\n"
	"jit_text_cave:\n"
	"	.fill 16384, 4, 0x60000000\n"
	"	.previous\n");
extern u32 jit_text_cave[];

static u8 *cache_at(u32 mb)
{
	return (u8 *)salsa64_ps3_rec_cache() + ((size_t)mb << 20);
}

static u32 data_mark __attribute__((aligned(16))) = 1;

static void attr_text(char *out, size_t n, const char *what, const void *p)
{
	struct salsa64_ps3_page_attr a;
	s32 rc = salsa64_ps3_page_attr(p, &a);
	if (rc == 0)
		snprintf(out, n, " %s=%p:%uK/0x%llx/0x%llx", what, p, a.page_size >> 10, (unsigned long long)a.attribute,
			 (unsigned long long)a.access_right);
	else
		snprintf(out, n, " %s=%p:rc=0x%08x", what, p, (unsigned)rc);
}

static void run_info(struct run *r)
{
	char fw[16] = "", buf[LINE_MAX] = "", part[96];
	phase(r, "ps3mapi");
	s32 core = mapi_core_version();
	if (core > 0) {
		mapi_fw_type(fw);
		fw[sizeof fw - 1] = '\0';
		snprintf(buf, sizeof buf, "ps3mapi=0x%x fw=%s/0x%x", (unsigned)core, fw, (unsigned)mapi_fw_version());
	} else {
		snprintf(buf, sizeof buf, "ps3mapi=no(0x%08x)", (unsigned)core);
	}
	if (get_user_memory() == 0) {
		snprintf(part, sizeof part, " memoria=%uK/%uK", user_mem.total >> 10, user_mem.available >> 10);
		strncat(buf, part, sizeof buf - strlen(buf) - 1);
	}
	phase(r, "atributos");
	void *heap = malloc(256);
	const void *ptrs[] = { (const void *)jit_text_cave, cache_at(0), cache_at(31), &data_mark, heap, &fw };
	const char *names[] = { "texto", "cache", "cache_fin", "datos", "heap", "pila" };
	for (unsigned i = 0; i < sizeof ptrs / sizeof ptrs[0]; i++) {
		attr_text(part, sizeof part, names[i], ptrs[i]);
		strncat(buf, part, sizeof buf - strlen(buf) - 1);
	}
	free(heap);
	result(r, "OK %s", buf);
}

static void run_app(struct run *r)
{
	char why[160];
	bool first;
	phase(r, "prueba");
	salsa64_ps3_rec_probe_start(NULL, NULL);
	bool ok = salsa64_ps3_rec_probe_wait(why, sizeof why, &first);
	result(r, "%s %s", ok ? "OK" : "FALLO", why);
}

static void run_text(struct run *r)
{
	test_code(r, jit_text_cave, NULL, write_direct, "");
}

static void run_nobits(struct run *r)
{
	test_code(r, (volatile u32 *)cache_at(1), (volatile u32 *)(cache_at(1) + JIT_BYTES), write_direct, "");
}

static void run_mapi_write(struct run *r)
{
	phase(r, "ps3mapi");
	s32 core = mapi_core_version();
	if (core <= 0) {
		result(r, "NO_DISPONIBLE ps3mapi=0x%08x", (unsigned)core);
		return;
	}
	test_code(r, (volatile u32 *)cache_at(2), (volatile u32 *)(cache_at(2) + JIT_BYTES), write_mapi, "");
}

static void run_mapi_pages(struct run *r)
{
	phase(r, "ps3mapi");
	s32 core = mapi_core_version();
	if (core <= 0) {
		result(r, "NO_DISPONIBLE ps3mapi=0x%08x", (unsigned)core);
		return;
	}
	static u64 table[2] __attribute__((aligned(16)));
	phase(r, "reserva");
	s32 rc = mapi_page_alloc(sysProcessGetPid(), 2 * JIT_BYTES, table);
	if (rc != 0 || !table[0]) {
		result(r, "NO_DISPONIBLE reserva=0x%08x", (unsigned)rc);
		return;
	}
	char note[96], attr[80];
	attr_text(attr, sizeof attr, "pagina", (void *)(uintptr_t)table[0]);
	snprintf(note, sizeof note, "%s", attr);
	volatile u32 *mem = (volatile u32 *)(uintptr_t)table[0];
	test_code(r, mem, mem + JIT_BYTES / 4, write_direct, note);
	mapi_page_free(sysProcessGetPid(), table);
}

static void run_dbg(struct run *r)
{
	phase(r, "pid");
	test_code(r, (volatile u32 *)cache_at(3), (volatile u32 *)(cache_at(3) + JIT_BYTES), write_dbg, "");
}

static u32 *heap_buf;

static void run_heap(struct run *r)
{
	phase(r, "reserva");
	if (!heap_buf)
		heap_buf = memalign(CACHE_LINE, 2 * JIT_BYTES);
	if (!heap_buf) {
		result(r, "NO_DISPONIBLE memalign");
		return;
	}
	test_code(r, heap_buf, heap_buf + JIT_BYTES / 4, write_direct, "");
}

static void run_sysmem(struct run *r)
{
	sys_mem_addr_t addr = 0;
	phase(r, "reserva");
	s32 rc = sysMemoryAllocate(128 * 1024, SYS_MEMORY_PAGE_SIZE_64K, &addr);
	if (rc != 0) {
		result(r, "NO_DISPONIBLE sysMemoryAllocate=0x%08x", (unsigned)rc);
		return;
	}
	volatile u32 *mem = (volatile u32 *)(uintptr_t)addr;
	test_code(r, mem, mem + JIT_BYTES / 4, write_direct, "");
	sysMemoryFree(addr);
}

static void run_mmapper(struct run *r)
{
	sys_mem_addr_t addr = 0;
	sys_mem_id_t id;
	phase(r, "reserva");
	s32 rc = sysMMapperAllocateAddress(0x10000000, SYS_MEMORY_PAGE_SIZE_1M, 0x10000000, &addr);
	if (rc != 0) {
		result(r, "NO_DISPONIBLE sysMMapperAllocateAddress=0x%08x", (unsigned)rc);
		return;
	}
	if ((rc = sysMMapperAllocateMemory(1024 * 1024, SYS_MEMORY_PAGE_SIZE_1M, &id)) != 0 ||
	    (rc = sysMMapperMapMemory(addr, id, SYS_MEMORY_PROT_READ_WRITE)) != 0) {
		result(r, "NO_DISPONIBLE sysMMapper=0x%08x", (unsigned)rc);
		return;
	}
	volatile u32 *mem = (volatile u32 *)(uintptr_t)addr;
	test_code(r, mem, mem + JIT_BYTES / 4, write_direct, "");
}

typedef struct {
	const char *name;
	const char *desc;  /* en pantalla, en inglés */
	void (*run)(struct run *r);
} mech_t;

/* Lo que interesa para la aplicación primero; lo que con HEN mata el
 * proceso (memoria de datos), al final. */
static const mech_t mechs[] = {
	{ "info",           "console info",                           run_info },
	{ "app",            "Salsa64's own check of its code cache",  run_app },
	{ "text",           "code area in .text, direct write",       run_text },
	{ "nobits",         "code cache (NOBITS), direct write",      run_nobits },
	{ "ps3mapi_mem",    "code cache, write through PS3MAPI",      run_mapi_write },
	{ "ps3mapi_paginas", "executable pages from PS3MAPI",         run_mapi_pages },
	{ "dbg905",         "code cache, write through syscall 905",  run_dbg },
	{ "heap",           "heap memory",                            run_heap },
	{ "sysmem64k",      "sysMemoryAllocate, 64 KB pages",         run_sysmem },
	{ "mmapper",        "sys_mmapper",                            run_mmapper },
};
#define NUM_MECHS ((int)(sizeof mechs / sizeof mechs[0]))

enum { ST_PENDING, ST_TRYING, ST_DONE };

static struct {
	int st;
	char phase[24];
	char line[LINE_MAX + 64];
} states[NUM_MECHS];
static struct run runs[NUM_MECHS];

static int mech_index(const char *name)
{
	for (int i = 0; i < NUM_MECHS; i++)
		if (strcmp(mechs[i].name, name) == 0)
			return i;
	return -1;
}

static int state_load(void)
{
	FILE *f = fopen(JIT_STATE, "r");
	if (!f)
		return 0;
	char line[LINE_MAX + 64];
	int runs_before = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "# arranque", 10))
			runs_before++;
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
	return runs_before;
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
	salsa64_log("%s", states[i].line);
}

static void child_thread(void *arg)
{
	struct run *r = arg;
	mechs[r - runs].run(r);
	__asm__ volatile("sync" : : : "memory");
	r->done = 1;
	sysThreadExit(0);
}

/* ---- Pantalla ------------------------------------------------------------- */

#define CANVAS_W 640
#define CANVAS_H 360

static u32 canvas_px[CANVAS_W * CANVAS_H] __attribute__((aligned(128)));
static rsxkit_fb *fb;
static volatile int quit, after_system;
static int current = -1;   /* vía en curso */
static bool finished;
static int relaunches;

static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)param, (void)user;
	if (status == SYSUTIL_EXIT_GAME)
		quit = 1;
	else if (status == SYSUTIL_DRAW_END)
		after_system = 1;
}

static const char *short_status(int i)
{
	if (states[i].st != ST_DONE)
		return i == current ? "..." : "";
	const char *s = states[i].line + strlen(mechs[i].name) + 1;
	return !strncmp(s, "OK", 2) ? "OK" : !strncmp(s, "FALLO", 5) ? "FAIL" : !strncmp(s, "COLGADO", 7) ? "HUNG"
	       : !strncmp(s, "CRASH", 5) ? "CRASH" : !strncmp(s, "NO_DISPONIBLE", 13) ? "N/A" : "?";
}

static void ui_frame(void)
{
	if (after_system) {
		after_system = 0;
		rsxkit_after_system();
	}
	if (!fb || !rsxkit_frame_ready()) {
		sysUsleep(16000);
		return;
	}
	const rsxkit_canvas c = { canvas_px, CANVAS_W, CANVAS_H, CANVAS_W * 4, RSXKIT_XRGB8888 };
	rsxkit_fill(&c, 0, 0, CANVAS_W, CANVAS_H, 0x101820);
	int y = 12;
	rsxkit_text_draw(&c, 16, y, "Salsa64 JIT test / prueba del JIT  (v" TEST_VERSION ")", 0, 0xFFD040, 1);
	y += 22;
	for (int i = 0; i < NUM_MECHS; i++, y += 16) {
		char row[96];
		snprintf(row, sizeof row, "%2d. %-16s %s", i + 1, mechs[i].name, mechs[i].desc);
		rsxkit_text_draw(&c, 16, y, row, 62, i == current ? 0xFFFFFF : 0xB0B8C0, 1);
		const char *s = short_status(i);
		uint32_t col = !strcmp(s, "OK") ? 0x60E060 : !strcmp(s, "...") ? 0xFFFFFF : !strcmp(s, "N/A") ? 0xA0A0A0
			       : 0xFF7060;
		rsxkit_text_draw(&c, 16 + 8 * 64, y, s, 0, col, 1);
	}
	y += 12;
	if (finished) {
		rsxkit_text_draw(&c, 16, y, "DONE. Please attach these two files to the GitHub issue:", 0, 0x60E060, 1);
		rsxkit_text_draw(&c, 16, y + 16, "  " JIT_RESULT "   and   " JIT_LOG, 0, 0xFFFFFF, 1);
		rsxkit_text_draw(&c, 16, y + 32, "Press X (or PS button > Quit) to exit.", 0, 0xFFFFFF, 1);
		rsxkit_text_draw(&c, 16, y + 56, "LISTO. Adjunta esos dos archivos al issue de GitHub.", 0, 0x60E060, 1);
		rsxkit_text_draw(&c, 16, y + 72, "Pulsa X (o el botón PS > Salir) para salir.", 0, 0xFFFFFF, 1);
	} else {
		char row[96];
		if (current >= 0) {
			snprintf(row, sizeof row, "Testing %s (%s)... please wait / espera", mechs[current].name,
				 runs[current].phase ? (const char *)runs[current].phase : "");
			rsxkit_text_draw(&c, 16, y, row, 76, 0xFFFFFF, 1);
		}
		rsxkit_text_draw(&c, 16, y + 20, "If it returns to the XMB or freezes: turn off if needed, then open the test", 0,
				 0xB0B8C0, 1);
		rsxkit_text_draw(&c, 16, y + 36, "again. It continues where it stopped. / Si vuelve al XMB o se congela:", 0,
				 0xB0B8C0, 1);
		rsxkit_text_draw(&c, 16, y + 52, "apaga si hace falta y ábrela de nuevo: sigue donde se quedó.", 0, 0xB0B8C0,
				 1);
	}
	if (relaunches) {
		char row[64];
		snprintf(row, sizeof row, "(restart %d / reinicio %d)", relaunches, relaunches);
		rsxkit_text_draw(&c, 16, CANVAS_H - 18, row, 0, 0x808890, 1);
	}
	rsxkit_frame_begin(0x000000);
	rsxkit_fb_upload(fb, canvas_px, CANVAS_W, CANVAS_H, CANVAS_W * 4, RSXKIT_XRGB8888);
	const rsxkit_view v = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_NEAREST, 0 };
	rsxkit_fb_draw(fb, &v, NULL);
	rsxkit_frame_end();
}

static bool cross_pressed(void)
{
	padInfo info;
	padData data;
	if (ioPadGetInfo(&info) != 0 || (info.info & 1))
		return false;
	for (int i = 0; i < 7; i++)
		if (info.status[i] && ioPadGetData(i, &data) == 0 && data.len > 0 && data.BTN_CROSS)
			return true;
	return false;
}

static void ui_step(void)
{
	ui_frame();
	sysUtilCheckCallback();
}

/* ---- Programa -------------------------------------------------------------- */

int main(void)
{
	salsa64_log_init("jit-test");
	mkdir(SALSA64_DATA_DIR, 0777);
	ioPadInit(7);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	int rc = rsxkit_init(NULL);
	fb = rc == RSXKIT_OK ? rsxkit_fb_create(CANVAS_W, CANVAS_H) : NULL;
	if (!fb)
		salsa64_log("sin pantalla (rsxkit %d): la prueba sigue solo con el log", rc);

	relaunches = state_load();
	state_append("# arranque %d, prueba v%s, caché en %p", relaunches + 1, TEST_VERSION, salsa64_ps3_rec_cache());
	salsa64_log("prueba del JIT v%s, arranque %d; caché de la aplicación en %p", TEST_VERSION, relaunches + 1,
		    salsa64_ps3_rec_cache());
	for (int i = 0; i < NUM_MECHS; i++) {
		if (states[i].st == ST_TRYING) {
			salsa64_log("%s: el intento anterior terminó el proceso en la fase '%s'", mechs[i].name,
				    states[i].phase);
			finish_mech(i, "%s CRASH %s", mechs[i].name, states[i].phase);
		}
	}

	for (int i = 0; i < NUM_MECHS && !quit; i++) {
		if (states[i].st != ST_PENDING)
			continue;
		struct run *r = &runs[i];
		r->name = mechs[i].name;
		r->phase = "inicio";
		r->phase_t0 = sysGetSystemTime();
		current = i;
		salsa64_log("== %s: %s", mechs[i].name, mechs[i].desc);
		sys_ppu_thread_t tid;
		if (sysThreadCreate(&tid, child_thread, r, CHILD_PRIO, 0x10000, 0, (char *)"jit_test") != 0) {
			finish_mech(i, "%s NO_DISPONIBLE hilo", mechs[i].name);
			continue;
		}
		while (!r->done && !quit && sysGetSystemTime() - r->phase_t0 < PHASE_US)
			ui_step();
		__asm__ volatile("lwsync" : : : "memory");
		if (r->done)
			finish_mech(i, "%s", r->line);
		else if (!quit)
			finish_mech(i, "%s COLGADO %s", mechs[i].name, (const char *)r->phase);
	}
	current = -1;

	if (!quit) {
		int ok = 0;
		salsa64_log("== resumen");
		for (int i = 0; i < NUM_MECHS; i++) {
			salsa64_log("  %s", states[i].line);
			if (i > 0 && !strcmp(short_status(i), "OK"))
				ok++;
		}
		remove(JIT_RESULT);
		if (rename(JIT_STATE, JIT_RESULT) != 0)
			salsa64_log("no se pudo renombrar %s", JIT_STATE);
		else
			salsa64_log("resultado guardado en %s", JIT_RESULT);
		salsa64_log("RESULTADO jit-test: %d de %d vías ejecutan código generado", ok, NUM_MECHS - 1);
		finished = true;
		/* Sin pantalla o en tools/rpcs3-run.py: terminar. */
		bool wait = fb && !salsa64_autotest();
		while (!quit && wait && !cross_pressed())
			ui_step();
	}
	salsa64_log_shutdown();
	if (fb)
		rsxkit_shutdown();
	return 0;
}
