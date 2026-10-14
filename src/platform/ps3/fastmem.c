/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — fastmem en la PS3: la ventana, el hilo ayudante y la
 * autoprueba (fastmem.h, docs/fastmem.md).
 *
 * Syscalls sin envoltura en PSL1GHT: 327 sys_mmapper_enable_page_fault_
 * notification(dirección de la región, cola) y 58 sys_ppu_thread_get_page_
 * fault_context(hilo, contexto). El contexto no está documentado: la
 * autoprueba hace fallar una carga cuyo pc conoce y busca ese valor en el
 * volcado; el índice que encuentra es el que usa después para los fallos
 * del juego (si cambia entre firmwares, la autoprueba lo vuelve a hallar).
 *
 * El evento (documentado por RPCS3): data_1 = dirección del fallo, data_2 =
 * tipo << 32 | id del hilo (0 PPU, 1 SPU, 2 raw SPU), data_3 = causa (2 sin
 * mapear, 1 solo lectura). Si el id del evento no sirve para reanudar, se
 * prueba con el del hilo de emulación que registró quien llama.
 */
#include <ppu-asm.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <stdio.h>
#include <string.h>
#include <sys/event_queue.h>
#include <sys/memory.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <lv2/memory.h>
#include <lv2/thread.h>

#include "cpu/rec.h"
#include "platform/ps3/fastmem.h"
#include "platform/ps3/log.h"

#define FM_WINDOW_BYTES 0x20000000u  /* 512 MB: va & 0x1FFFFFFF */
#define FM_HELPER_PRIO  900          /* por encima de emulación (1000) y presentación (1001) */
#define FM_PROBE_MARK   0x5A5A
#define FM_CTX_WORDS    512

#define PPC_LWZ_R3_R3 0x80630000u
#define PPC_LI_R3(v)  (0x38600000u | ((u32)(v) & 0xFFFF))
#define PPC_BLR       0x4E800020u

static struct {
	sys_mem_addr_t region;
	sys_mem_id_t rdram_id;
	void *window;
	bool queue_ok, helper_ok, notify_ok;
	sys_event_queue_t queue;
	sys_ppu_thread_t helper;
	struct salsa64_rec *volatile rec;
	volatile int stop;
	volatile uint32_t faults, unknown, spu_faults;
	int ctx_nip;                 /* índice del pc en el contexto; -1 = no se sabe */
	/* Autoprueba: una carga en `probe` desde un hilo propio; el ayudante la
	 * parchea al verla fallar. */
	volatile uint32_t *probe;
	volatile uint32_t probe_tid;
	volatile int probe_done;
	volatile uint64_t probe_result;
	volatile uint64_t ev_data[3];
	u64 ctx[FM_CTX_WORDS] __attribute__((aligned(128)));
} fm = { .ctx_nip = -1 };

int salsa64_ps3_fastmem_wanted(void)
{
	struct stat st;
	return stat(SALSA64_FASTMEM_FLAG, &st) == 0;
}

static s32 sc_enable_page_fault_notification(u32 start, u32 queue)
{
	lv2syscall2(327, start, queue);
	return_to_user_prog(s32);
}

static s32 sc_page_fault_context(u64 tid, void *ctx)
{
	lv2syscall2(58, tid, (u64)(uintptr_t)ctx);
	return_to_user_prog(s32);
}

static s32 sc_spu_thread_recover_page_fault(u32 id)
{
	lv2syscall1(198, id);
	return_to_user_prog(s32);
}

/* ---- La ventana --------------------------------------------------------------- */

void *salsa64_ps3_fastmem_window(uint32_t bytes)
{
	if (fm.window)
		return fm.window;
	if (bytes == 0 || (bytes & 0xFFFFF) || bytes > 8u << 20) {
		salsa64_log("fastmem: la RDRAM tiene que ser un múltiplo de 1 MB hasta 8 MB");
		return NULL;
	}
	s32 rc = sysMMapperAllocateAddress(FM_WINDOW_BYTES, SYS_MEMORY_PAGE_SIZE_1M | SYS_MEMORY_ACCESS_RIGHT_ANY,
					   FM_WINDOW_BYTES, &fm.region);
	if (rc != 0)
		rc = sysMMapperAllocateAddress(FM_WINDOW_BYTES, SYS_MEMORY_PAGE_SIZE_1M, FM_WINDOW_BYTES, &fm.region);
	if (rc != 0) {
		salsa64_log("fastmem: sin región de 512 MB (sys_mmapper_allocate_address 0x%08x)", (unsigned)rc);
		return NULL;
	}
	rc = sysMMapperAllocateMemory(bytes, SYS_MEMORY_PAGE_SIZE_1M, &fm.rdram_id);
	if (rc != 0) {
		salsa64_log("fastmem: sin memoria para la RDRAM (sys_mmapper_allocate_memory 0x%08x)", (unsigned)rc);
		sysMMapperFreeAddress(fm.region);
		return NULL;
	}
	rc = sysMMapperMapMemory(fm.region, fm.rdram_id, SYS_MEMORY_PROT_READ_WRITE);
	if (rc != 0) {
		salsa64_log("fastmem: no se pudo mapear la RDRAM en la ventana (0x%08x)", (unsigned)rc);
		sysMMapperFreeMemory(fm.rdram_id);
		sysMMapperFreeAddress(fm.region);
		return NULL;
	}
	fm.window = (void *)(uintptr_t)fm.region;
	salsa64_log("fastmem: ventana de 512 MB en 0x%08x con la RDRAM (%u MB) al principio", fm.region, bytes >> 20);
	return fm.window;
}

void *salsa64_ps3_fastmem_ptr(void)
{
	return fm.window;
}

/* ---- El ayudante --------------------------------------------------------------- */

static void flush_code(volatile uint32_t *w)
{
	__asm__ volatile("dcbst 0,%0; sync; icbi 0,%0; isync" : : "r"((uintptr_t)w) : "memory");
}

/* El pc del hilo suspendido: del índice conocido del contexto o, si no se
 * sabe, buscando un valor que apunte a la caché de código y sea un sitio. */
static uintptr_t fault_nip(struct salsa64_rec *rec, const volatile uint32_t *probe)
{
	if (probe) {
		for (int k = 0; k < FM_CTX_WORDS; k++)
			if (fm.ctx[k] == (u64)(uintptr_t)probe) {
				fm.ctx_nip = k;
				return (uintptr_t)probe;
			}
		return 0;
	}
	if (fm.ctx_nip >= 0)
		return (uintptr_t)fm.ctx[fm.ctx_nip];
	if (rec) {
		uintptr_t lo = (uintptr_t)rec->code, hi = lo + (uintptr_t)rec->code_words * 4;
		for (int k = 0; k < FM_CTX_WORDS; k++)
			if (fm.ctx[k] >= lo && fm.ctx[k] < hi)
				return (uintptr_t)fm.ctx[k];
	}
	return 0;
}

static void helper_thread(void *arg)
{
	(void)arg;
	while (!fm.stop) {
		sys_event_t ev;
		if (sysEventQueueReceive(fm.queue, &ev, 100000) != 0)
			continue;
		u32 tid = (u32)ev.data_2, type = (u32)(ev.data_2 >> 32);
		fm.ev_data[0] = ev.data_1;
		fm.ev_data[1] = ev.data_2;
		fm.ev_data[2] = ev.data_3;
		if (type == 1 || type == 2) {
			/* Un SPE con una dirección fuera de la RDRAM: hoy sería un
			 * cuelgue; se registra y se reanuda para que lo cuente el log. */
			fm.spu_faults++;
			sc_spu_thread_recover_page_fault(tid);
			continue;
		}
		memset((void *)fm.ctx, 0, sizeof fm.ctx);
		s32 crc = sc_page_fault_context(tid, (void *)fm.ctx);
		struct salsa64_rec *rec = fm.rec;
		const volatile uint32_t *probe = fm.probe;
		uintptr_t nip = crc == 0 ? fault_nip(rec, probe) : 0;
		bool fixed = false;
		if (probe && nip == (uintptr_t)probe) {
			*fm.probe = PPC_LI_R3(FM_PROBE_MARK);
			flush_code(fm.probe);
			fm.probe = NULL;
			fixed = true;
		} else if (rec && nip && salsa64_rec_fastmem_fault(rec, nip)) {
			fm.faults++;
			fixed = true;
		}
		if (!fixed) {
			/* No es un sitio del recompilador: reanudarlo repetiría el fallo
			 * sin fin. Queda suspendido y el log lo dice; el autor sale de la
			 * aplicación. */
			fm.unknown++;
			salsa64_log("fastmem: fallo de página que no es del código generado: hilo 0x%x, dirección 0x%08llx, "
				    "causa %llu, contexto 0x%08x, pc %p: el hilo queda detenido",
				    tid, (unsigned long long)ev.data_1, (unsigned long long)ev.data_3, (unsigned)crc, (void *)nip);
			continue;
		}
		if (sysThreadRecoverPageFault(tid) != 0 && probe)
			sysThreadRecoverPageFault(fm.probe_tid);
	}
	sysThreadExit(0);
}

/* ---- Autoprueba ---------------------------------------------------------------- */

typedef struct {
	u64 entry, toc, env;
} opd_t;

static void probe_thread(void *arg)
{
	(void)arg;
	sys_ppu_thread_t tid;
	sysThreadGetId(&tid);
	fm.probe_tid = (u32)tid;
	static opd_t opd;
	u64 toc;
	__asm__ volatile("mr %0,2" : "=r"(toc));
	opd.entry = (u64)(uintptr_t)fm.probe;
	opd.toc = toc;
	opd.env = 0;
	void *dp = &opd;
	__asm__ volatile("" : "+r"(dp) : : "memory");  /* que gcc no vea el descriptor como un objeto suyo */
	u64 (*fn)(u64) = (u64 (*)(u64))dp;
	fm.probe_result = fn((u64)(uintptr_t)fm.window + 0x10000000u);  /* 256 MB: sin mapear */
	fm.probe_done = 1;
	sysThreadExit(0);
}

/* Una carga que falla desde un hilo propio, con el código en el final de la
 * caché (ejecutable; el recompilador todavía no la usa). Dice si el evento
 * llega, si el contexto trae el pc y si el hilo sigue por la instrucción
 * parchada. */
static bool self_test(struct salsa64_rec *rec)
{
	volatile uint32_t *code = rec->code + rec->code_words - 32;
	code[0] = PPC_LWZ_R3_R3;
	code[1] = PPC_BLR;
	flush_code(code);
	fm.probe = code;
	fm.probe_done = 0;
	fm.probe_result = 0;
	sys_ppu_thread_t tid;
	if (sysThreadCreate(&tid, probe_thread, NULL, 1001, 0x4000, THREAD_JOINABLE, (char *)"salsa64_fm_prueba") != 0) {
		salsa64_log("fastmem: no se pudo crear el hilo de la autoprueba");
		fm.probe = NULL;
		return false;
	}
	u64 t0 = sysGetSystemTime();
	while (!fm.probe_done && sysGetSystemTime() - t0 < 500000)
		sysUsleep(1000);
	if (!fm.probe_done) {
		salsa64_log("fastmem: la autoprueba no volvió en 0,5 s (evento %s): sin notificación de fallos, fastmem apagado",
			    fm.ev_data[0] ? "recibido" : "no recibido");
		fm.probe = NULL;
		return false;
	}
	u64 ret;
	sysThreadJoin(tid, &ret);
	bool ok = fm.probe_result == FM_PROBE_MARK;
	salsa64_log("fastmem: autoprueba %s: la carga devolvió 0x%llx en %llu µs; evento data1=%08llx data2=%016llx data3=%llx; "
		    "pc en la palabra %d del contexto",
		    ok ? "bien" : "FALLO (el acceso no falló o no se pudo parchear)", (unsigned long long)fm.probe_result,
		    (unsigned long long)(sysGetSystemTime() - t0), (unsigned long long)fm.ev_data[0],
		    (unsigned long long)fm.ev_data[1], (unsigned long long)fm.ev_data[2], fm.ctx_nip);
	fm.probe = NULL;
	return ok && fm.ctx_nip >= 0;
}

bool salsa64_ps3_fastmem_start(struct salsa64_rec *rec)
{
	if (!fm.window)
		return false;
	fm.rec = rec;
	if (!fm.queue_ok) {
		sys_event_queue_attr_t attr;
		memset(&attr, 0, sizeof attr);
		attr.attr_protocol = SYS_EVENT_QUEUE_FIFO;
		attr.type = SYS_EVENT_QUEUE_PPU;
		memcpy(attr.name, "fmfault", 8);
		s32 rc = sysEventQueueCreate(&fm.queue, &attr, SYS_EVENT_QUEUE_KEY_LOCAL, 32);
		if (rc != 0) {
			salsa64_log("fastmem: sin cola de eventos (0x%08x)", (unsigned)rc);
			return false;
		}
		fm.queue_ok = true;
	}
	if (!fm.notify_ok) {
		s32 rc = sc_enable_page_fault_notification(fm.region, (u32)fm.queue);
		if (rc != 0) {
			salsa64_log("fastmem: lv2 no acepta la notificación de fallos (0x%08x): fastmem apagado", (unsigned)rc);
			return false;
		}
		fm.notify_ok = true;
	}
	if (!fm.helper_ok) {
		fm.stop = 0;
		if (sysThreadCreate(&fm.helper, helper_thread, NULL, FM_HELPER_PRIO, 0x8000, THREAD_JOINABLE,
				    (char *)"salsa64_fastmem") != 0) {
			salsa64_log("fastmem: no se pudo crear el hilo ayudante");
			return false;
		}
		fm.helper_ok = true;
		if (!self_test(rec))
			return false;
	}
	return true;
}

void salsa64_ps3_fastmem_stop(void)
{
	if (!fm.helper_ok)
		return;
	fm.stop = 1;
	u64 ret;
	sysThreadJoin(fm.helper, &ret);
	fm.helper_ok = false;
	fm.rec = NULL;
}

void salsa64_ps3_fastmem_counts(uint32_t *faults, uint32_t *unknown, uint32_t *spu)
{
	*faults = fm.faults;
	*unknown = fm.unknown;
	*spu = fm.spu_faults;
}
