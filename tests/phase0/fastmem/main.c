/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — Fase 0: viabilidad de fastmem en el PPE con lv2 (docs/fastmem.md).
 *
 * Fastmem es que un acceso de la N64 a la RDRAM sea una carga o un
 * almacenamiento del PPE con base + desplazamiento, sin comprobar antes el
 * segmento ni el tamaño: lo que no es RDRAM tiene que caer en una página sin
 * mapear y el fallo de página tiene que poder atraparse y reanudarse desde
 * otro hilo (lv2 no tiene señales). Este programa pregunta eso a lv2, por
 * fases, con el mismo archivo de estado que jit-ppe: una fase que mata el
 * proceso queda como CRASH al relanzar. Hay que relanzarlo hasta ver
 * «RESULTADO fastmem».
 *
 *   reservas            cuánto espacio de direcciones da sys_mmapper_allocate_address
 *                       (256 MB a 2 GB, páginas de 64 KB o 1 MB, con y sin
 *                       derechos de acceso), cuántas regiones de 256 MB a la
 *                       vez y la región fija (syscall 326);
 *   mapeo               asignar, mapear, desmapear y remapear objetos de 64 KB
 *                       y 1 MB (µs por operación), el mismo objeto en dos
 *                       direcciones (espejo), buscar sitio, cambiar permisos
 *                       (336), atributos de página (351) y cuántos objetos;
 *   datos               ns por lectura recorriendo 8 MB en el heap y en
 *                       páginas mapeadas de 64 KB y de 1 MB (paso 128 y 4224);
 *   fallo_evento        con la notificación de fallos (327) y una cola de
 *                       eventos: un hilo lee una página sin mapear, otro
 *                       recibe el evento, mapea la página, escribe un valor
 *                       y reanuda al primero (57), sin pedir el contexto:
 *                       ¿repite la carga y lee ese valor? ¿cuánto tarda?
 *   fallo_contexto      lo mismo pidiendo antes el contexto (58), que se
 *                       vuelca tal cual (no hay documentación pública de su
 *                       formato; se busca en él la dirección del fallo);
 *   fallo_escritura     como fallo_evento con un almacenamiento;
 *   fallo_parche        la carga está en código generado (cola NOBITS, como
 *                       la caché del recompilador): el otro hilo pide el
 *                       contexto (busca el pc de la carga y un valor marcado
 *                       que la víctima deja en r5), reemplaza la carga por
 *                       `li r3, marca`, limpia la caché de instrucciones y
 *                       reanuda: ¿el hilo sigue por la instrucción nueva?
 *   fallo_main          lo mismo con el hilo principal como víctima;
 *   fallo_tiempo        diez fallos parchados seguidos: µs por fallo;
 *   fallo_solo_lectura  página de solo lectura (336): ¿el almacenamiento avisa
 *                       con causa «solo lectura» y sigue al dar permiso?
 *
 * Cada arranque escribe su propio log (fastmem-N.log: el de la consola se
 * pisaba y se perdió la primera corrida), el archivo de estado se escribe
 * sin búfer, y el ayudante anota en él cada paso de un fallo («PASO»), para
 * saber dónde murió el proceso si muere.
 */
#include <lv2/memory.h>
#include <lv2/thread.h>
#include <fcntl.h>
#include <malloc.h>
#include <ppu-asm.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event_queue.h>
#include <sys/memory.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <unistd.h>

#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"

#define FM_STATE  SALSA64_DATA_DIR "/fastmem_state.txt"
#define FM_RESULT SALSA64_DATA_DIR "/fastmem_result.txt"
#define FM_LAUNCH SALSA64_DATA_DIR "/fastmem_launch.txt"
#define MB        (1024u * 1024u)
#define CACHE_LINE 128
#define FM_PATTERN 0x5A5AC0DEu
#define FM_MAGIC   0x5A5AA5A5DEADBEEFull
#define FM_MARK    0x1234
#define FM_CAVE_BYTES (64 * 1024)
#define STR_(x) #x
#define STR(x)  STR_(x)

SYS_PROCESS_PARAM(1001, 0x100000);

/* --- Syscalls sin envoltura en PSL1GHT ---------------------------------------- */

static s32 sc_allocate_fixed_address(void)
{
	lv2syscall0(326);
	return_to_user_prog(s32);
}

static s32 sc_enable_page_fault_notification(u32 start, u32 queue)
{
	lv2syscall2(327, start, queue);
	return_to_user_prog(s32);
}

static s32 sc_change_access_right(u32 addr, u64 flags)
{
	lv2syscall2(336, addr, flags);
	return_to_user_prog(s32);
}

static s32 sc_page_attribute(u32 addr, void *attr)
{
	lv2syscall2(351, addr, (u64)(uintptr_t)attr);
	return_to_user_prog(s32);
}

static s32 sc_user_memory(void *p)
{
	lv2syscall1(352, (u64)(uintptr_t)p);
	return_to_user_prog(s32);
}

static s32 sc_page_fault_context(u64 tid, void *ctx)
{
	lv2syscall2(58, tid, (u64)(uintptr_t)ctx);
	return_to_user_prog(s32);
}

/* --- Tiempo ---------------------------------------------------------------------- */

static u64 tb_freq = 79800000;

static double tb_us(u64 ticks)
{
	return (double)ticks * 1e6 / (double)tb_freq;
}

/* --- Código generado (como jit-ppe) ---------------------------------------------- */

/* Cola sin contenido en el archivo: nobits.ld la pone al final del segmento
 * de texto y el cargador la rellena con ceros; es ejecutable y escribible. */
__asm__("	.section .salsa64_jit_nobits,\"ax\",@nobits\n"
	"	.balign 65536\n"
	"	.globl salsa64_jit_nobits\n"
	"salsa64_jit_nobits:\n"
	"	.space " STR(FM_CAVE_BYTES) "\n"
	"	.previous\n");
extern u32 salsa64_jit_nobits[];

#define PPC_LWZ_R3_R3  0x80630000u /* lwz r3,0(r3) */
#define PPC_STW_R4_R3  0x90830000u /* stw r4,0(r3) */
#define PPC_LI_R3(v)   (0x38600000u | ((u32)(v) & 0xFFFF))
#define PPC_BLR        0x4E800020u

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

/* Función k de la cola (una por línea de caché): lwz r3,0(r3); blr, o
 * stw r4,0(r3); li r3,0; blr. */
static volatile u32 *cave_fn(unsigned k, bool store)
{
	volatile u32 *code = salsa64_jit_nobits + k * (CACHE_LINE / 4);
	if (store) {
		code[0] = PPC_STW_R4_R3;
		code[1] = PPC_LI_R3(0);
		code[2] = PPC_BLR;
	} else {
		code[0] = PPC_LWZ_R3_R3;
		code[1] = PPC_BLR;
	}
	flush_code((const void *)code, 3 * sizeof(u32));
	return code;
}

/* ABI ELFv1 de 64 bits (PSL1GHT): un puntero a función apunta a un
 * descriptor {entrada, TOC, entorno}. Se reutiliza el TOC actual. */
typedef struct {
	u64 entry;
	u64 toc;
	u64 env;
} opd_t;

static u64 call_code(const volatile u32 *code, u64 r3, u64 r4, u64 r5)
{
	static opd_t opd[16];
	static unsigned next;
	opd_t *d = &opd[next++ & 15];
	u64 toc;
	__asm__ volatile("mr %0,2" : "=r"(toc));
	d->entry = (u64)(uintptr_t)code;
	d->toc = toc;
	d->env = 0;
	__asm__ volatile("" : : : "memory");
	u64 (*fn)(u64, u64, u64) = (u64 (*)(u64, u64, u64))(void *)d;
	return fn(r3, r4, r5);
}

/* --- Estado persistente (como jit-ppe) -------------------------------------------- */

enum { ST_PENDING, ST_TRYING, ST_DONE };

typedef struct {
	const char *name;
	void (*run)(int i);
	bool fault; /* usa la notificación de fallos: se omite si fallo_evento mató el proceso */
} phase_t;

static void phase_reservas(int i);
static void phase_mapeo(int i);
static void phase_datos(int i);
static void phase_fallo_evento(int i);
static void phase_fallo_contexto(int i);
static void phase_fallo_escritura(int i);
static void phase_fallo_solo_lectura(int i);
static void phase_fallo_parche(int i);
static void phase_fallo_main(int i);
static void phase_fallo_tiempo(int i);

static const phase_t phases[] = {
	{ "reservas",           phase_reservas,           false },
	{ "mapeo",              phase_mapeo,              false },
	{ "datos",              phase_datos,              false },
	{ "fallo_evento",       phase_fallo_evento,       true },
	{ "fallo_contexto",     phase_fallo_contexto,     true },
	{ "fallo_escritura",    phase_fallo_escritura,    true },
	{ "fallo_parche",       phase_fallo_parche,       true },
	{ "fallo_main",         phase_fallo_main,         true },
	{ "fallo_tiempo",       phase_fallo_tiempo,       true },
	{ "fallo_solo_lectura", phase_fallo_solo_lectura, true },
};
#define NUM_PHASES ((int)(sizeof phases / sizeof phases[0]))

static struct {
	int st;
	char phase[24];
	char line[400];
} states[NUM_PHASES];

static int phase_index(const char *name)
{
	for (int i = 0; i < NUM_PHASES; i++)
		if (strcmp(phases[i].name, name) == 0)
			return i;
	return -1;
}

/* Sin búfer (open/write/close): el proceso puede morir enseguida. */
static void state_append(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void state_append(const char *fmt, ...)
{
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof line - 2)
		n = (int)sizeof line - 2;
	line[n++] = '\n';
	int fd = open(FM_STATE, O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (fd < 0) {
		salsa64_log("no se pudo abrir %s: el progreso no se guardará", FM_STATE);
		return;
	}
	write(fd, line, (size_t)n);
	close(fd);
}

/* Un paso dentro de una fase (lo anota también el ayudante). */
static void step(const char *phase, const char *what)
{
	state_append("%s PASO %s", phase, what);
}

static void state_load(void)
{
	FILE *f = fopen(FM_STATE, "r");
	if (!f)
		return;
	char line[sizeof states[0].line];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char name[24], status[16];
		int off = 0;
		if (sscanf(line, "%23s %15s %n", name, status, &off) < 2)
			continue;
		int i = phase_index(name);
		if (i < 0)
			continue;
		if (strcmp(status, "PASO") == 0) {
			if (states[i].st == ST_TRYING)  /* hasta dónde llegó */
				snprintf(states[i].phase, sizeof states[i].phase, "%s", line + off);
		} else if (strcmp(status, "PROBANDO") == 0) {
			states[i].st = ST_TRYING;
			snprintf(states[i].phase, sizeof states[i].phase, "%s", line + off);
		} else {
			states[i].st = ST_DONE;
			snprintf(states[i].line, sizeof states[i].line, "%s", line);
		}
	}
	fclose(f);
}

static void finish(int i, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish(int i, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(states[i].line, sizeof states[i].line, fmt, ap);
	va_end(ap);
	states[i].st = ST_DONE;
	state_append("%s", states[i].line);
	salsa64_log("%s", states[i].line);
}

static void trying(int i, const char *what)
{
	state_append("%s PROBANDO %s", phases[i].name, what);
}

/* --- Región y fallos -------------------------------------------------------------- */

/* Región de 256 MB en páginas de 64 KB, reservada al arrancar; las fases
 * mapean dentro a estos desplazamientos. */
#define OFF_MAP     0x00100000u /* mapeo: objeto de 64 KB */
#define OFF_MAP_1M  0x00200000u /* mapeo: objeto de 1 MB */
#define OFF_MIRROR  0x00400000u /* mapeo: espejo */
#define OFF_RIGHTS  0x00600000u /* mapeo: permisos */
#define OFF_DATA    0x01000000u /* datos: 8 MB en 128 objetos de 64 KB */
#define OFF_FAULT   0x02000000u /* fallos: una página de 64 KB por prueba */

static sys_mem_addr_t region;    /* 64 KB por página */
static sys_mem_addr_t region_1m; /* 1 MB por página (mapeo y datos); 0 si no se pudo */
static sys_ppu_thread_t main_tid;

enum { FM_NONE, FM_MAP_READ, FM_MAP_WRITE, FM_RIGHTS, FM_PATCH };

static sys_mem_addr_t fault_region;  /* donde está la notificación (la región de la prueba u otra) */

static struct {
	bool enabled;             /* 327 aceptó la región */
	bool want_ctx;            /* pedir el contexto (58) al atender el fallo */
	const char *phase;        /* fase en curso, para los PASO del ayudante */
	bool broken;              /* una prueba no volvió: no insistir */
	sys_event_queue_t queue;
	sys_ppu_thread_t helper;
	volatile int mode;
	volatile int stop;
	volatile int events;      /* eventos atendidos */
	sys_mem_id_t spare;       /* objeto de 64 KB listo para mapear en el fallo */
	volatile u32 *patch_at;   /* FM_PATCH: palabra a reemplazar */
	u32 patch_word;
	volatile u32 victim_tid;  /* por si el evento no trae el id del hilo */
	u64 t_event, t_done;      /* timebase al recibir el evento y al reanudar */
	sys_event_t ev;
	s32 ctx_rc, act_rc, recover_rc;
	u32 ev_tid;
	u64 ctx[512] __attribute__((aligned(128)));
} fault;

static u32 fault_page(unsigned k)
{
	return fault_region + OFF_FAULT + k * 0x10000u;
}

static void helper_thread(void *arg)
{
	(void)arg;
	while (!fault.stop) {
		sys_event_t ev;
		if (sysEventQueueReceive(fault.queue, &ev, 200000) != 0)
			continue;
		fault.t_event = __gettime();
		fault.ev = ev;
		/* En la documentación de RPCS3, data_2 = tipo << 32 | id del hilo y
		 * data_1 = dirección; si el id no sirve, se usa el de la víctima. */
		u32 tid = (u32)ev.data_2;
		u32 addr = (u32)ev.data_1;
		fault.ev_tid = tid;
		const char *ph = fault.phase ? fault.phase : "?";
		step(ph, "evento recibido");
		memset((void *)fault.ctx, 0, sizeof fault.ctx);
		fault.ctx_rc = -1;
		if (fault.want_ctx) {
			step(ph, "pidiendo contexto");
			fault.ctx_rc = sc_page_fault_context(tid, (void *)fault.ctx);
			if (fault.ctx_rc != 0 && tid != fault.victim_tid)
				fault.ctx_rc = sc_page_fault_context(fault.victim_tid, (void *)fault.ctx);
			step(ph, "contexto pedido");
		}
		switch (fault.mode) {
		case FM_MAP_READ:
			fault.act_rc = sysMMapperMapMemory(addr & ~0xFFFFu, fault.spare, SYS_MEMORY_PROT_READ_WRITE);
			if (fault.act_rc == 0)
				*(volatile u32 *)(uintptr_t)addr = FM_PATTERN;
			break;
		case FM_MAP_WRITE:
			fault.act_rc = sysMMapperMapMemory(addr & ~0xFFFFu, fault.spare, SYS_MEMORY_PROT_READ_WRITE);
			break;
		case FM_RIGHTS:
			fault.act_rc = sc_change_access_right(addr & ~0xFFFFu, SYS_MEMORY_PROT_READ_WRITE);
			break;
		case FM_PATCH:
			*fault.patch_at = fault.patch_word;
			flush_code((const void *)fault.patch_at, sizeof(u32));
			fault.act_rc = 0;
			break;
		default:
			fault.act_rc = -1;
			break;
		}
		step(ph, "acción hecha, reanudando");
		fault.recover_rc = sysThreadRecoverPageFault(tid);
		if (fault.recover_rc != 0 && tid != fault.victim_tid)
			fault.recover_rc = sysThreadRecoverPageFault(fault.victim_tid);
		fault.t_done = __gettime();
		fault.events++;
		step(ph, "reanudado");
	}
	sysThreadExit(0);
}

/* Vuelca lo que trajo el contexto y busca en él valores conocidos. */
static void dump_context(u32 addr, const volatile u32 *code)
{
	char line[200];
	int n = 0, pos = 0, at_addr = -1, at_pc = -1, at_magic = -1, at_pc4 = -1;
	for (int k = 0; k < 512; k++) {
		u64 v = fault.ctx[k];
		if (v == 0)
			continue;
		if (v == addr || v == (u64)(0xFFFFFFFF00000000ull | addr))
			at_addr = k;
		if (code && v == (u64)(uintptr_t)code)
			at_pc = k;
		if (code && v == (u64)(uintptr_t)code + 4)
			at_pc4 = k;
		if (v == FM_MAGIC)
			at_magic = k;
		if (n < 48) {
			pos += snprintf(line + pos, sizeof line - (size_t)pos, " [%d]=%016llx", k, (unsigned long long)v);
			if (++n % 4 == 0) {
				salsa64_log("contexto:%s", line);
				pos = 0;
			}
		}
	}
	if (pos)
		salsa64_log("contexto:%s", line);
	salsa64_log("contexto: rc=0x%08x, %d palabras distintas de 0; dirección del fallo en [%d], pc de la carga en [%d] (pc+4 en [%d]), r5 marcado en [%d]",
		   (unsigned)fault.ctx_rc, n, at_addr, at_pc, at_pc4, at_magic);
}

static void log_event(void)
{
	salsa64_log("evento: source=%016llx data1=%016llx data2=%016llx data3=%016llx (hilo del evento 0x%x, víctima 0x%x)",
		   (unsigned long long)fault.ev.source, (unsigned long long)fault.ev.data_1,
		   (unsigned long long)fault.ev.data_2, (unsigned long long)fault.ev.data_3, fault.ev_tid, fault.victim_tid);
}

/* La víctima: un hilo que hace un acceso y anota cuánto tardó. */
typedef struct {
	u32 addr;
	u32 value;      /* valor a escribir, o marca para el código generado */
	int kind;       /* 0 carga, 1 almacenamiento, 2 código generado */
	volatile u32 *code;
	u64 t0, t1;
	u64 result;
	volatile int done;
} victim_t;

static void victim_do(victim_t *v)
{
	sys_ppu_thread_t tid;
	sysThreadGetId(&tid);
	fault.victim_tid = (u32)tid;
	v->done = 0;
	v->t0 = __gettime();
	if (v->kind == 0) {
		v->result = *(volatile u32 *)(uintptr_t)v->addr;
	} else if (v->kind == 1) {
		*(volatile u32 *)(uintptr_t)v->addr = v->value;
		v->result = *(volatile u32 *)(uintptr_t)v->addr;
	} else {
		v->result = call_code(v->code, v->addr, v->value, FM_MAGIC);
	}
	v->t1 = __gettime();
	v->done = 1;
}

static void victim_thread(void *arg)
{
	victim_do(arg);
	sysThreadExit(0);
}

/* Lanza la víctima en un hilo y espera hasta 5 s. Devuelve false si no volvió. */
static bool victim_run(victim_t *v, int mode)
{
	fault.events = 0;
	fault.mode = mode;
	fault.t_event = fault.t_done = 0;
	fault.ev_tid = 0;
	fault.ctx_rc = fault.act_rc = fault.recover_rc = -1;
	memset(&fault.ev, 0, sizeof fault.ev);
	__asm__ volatile("" : : : "memory");
	sys_ppu_thread_t tid;
	if (sysThreadCreate(&tid, victim_thread, v, 1001, 0x10000, THREAD_JOINABLE, (char *)"fm_victima") != 0) {
		salsa64_log("no se pudo crear el hilo víctima");
		return false;
	}
	u64 t0 = __gettime();
	while (!v->done && tb_us(__gettime() - t0) < 5e6)
		sysUsleep(1000);
	if (!v->done) {
		fault.broken = true;
		salsa64_log("la víctima no volvió en 5 s (eventos atendidos: %d)", fault.events);
		return false;
	}
	u64 ret;
	sysThreadJoin(tid, &ret);
	fault.mode = FM_NONE;
	return true;
}

static bool fault_setup(void)
{
	sys_event_queue_attr_t attr;
	memset(&attr, 0, sizeof attr);
	attr.attr_protocol = SYS_EVENT_QUEUE_FIFO;
	attr.type = SYS_EVENT_QUEUE_PPU;
	memcpy(attr.name, "fmfault", 8);
	s32 rc = sysEventQueueCreate(&fault.queue, &attr, SYS_EVENT_QUEUE_KEY_LOCAL, 32);
	if (rc != 0) {
		salsa64_log("sysEventQueueCreate: 0x%08x", (unsigned)rc);
		return false;
	}
	fault_region = region;
	rc = sc_enable_page_fault_notification(region, (u32)fault.queue);
	salsa64_log("sys_mmapper_enable_page_fault_notification(0x%08x, cola 0x%x): 0x%08x", region, (unsigned)fault.queue, (unsigned)rc);
	if (rc != 0) {
		/* En la primera corrida (2026-10-13) la tercera vez dio 0x80010002
		 * con el mismo rango de siempre: ¿se quedó registrado el de un
		 * proceso muerto? Probar con otra región. */
		sys_mem_addr_t other = 0;
		if (sysMMapperAllocateAddress(0x10000000, SYS_MEMORY_PAGE_SIZE_64K, 0x10000000, &other) == 0) {
			rc = sc_enable_page_fault_notification(other, (u32)fault.queue);
			salsa64_log("sys_mmapper_enable_page_fault_notification(0x%08x, otra región): 0x%08x", other, (unsigned)rc);
			if (rc == 0)
				fault_region = other;
		}
	}
	if (rc != 0)
		return false;
	rc = sysMMapperAllocateMemory(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K, &fault.spare);
	if (rc != 0) {
		salsa64_log("sysMMapperAllocateMemory (repuesto de 64 KB): 0x%08x", (unsigned)rc);
		return false;
	}
	if (sysThreadCreate(&fault.helper, helper_thread, NULL, 500, 0x10000, THREAD_JOINABLE, (char *)"fm_ayudante") != 0) {
		salsa64_log("no se pudo crear el hilo ayudante");
		return false;
	}
	return true;
}

static void fault_teardown(void)
{
	if (!fault.enabled)
		return;
	fault.stop = 1;
	u64 ret;
	sysThreadJoin(fault.helper, &ret);
	sysEventQueueDestroy(fault.queue, 0);
}

/* Lo común de las fases de fallo: la línea de resultado. */
static void fault_report(int i, const victim_t *v, bool ok, const char *extra)
{
	if (fault.events == 0) {
		finish(i, "%s FALLO %s; %.1f µs en la víctima y ningún evento: el acceso no falló o lv2 no avisó",
		       phases[i].name, extra, tb_us(v->t1 - v->t0));
		return;
	}
	finish(i, "%s %s %s; %.1f µs en la víctima (evento a los %.1f, reanudado a los %.1f); evento %d, contexto 0x%08x, acción 0x%08x, reanudar 0x%08x; data1=%08llx data2=%016llx data3=%llx",
	       phases[i].name, ok ? "OK" : "FALLO", extra, tb_us(v->t1 - v->t0), tb_us(fault.t_event - v->t0),
	       tb_us(fault.t_done - v->t0), fault.events, (unsigned)fault.ctx_rc, (unsigned)fault.act_rc,
	       (unsigned)fault.recover_rc, (unsigned long long)fault.ev.data_1, (unsigned long long)fault.ev.data_2,
	       (unsigned long long)fault.ev.data_3);
}

/* --- Fases ----------------------------------------------------------------------- */

static void log_user_memory(const char *when)
{
	struct {
		u32 total;
		u32 available;
	} um = { 0, 0 };
	if (sc_user_memory(&um) == 0)
		salsa64_log("memoria de usuario %s: total %u KB, disponible %u KB", when, um.total >> 10, um.available >> 10);
}

static void log_page_attribute(const char *what, u32 addr)
{
	u64 attr[4] = { 0, 0, 0, 0 };
	s32 rc = sc_page_attribute(addr, attr);
	salsa64_log("atributos de página de %s (0x%08x): rc=0x%08x %016llx %016llx %016llx %016llx", what, addr, (unsigned)rc,
		   (unsigned long long)attr[0], (unsigned long long)attr[1], (unsigned long long)attr[2], (unsigned long long)attr[3]);
}

static void phase_reservas(int i)
{
	static const struct {
		const char *name;
		size_t size, align;
		u64 flags;
	} tries[] = {
		{ "256M",        0x10000000, 0x10000000, SYS_MEMORY_PAGE_SIZE_64K },
		{ "512M",        0x20000000, 0x20000000, SYS_MEMORY_PAGE_SIZE_64K },
		{ "512M/al256M", 0x20000000, 0x10000000, SYS_MEMORY_PAGE_SIZE_64K },
		{ "1G",          0x40000000, 0x40000000, SYS_MEMORY_PAGE_SIZE_64K },
		{ "1G/al256M",   0x40000000, 0x10000000, SYS_MEMORY_PAGE_SIZE_64K },
		{ "2G",          0x80000000, 0x80000000, SYS_MEMORY_PAGE_SIZE_64K },
		{ "256M/1M",     0x10000000, 0x10000000, SYS_MEMORY_PAGE_SIZE_1M },
		{ "512M/1M",     0x20000000, 0x20000000, SYS_MEMORY_PAGE_SIZE_1M },
		{ "512M/1M/der", 0x20000000, 0x20000000, SYS_MEMORY_PAGE_SIZE_1M | SYS_MEMORY_ACCESS_RIGHT_ANY },
		{ "256M/al0",    0x10000000, 0,          SYS_MEMORY_PAGE_SIZE_64K },
	};
	char out[400] = "";
	int pos = 0;
	trying(i, "reservar");
	log_user_memory("antes de reservar");
	for (unsigned k = 0; k < sizeof tries / sizeof tries[0]; k++) {
		sys_mem_addr_t addr = 0;
		s32 rc = sysMMapperAllocateAddress(tries[k].size, tries[k].flags, tries[k].align, &addr);
		if (rc == 0) {
			pos += snprintf(out + pos, sizeof out - (size_t)pos, " %s@%08x", tries[k].name, addr);
			s32 rf = sysMMapperFreeAddress(addr);
			if (rf != 0)
				pos += snprintf(out + pos, sizeof out - (size_t)pos, "(liberar 0x%08x)", (unsigned)rf);
		} else {
			pos += snprintf(out + pos, sizeof out - (size_t)pos, " %s:0x%08x", tries[k].name, (unsigned)rc);
		}
	}
	salsa64_log("reservas:%s", out);

	/* Cuántas de 256 MB a la vez (más la de la prueba, ya reservada). */
	sys_mem_addr_t addrs[16];
	int n = 0;
	char list[200] = "";
	int lp = 0;
	while (n < 16 && sysMMapperAllocateAddress(0x10000000, SYS_MEMORY_PAGE_SIZE_64K, 0x10000000, &addrs[n]) == 0) {
		lp += snprintf(list + lp, sizeof list - (size_t)lp, " %08x", addrs[n]);
		n++;
	}
	for (int k = 0; k < n; k++)
		sysMMapperFreeAddress(addrs[k]);
	salsa64_log("regiones de 256 MB a la vez: %d más la de la prueba (0x%08x):%s", n, region, list);

	/* Región fija (326): en RPCS3, 256 MB en 0xB0000000. */
	s32 rc = sc_allocate_fixed_address();
	salsa64_log("sys_mmapper_allocate_fixed_address: 0x%08x", (unsigned)rc);
	log_page_attribute("0xB0000000", 0xB0000000u);
	if (rc == 0) {
		s32 rf = sysMMapperFreeAddress(0xB0000000u);
		salsa64_log("liberar 0xB0000000: 0x%08x", (unsigned)rf);
	}
	log_page_attribute("la región de la prueba (sin mapear)", region);
	log_page_attribute("el código", (u32)(uintptr_t)salsa64_jit_nobits);
	log_user_memory("después de reservar");
	finish(i, "reservas OK %d regiones de 256 MB;%s", n + 1, out);
}

/* Objetos de 64 KB mapeados en OFF_DATA (datos los recorre). */
static sys_mem_id_t data_ids[128];
static int data_n;
static sys_mem_id_t data_1m_ids[8];
static int data_1m_n;

static void phase_mapeo(int i)
{
	sys_mem_id_t id64 = 0, id1m = 0;
	s32 rc;
	trying(i, "asignar");
	rc = sysMMapperAllocateMemory(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K, &id64);
	s32 rc1 = sysMMapperAllocateMemory(MB, SYS_MEMORY_PAGE_SIZE_1M, &id1m);
	salsa64_log("asignar 64 KB: 0x%08x (id 0x%x); 1 MB: 0x%08x (id 0x%x)", (unsigned)rc, id64, (unsigned)rc1, id1m);
	if (rc != 0) {
		finish(i, "mapeo NO_DISPONIBLE asignar 64 KB: 0x%08x", (unsigned)rc);
		return;
	}
	log_user_memory("con 64 KB + 1 MB asignados");

	/* Mapear, escribir, leer, atributos. */
	trying(i, "mapear 64K");
	u32 a64 = region + OFF_MAP;
	rc = sysMMapperMapMemory(a64, id64, SYS_MEMORY_PROT_READ_WRITE);
	salsa64_log("mapear 64 KB en 0x%08x: 0x%08x", a64, (unsigned)rc);
	if (rc != 0) {
		finish(i, "mapeo FALLO mapear 64 KB: 0x%08x", (unsigned)rc);
		return;
	}
	volatile u32 *p = (volatile u32 *)(uintptr_t)a64;
	p[0] = FM_PATTERN;
	bool rw_ok = p[0] == FM_PATTERN;
	log_page_attribute("64 KB mapeados", a64);

	/* Tiempos: desmapear y mapear de nuevo, 50 veces. */
	u64 t_unmap = 0, t_map = 0;
	for (int k = 0; k < 50; k++) {
		sys_mem_id_t out;
		u64 t0 = __gettime();
		sysMMapperUnmapMemory(a64, &out);
		u64 t1 = __gettime();
		sysMMapperMapMemory(a64, id64, SYS_MEMORY_PROT_READ_WRITE);
		u64 t2 = __gettime();
		t_unmap += t1 - t0;
		t_map += t2 - t1;
	}
	bool kept = p[0] == FM_PATTERN; /* ¿el contenido sobrevive al desmapeo? */
	salsa64_log("64 KB: desmapear %.1f µs, mapear %.1f µs (media de 50); contenido tras remapear: %s", tb_us(t_unmap) / 50,
		   tb_us(t_map) / 50, kept ? "se conserva" : "no se conserva");

	/* 1 MB en la región de 64 KB y en una de 1 MB. */
	u64 t_unmap1 = 0, t_map1 = 0;
	char m1[160] = "";
	if (rc1 == 0) {
		trying(i, "mapear 1M");
		u32 a1 = region + OFF_MAP_1M;
		rc = sysMMapperMapMemory(a1, id1m, SYS_MEMORY_PROT_READ_WRITE);
		snprintf(m1, sizeof m1, "1 MB en región de 64 KB: 0x%08x", (unsigned)rc);
		if (rc == 0) {
			volatile u32 *q = (volatile u32 *)(uintptr_t)a1;
			/* Primer toque de cada página de 4 KB, y segundo. */
			u64 t0 = __gettime();
			for (unsigned k = 0; k < MB / 4096; k++)
				q[k * 1024] = k;
			u64 t1 = __gettime();
			for (unsigned k = 0; k < MB / 4096; k++)
				q[k * 1024] = k + 1;
			u64 t2 = __gettime();
			salsa64_log("1 MB recién mapeado: primer toque de 256 páginas de 4 KB %.0f ns por página, segundo %.0f",
				   tb_us(t1 - t0) * 1000 / 256, tb_us(t2 - t1) * 1000 / 256);
			for (int k = 0; k < 20; k++) {
				sys_mem_id_t out;
				t0 = __gettime();
				sysMMapperUnmapMemory(a1, &out);
				t1 = __gettime();
				sysMMapperMapMemory(a1, id1m, SYS_MEMORY_PROT_READ_WRITE);
				t2 = __gettime();
				t_unmap1 += t1 - t0;
				t_map1 += t2 - t1;
			}
			salsa64_log("1 MB: desmapear %.1f µs, mapear %.1f µs (media de 20)", tb_us(t_unmap1) / 20, tb_us(t_map1) / 20);
			sys_mem_id_t out;
			sysMMapperUnmapMemory(a1, &out);
		}
		if (region_1m) {
			rc = sysMMapperMapMemory(region_1m, id1m, SYS_MEMORY_PROT_READ_WRITE);
			size_t l = strlen(m1);
			snprintf(m1 + l, sizeof m1 - l, "; en región de 1 MB: 0x%08x", (unsigned)rc);
			if (rc == 0) {
				sys_mem_id_t out;
				sysMMapperUnmapMemory(region_1m, &out);
			}
			/* Un objeto de 64 KB en una región de 1 MB (se espera que no). */
			rc = sysMMapperMapMemory(region_1m, id64, SYS_MEMORY_PROT_READ_WRITE);
			l = strlen(m1);
			snprintf(m1 + l, sizeof m1 - l, "; 64 KB en región de 1 MB: 0x%08x", (unsigned)rc);
			if (rc == 0) {
				sys_mem_id_t out;
				sysMMapperUnmapMemory(region_1m, &out);
			}
		}
		salsa64_log("%s", m1);
	}

	/* Espejo: el mismo objeto en dos direcciones (id64 sigue mapeado en a64). */
	trying(i, "espejo");
	u32 am = region + OFF_MIRROR;
	rc = sysMMapperMapMemory(am, id64, SYS_MEMORY_PROT_READ_WRITE);
	char mirror[120];
	if (rc == 0) {
		volatile u32 *q = (volatile u32 *)(uintptr_t)am;
		p[1] = 0x11223344;
		bool same = q[1] == 0x11223344;
		sys_mem_id_t out;
		s32 ru = sysMMapperUnmapMemory(am, &out);
		snprintf(mirror, sizeof mirror, "espejo OK (%s; desmapear 0x%08x, id 0x%x)", same ? "misma memoria" : "OTRA memoria",
			 (unsigned)ru, out);
	} else {
		snprintf(mirror, sizeof mirror, "espejo NO: 0x%08x", (unsigned)rc);
	}
	salsa64_log("%s", mirror);

	/* Buscar sitio. */
	trying(i, "buscar");
	sys_mem_id_t id64b = 0;
	char search[120] = "search_and_map: sin objeto";
	if (sysMMapperAllocateMemory(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K, &id64b) == 0) {
		sys_mem_addr_t found = 0;
		rc = sysMMapperSearchAndMap(region, id64b, SYS_MEMORY_PROT_READ_WRITE, &found);
		snprintf(search, sizeof search, "search_and_map desde 0x%08x: 0x%08x en 0x%08x", region, (unsigned)rc, found);
		if (rc == 0) {
			sys_mem_id_t out;
			sysMMapperUnmapMemory(found, &out);
		}
		sysMMapperFreeMemory(id64b);
	}
	salsa64_log("%s", search);

	/* Permisos: solo lectura y de vuelta (sin escribir mientras tanto). */
	trying(i, "permisos");
	rc = sc_change_access_right(a64, SYS_MEMORY_PROT_READ_ONLY);
	u32 seen = p[0];
	s32 rc2 = sc_change_access_right(a64, SYS_MEMORY_PROT_READ_WRITE);
	log_page_attribute("64 KB tras volver a RW", a64);
	salsa64_log("permisos: a solo lectura 0x%08x (se lee %08x), a RW 0x%08x", (unsigned)rc, seen, (unsigned)rc2);

	/* Cuántos objetos de 64 KB (hasta 256) y mapear 128 seguidos para datos. */
	trying(i, "objetos");
	int n = 0;
	sys_mem_id_t ids[256];
	while (n < 256 && sysMMapperAllocateMemory(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K, &ids[n]) == 0)
		n++;
	u64 t0 = __gettime();
	int mapped = 0;
	for (int k = 0; k < n && k < 128; k++) {
		if (sysMMapperMapMemory(region + OFF_DATA + (u32)k * 0x10000u, ids[k], SYS_MEMORY_PROT_READ_WRITE) != 0)
			break;
		mapped++;
	}
	u64 t1 = __gettime();
	data_n = mapped;
	for (int k = 0; k < mapped; k++)
		data_ids[k] = ids[k];
	for (int k = mapped; k < n; k++)
		sysMMapperFreeMemory(ids[k]);
	salsa64_log("objetos de 64 KB: %d asignados (tope 256); %d mapeados seguidos en %.0f µs (%.1f por objeto)", n, mapped,
		   tb_us(t1 - t0), mapped ? tb_us(t1 - t0) / mapped : 0.0);
	log_user_memory("con 8 MB mapeados");
	if (region_1m) {
		int m = 0;
		while (m < 8 && sysMMapperAllocateMemory(MB, SYS_MEMORY_PAGE_SIZE_1M, &data_1m_ids[m]) == 0 &&
		       sysMMapperMapMemory(region_1m + (u32)m * MB, data_1m_ids[m], SYS_MEMORY_PROT_READ_WRITE) == 0)
			m++;
		data_1m_n = m;
		salsa64_log("objetos de 1 MB mapeados seguidos en la región de 1 MB: %d", m);
	}

	sys_mem_id_t out;
	sysMMapperUnmapMemory(a64, &out);
	sysMMapperFreeMemory(id64);
	if (rc1 == 0)
		sysMMapperFreeMemory(id1m);
	finish(i, "mapeo %s rw=%s; 64K desmapear %.1f µs mapear %.1f µs; 1M desmapear %.1f mapear %.1f; %s; %s; %d objetos", rw_ok ? "OK" : "FALLO",
	       rw_ok ? "sí" : "no", tb_us(t_unmap) / 50, tb_us(t_map) / 50, tb_us(t_unmap1) / 20, tb_us(t_map1) / 20, mirror, m1, n);
}

/* ns por lectura de 32 bits recorriendo `len` bytes a paso `stride`, 8 pasadas. */
static double read_loop(const volatile u8 *base, size_t len, size_t stride)
{
	u32 sum = 0;
	size_t n = 0;
	u64 t0 = __gettime();
	for (int pass = 0; pass < 8; pass++)
		for (size_t off = 0; off + 4 <= len; off += stride) {
			sum += *(const volatile u32 *)(base + off);
			n++;
		}
	u64 t1 = __gettime();
	__asm__ volatile("" : : "r"(sum));
	return tb_us(t1 - t0) * 1000.0 / (double)n;
}

static void phase_datos(int i)
{
	trying(i, "recorrer");
	void *heap = memalign(MB, 8 * MB);
	char out[300] = "";
	int pos = 0;
	if (heap) {
		memset(heap, 1, 8 * MB);
		pos += snprintf(out + pos, sizeof out - (size_t)pos, " heap %.1f/%.1f", read_loop(heap, 8 * MB, 128), read_loop(heap, 8 * MB, 4224));
		free(heap);
	}
	if (data_n == 128) {
		volatile u8 *b = (volatile u8 *)(uintptr_t)(region + OFF_DATA);
		for (size_t k = 0; k < 8 * MB; k += 4)
			*(volatile u32 *)(b + k) = 1;
		pos += snprintf(out + pos, sizeof out - (size_t)pos, " 64K %.1f/%.1f", read_loop(b, 8 * MB, 128), read_loop(b, 8 * MB, 4224));
	}
	if (data_1m_n == 8) {
		volatile u8 *b = (volatile u8 *)(uintptr_t)region_1m;
		for (size_t k = 0; k < 8 * MB; k += 4)
			*(volatile u32 *)(b + k) = 1;
		pos += snprintf(out + pos, sizeof out - (size_t)pos, " 1M %.1f/%.1f", read_loop(b, 8 * MB, 128), read_loop(b, 8 * MB, 4224));
	}
	/* Lo mismo dentro de la L2: 256 KB. */
	if (data_n >= 4) {
		volatile u8 *b = (volatile u8 *)(uintptr_t)(region + OFF_DATA);
		pos += snprintf(out + pos, sizeof out - (size_t)pos, "; 256 KB en 64K %.1f/%.1f", read_loop(b, 256 * 1024, 128), read_loop(b, 256 * 1024, 4224));
	}
	finish(i, "datos OK ns por lectura (8 MB, paso 128 / paso 4224):%s", out);
}

static void fallo_read(int i, bool ctx, unsigned page)
{
	victim_t v = { .addr = fault_page(page) + 0x100, .kind = 0 };
	fault.phase = phases[i].name;
	fault.want_ctx = ctx;
	trying(i, ctx ? "leer sin mapear, con contexto" : "leer sin mapear");
	if (!victim_run(&v, FM_MAP_READ)) {
		finish(i, "%s FALLO la víctima no volvió (eventos %d)", phases[i].name, fault.events);
		return;
	}
	log_event();
	if (ctx)
		dump_context(v.addr, NULL);
	char extra[80];
	snprintf(extra, sizeof extra, "leyó %08llx (%s)", (unsigned long long)v.result,
		 v.result == FM_PATTERN ? "el valor que puso el ayudante: la carga se repitió" : "OTRO valor");
	fault_report(i, &v, v.result == FM_PATTERN && fault.events == 1, extra);
	/* La página queda mapeada: otro repuesto para la siguiente. */
	sysMMapperAllocateMemory(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K, &fault.spare);
}

static void phase_fallo_evento(int i)
{
	fallo_read(i, false, 0);
}

static void phase_fallo_contexto(int i)
{
	fallo_read(i, true, 1);
}

static void phase_fallo_escritura(int i)
{
	victim_t v = { .addr = fault_page(2) + 0x200, .value = 0xC0FFEE00u, .kind = 1 };
	fault.phase = phases[i].name;
	fault.want_ctx = false;
	trying(i, "escribir sin mapear");
	if (!victim_run(&v, FM_MAP_WRITE)) {
		finish(i, "fallo_escritura FALLO la víctima no volvió (eventos %d)", fault.events);
		return;
	}
	log_event();
	char extra[80];
	snprintf(extra, sizeof extra, "releyó %08llx (%s)", (unsigned long long)v.result,
		 v.result == v.value ? "lo escrito: el almacenamiento se repitió" : "OTRO valor");
	fault_report(i, &v, v.result == v.value && fault.events == 1, extra);
	sysMMapperAllocateMemory(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K, &fault.spare);
}

static void phase_fallo_solo_lectura(int i)
{
	u32 page = fault_page(15);
	fault.phase = phases[i].name;
	fault.want_ctx = false;
	trying(i, "preparar solo lectura");
	s32 rc = sysMMapperMapMemory(page, fault.spare, SYS_MEMORY_PROT_READ_WRITE);
	if (rc != 0) {
		finish(i, "fallo_solo_lectura NO_DISPONIBLE mapear: 0x%08x", (unsigned)rc);
		return;
	}
	*(volatile u32 *)(uintptr_t)(page + 0x300) = 0;
	rc = sc_change_access_right(page, SYS_MEMORY_PROT_READ_ONLY);
	if (rc != 0) {
		finish(i, "fallo_solo_lectura NO_DISPONIBLE cambiar a solo lectura: 0x%08x", (unsigned)rc);
		return;
	}
	victim_t v = { .addr = page + 0x300, .value = 0xBEEF0001u, .kind = 1 };
	trying(i, "escribir en solo lectura");
	if (!victim_run(&v, FM_RIGHTS)) {
		finish(i, "fallo_solo_lectura FALLO la víctima no volvió (eventos %d)", fault.events);
		return;
	}
	log_event();
	char extra[80];
	snprintf(extra, sizeof extra, "releyó %08llx (%s)", (unsigned long long)v.result,
		 v.result == v.value ? "lo escrito tras dar permiso" : "OTRO valor");
	fault_report(i, &v, v.result == v.value && fault.events == 1, extra);
	sysMMapperAllocateMemory(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K, &fault.spare);
}

static void fallo_patch(int i, bool from_main, unsigned fn, u32 page)
{
	/* Escribir la cola ya es un paso arriesgado (en RPCS3 el segmento de
	 * texto es de solo lectura): anotarlo antes. */
	fault.phase = phases[i].name;
	fault.want_ctx = true;
	trying(i, from_main ? "código generado en el hilo principal" : "código generado");
	victim_t v = { .addr = page + 0x400, .value = 0, .kind = 2, .code = cave_fn(fn, false) };
	fault.patch_at = v.code;
	fault.patch_word = PPC_LI_R3(FM_MARK);
	bool back;
	if (from_main) {
		fault.events = 0;
		fault.mode = FM_PATCH;
		fault.t_event = fault.t_done = 0;
		fault.ev_tid = 0;
		fault.ctx_rc = fault.act_rc = fault.recover_rc = -1;
		memset(&fault.ev, 0, sizeof fault.ev);
		__asm__ volatile("" : : : "memory");
		victim_do(&v);
		fault.mode = FM_NONE;
		back = true;
	} else {
		back = victim_run(&v, FM_PATCH);
	}
	if (!back) {
		finish(i, "%s FALLO la víctima no volvió (eventos %d)", phases[i].name, fault.events);
		return;
	}
	log_event();
	dump_context(v.addr, v.code);
	char extra[80];
	snprintf(extra, sizeof extra, "devolvió %04llx (%s)", (unsigned long long)v.result,
		 v.result == FM_MARK ? "la instrucción parchada: siguió por el código nuevo" : "OTRO valor");
	fault_report(i, &v, v.result == FM_MARK && fault.events == 1, extra);
}

static void phase_fallo_parche(int i)
{
	fallo_patch(i, false, 0, fault_page(3));
}

static void phase_fallo_main(int i)
{
	fallo_patch(i, true, 1, fault_page(4));
}

static void phase_fallo_tiempo(int i)
{
	fault.phase = phases[i].name;
	fault.want_ctx = true;
	trying(i, "diez fallos parchados");
	double total = 0, to_event = 0, to_done = 0;
	int ok = 0;
	for (unsigned k = 0; k < 10; k++) {
		victim_t v = { .addr = fault_page(5 + k) + 0x500, .value = 0, .kind = 2, .code = NULL };
		v.code = cave_fn(2 + k, false);
		fault.patch_at = v.code;
		fault.patch_word = PPC_LI_R3(FM_MARK);
		if (!victim_run(&v, FM_PATCH)) {
			finish(i, "fallo_tiempo FALLO la víctima %u no volvió", k);
			return;
		}
		if (v.result == FM_MARK && fault.events == 1)
			ok++;
		total += tb_us(v.t1 - v.t0);
		to_event += tb_us(fault.t_event - v.t0);
		to_done += tb_us(fault.t_done - v.t0);
	}
	finish(i, "fallo_tiempo %s %d de 10; por fallo %.1f µs en la víctima (evento a los %.1f, reanudado a los %.1f)",
	       ok == 10 ? "OK" : "FALLO", ok, total / 10, to_event / 10, to_done / 10);
}

/* --- Principal ---------------------------------------------------------------------- */

/* Número de arranque (FM_LAUNCH): cada uno con su propio log. */
static unsigned launch_number(void)
{
	unsigned n = 0;
	FILE *f = fopen(FM_LAUNCH, "r");
	if (f) {
		if (fscanf(f, "%u", &n) != 1)
			n = 0;
		fclose(f);
	}
	n++;
	f = fopen(FM_LAUNCH, "w");
	if (f) {
		fprintf(f, "%u\n", n);
		fclose(f);
	}
	return n;
}

int main(void)
{
	mkdir(SALSA64_DATA_DIR, 0777);
	unsigned launch = launch_number();
	char tag[32];
	snprintf(tag, sizeof tag, "fastmem-%u", launch);
	salsa64_log_init(tag);
	salsa64_log("arranque %u (log en %s/%s.log; el estado en %s)", launch, SALSA64_DATA_DIR, tag, FM_STATE);
	tb_freq = sysGetTimebaseFrequency();
	sysThreadGetId(&main_tid);
	salsa64_log("timebase %llu Hz; hilo principal 0x%llx; cola NOBITS en %p", (unsigned long long)tb_freq,
		   (unsigned long long)main_tid, (void *)salsa64_jit_nobits);
	log_user_memory("al arrancar");
	state_load();

	s32 rc = sysMMapperAllocateAddress(0x10000000, SYS_MEMORY_PAGE_SIZE_64K, 0x10000000, &region);
	if (rc != 0) {
		salsa64_log("RESULTADO fastmem: sin región de 256 MB (sysMMapperAllocateAddress 0x%08x): nada que probar", (unsigned)rc);
		salsa64_log_shutdown();
		return 0;
	}
	rc = sysMMapperAllocateAddress(0x10000000, SYS_MEMORY_PAGE_SIZE_1M, 0x10000000, &region_1m);
	if (rc != 0)
		region_1m = 0;
	salsa64_log("región de 64 KB en 0x%08x; región de 1 MB en 0x%08x%s", region, region_1m, region_1m ? "" : " (no se pudo)");

	for (int i = 0; i < NUM_PHASES; i++) {
		if (states[i].st == ST_TRYING) {
			salsa64_log("%s: el intento anterior terminó el proceso en la fase '%s'", phases[i].name, states[i].phase);
			finish(i, "%s CRASH %s", phases[i].name, states[i].phase);
		}
	}

	/* Las fases de fallo solo si hace falta alguna y fallo_evento no mató el proceso. */
	bool want_fault = false;
	for (int i = 0; i < NUM_PHASES; i++)
		if (phases[i].fault && states[i].st == ST_PENDING)
			want_fault = true;
	int fl = phase_index("fallo_evento");
	bool skip_fault = states[fl].st == ST_DONE && strstr(states[fl].line, " CRASH ") != NULL;
	if (want_fault && !skip_fault)
		fault.enabled = fault_setup();

	for (int i = 0; i < NUM_PHASES; i++) {
		if (states[i].st != ST_PENDING)
			continue;
		if (phases[i].fault && skip_fault) {
			finish(i, "%s OMITIDO fallo_evento mató el proceso", phases[i].name);
			continue;
		}
		if (phases[i].fault && !fault.enabled) {
			finish(i, "%s NO_DISPONIBLE sin notificación de fallos", phases[i].name);
			continue;
		}
		if (phases[i].fault && fault.broken) {
			finish(i, "%s OMITIDO una víctima anterior no volvió", phases[i].name);
			continue;
		}
		salsa64_log("== %s", phases[i].name);
		phases[i].run(i);
	}

	int ok = 0;
	salsa64_log("== resumen");
	for (int i = 0; i < NUM_PHASES; i++) {
		salsa64_log("  %s", states[i].line);
		if (strstr(states[i].line, " OK "))
			ok++;
	}
	remove(FM_RESULT);
	if (rename(FM_STATE, FM_RESULT) != 0)
		salsa64_log("no se pudo renombrar %s; borrarlo a mano para repetir la prueba", FM_STATE);
	else
		salsa64_log("resultado guardado en %s", FM_RESULT);
	salsa64_log("RESULTADO fastmem: %d de %d fases OK", ok, NUM_PHASES);
	if (fault.broken)
		salsa64_log("hay un hilo atascado en un fallo de página: salir de la aplicación con el botón PS");
	fault_teardown();
	salsa64_log_shutdown();
	return 0;
}
