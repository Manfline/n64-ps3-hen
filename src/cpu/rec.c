/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — recompilador: despachador, caché de bloques, enlaces e
 * invalidación. La traducción está en rec_emit.c; diseño en docs/rec-ppe.md.
 *
 * El despachador reemplaza al bucle del intérprete (salsa64_cpu_run):
 * atiende los eventos, decide si el bloque en pc puede correr en nativo
 * (modo kernel, KSEG0 o lo de rec_tlb.c, sin interrupción ni delay slot
 * pendientes, y que quepa entero antes del próximo evento) y, si no, deja
 * que el intérprete avance hasta el final del bloque básico. El resultado
 * es el mismo que con el intérprete solo, ciclo por ciclo.
 */
#include "cpu/rec.h"

#include <stddef.h>
#include <string.h>

#include "core/sched.h"
#include "cpu/ppc.h"
#include "cpu/vr4300_internal.h"

#if defined(__powerpc64__) && SALSA64_HOST_BIG_ENDIAN
#define REC_NATIVE 1
#else
#define REC_NATIVE 0
#endif

#define OFF(field) ((int32_t)offsetof(struct salsa64_cpu, field))
/* Sin REC_NATIVE (host que no es ppc64) no se usan, pero compilan igual. */
#define REC_UNUSED __attribute__((unused))
#define FRAME   400   /* marco de la entrada: 112 de enlace y parámetros + r14–r31 + f14–f31 */
#define SAVE    112
#define FSAVE   256

/* rec_emit_fpu.h: constantes en f26–f31 (ver rec.h). */
const double salsa64_rec_fconst[6] = { 1.0, 0.0, 0x1p-60, 0x1p61, 0x1p-440, 0x1p441 };
#define SCRATCH_WORDS  (2 * SALSA64_REC_BUF_WORDS)
#define MAX_BLOCK_WORDS (2 * SALSA64_REC_BUF_WORDS)
#define MAX_BLOCK_BYTES (4 * SALSA64_REC_MAX_INSNS)
#define CALL_REACH (30u << 20)  /* alcance seguro de `bl` (±32 MB) */

_Static_assert(offsetof(struct salsa64_cpu, irq_pending) % 4 == 0 &&
	       offsetof(struct salsa64_cpu, redirect) == offsetof(struct salsa64_cpu, irq_pending) + 1 &&
	       offsetof(struct salsa64_cpu, rec_abort) == offsetof(struct salsa64_cpu, irq_pending) + 2 &&
	       offsetof(struct salsa64_cpu, mode) == offsetof(struct salsa64_cpu, irq_pending) + 3,
	       "el código generado lee irq_pending, redirect, rec_abort y mode con un lwz");
_Static_assert(offsetof(struct salsa64_sched, next) == 0, "el código generado lee sched->next en +0");
_Static_assert(SALSA64_REC_MAX_LINKS < (1u << 31), "índices de enlaces");
_Static_assert(sizeof(struct salsa64_rec_block) == 16 && offsetof(struct salsa64_rec_block, code) == 0,
	       "exit_jr indexa blocks[] con << 4 y lee code en +0");

/* ---- Caché de instrucciones ----------------------------------------------------- */

/* Después de escribir código: dcbst por línea, sync, icbi por línea, isync
 * (docs/jit-ppe.md). Las líneas del PPE son de 128 bytes. */
void salsa64_rec_sync(const void *p, size_t bytes)
{
#if REC_NATIVE
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)127, e = (uintptr_t)p + bytes;
	for (uintptr_t x = a; x < e; x += 128)
		__asm__ volatile("dcbst 0,%0" : : "r"(x) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uintptr_t x = a; x < e; x += 128)
		__asm__ volatile("icbi 0,%0" : : "r"(x) : "memory");
	__asm__ volatile("isync" : : : "memory");
#else
	(void)p, (void)bytes;
#endif
}

/* ---- Perfil (rec_prof.h) ---------------------------------------------------------- */

/* Causa de una carga o un almacenamiento que vino por el camino lento, según
 * la dirección virtual y lo que dio la traducción. El camino rápido toma
 * KSEG0/KSEG1 alineado a la RDRAM (sin código recompilado, al escribir). */
REC_UNUSED static unsigned mem_cause(const struct salsa64_cpu *cpu, uint64_t va, bool ok, uint32_t pa, bool store)
{
	unsigned base = store ? SALSA64_PROF_STORE_TLB_RDRAM : SALSA64_PROF_LOAD_TLB_RDRAM;
	unsigned addr = store ? SALSA64_PROF_STORE_ADDR : SALSA64_PROF_LOAD_ADDR;
	uint32_t a = (uint32_t)va;
	if ((int64_t)va != (int32_t)va)
		return addr;
	if (a >= 0x80000000u && a < 0xC0000000u) {
		if (!ok)
			return addr;
		if (pa >= cpu->rdram_size)
			return store ? SALSA64_PROF_STORE_IO : SALSA64_PROF_LOAD_IO;
		return store ? SALSA64_PROF_STORE_CODE : addr;
	}
	if (!ok)
		return base + 2;  /* falla de TLB */
	return pa < cpu->rdram_size ? base : base + 1;
}

/* Clase de una instrucción que corre el intérprete. */
REC_UNUSED static unsigned exec_cause(uint32_t i)
{
	switch (I_OP(i)) {
	case 0x00: return I_FUNCT(i) >= 0x30 && I_FUNCT(i) <= 0x36 ? SALSA64_PROF_EXEC_TRAP : SALSA64_PROF_EXEC_OTHER;
	case 0x01: return I_RT(i) >= 0x08 && I_RT(i) <= 0x0E ? SALSA64_PROF_EXEC_TRAP : SALSA64_PROF_EXEC_OTHER;
	case 0x10: return SALSA64_PROF_EXEC_COP0;
	case 0x11: return SALSA64_PROF_EXEC_FPU;
	case 0x2F: return SALSA64_PROF_EXEC_CACHE;
	case 0x30: case 0x34: case 0x38: case 0x3C: return SALSA64_PROF_EXEC_LLSC;
	default:   return SALSA64_PROF_EXEC_OTHER;
	}
}

/* El evento vencido: uno solo, su ranura; varios, MANY. */
static unsigned due_event(const struct salsa64_sched *s, uint64_t now)
{
	unsigned ev = SALSA64_PROF_EV_MANY, n = 0;
	for (unsigned i = 0; i < SALSA64_EV_MAX; i++) {
		if (s->when[i] <= now) {
			n++;
			ev = SALSA64_PROF_EV_STOP + i;
		}
	}
	return n == 1 ? ev : SALSA64_PROF_EV_MANY;
}

_Static_assert(SALSA64_EV_MAX == 8 && SALSA64_PROF_EV_MANY == SALSA64_PROF_EV_STOP + SALSA64_EV_MAX,
	       "el perfil cuenta los eventos en el orden de enum salsa64_event");
_Static_assert(SALSA64_PROF_EXIT_START - SALSA64_PROF_EXIT_LINK == SALSA64_REC_EXIT_START - SALSA64_REC_EXIT_LINK,
	       "el perfil cuenta las salidas en el orden de SALSA64_REC_EXIT_*");

/* ---- Llamadas desde el código generado ------------------------------------------ */

REC_UNUSED static uint64_t rec_h_load(struct salsa64_cpu *cpu, uint64_t va, unsigned kind)
{
	static const uint8_t align[] = { 0, 0, 1, 1, 3, 3, 7 };
	struct salsa64_rec_prof *prof = cpu->rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0, v = 0;
	uint32_t pa = 0;
	bool ok = vr4300_vaddr(cpu, va, align[kind], SALSA64_ACC_LOAD, &pa);
	if (ok) {
		if (!SALSA64_REC_OLD(cpu->rec, SALSA64_REC_DBG_NO_DATA))
			salsa64_rec_x_data_fill(cpu->rec, va);
		switch (kind) {
		case SALSA64_REC_LB:  v = salsa64_sx8(vr4300_read8(cpu, pa)); break;
		case SALSA64_REC_LBU: v = vr4300_read8(cpu, pa); break;
		case SALSA64_REC_LH:  v = salsa64_sx16(vr4300_read16(cpu, pa)); break;
		case SALSA64_REC_LHU: v = vr4300_read16(cpu, pa); break;
		case SALSA64_REC_LW:  v = salsa64_sx32(vr4300_read32(cpu, pa)); break;
		case SALSA64_REC_LWU: v = vr4300_read32(cpu, pa); break;
		default:              v = vr4300_read64(cpu, pa); break;
		}
	}
	if (prof)
		salsa64_rec_prof_add(prof, mem_cause(cpu, va, ok, pa, false), salsa64_rec_prof_now() - t0);
	return v;
}

REC_UNUSED static void rec_h_store(struct salsa64_cpu *cpu, uint64_t va, uint64_t v, unsigned kind)
{
	static const uint8_t align[] = { 0, 1, 3, 7 };
	struct salsa64_rec_prof *prof = cpu->rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0;
	uint32_t pa = 0;
	bool ok = vr4300_vaddr(cpu, va, align[kind], SALSA64_ACC_STORE, &pa);
	if (ok) {
		if (!SALSA64_REC_OLD(cpu->rec, SALSA64_REC_DBG_NO_DATA))
			salsa64_rec_x_data_fill(cpu->rec, va);
		switch (kind) {
		case SALSA64_REC_SB: vr4300_write8(cpu, pa, v); break;
		case SALSA64_REC_SH: vr4300_write16(cpu, pa, v); break;
		case SALSA64_REC_SW: vr4300_write32(cpu, pa, (uint32_t)v); break;
		default:             vr4300_write64(cpu, pa, v); break;
		}
	}
	if (prof)
		salsa64_rec_prof_add(prof, mem_cause(cpu, va, ok, pa, true), salsa64_rec_prof_now() - t0);
}

REC_UNUSED static void rec_h_exec(struct salsa64_cpu *cpu, uint32_t instr)
{
	struct salsa64_rec *rec = cpu->rec;
	struct salsa64_rec_prof *prof = rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0;
	rec->stats.exec_calls++;
	salsa64_vr4300_exec_one(cpu, instr);
	if (I_OP(instr) == 0x10) {
		/* COP0 termina el bloque y el código enlaza con el siguiente sin
		 * pasar por el despachador: lo que este haría antes de correr un
		 * bloque va aquí. La TLB, el ASID o ERL (rec_tlb.h); Status.FR (la
		 * caché se compiló para el otro: que el despachador la vacíe). El
		 * modo y las interrupciones los revisa el código al volver. */
		salsa64_rec_x_sync(rec);
		if ((cpu->status & SALSA64_ST_FR) != rec->fr)
			cpu->rec_abort = 1;
	}
	if (prof)
		salsa64_rec_prof_add(prof, exec_cause(instr), salsa64_rec_prof_now() - t0);
}

/* TLBWI/TLBWR desde el código generado (rec_emit_ext.h, emit_tlbw): la
 * escritura y la ktlb al día solo para esa entrada, sin el intérprete ni la
 * comparación de las 32 entradas de salsa64_rec_x_sync. El bloque termina
 * después y enlaza con el siguiente, cuya entrada revisa su página. */
REC_UNUSED static void rec_h_tlbw(struct salsa64_cpu *cpu, uint32_t instr)
{
	struct salsa64_rec *rec = cpu->rec;
	struct salsa64_rec_prof *prof = rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0;
	rec->stats.exec_calls++;
	salsa64_rec_x_tlb_written(rec, salsa64_vr4300_tlbw(cpu, I_FUNCT(instr) == 0x06));
	if (prof)
		salsa64_rec_prof_add(prof, SALSA64_PROF_EXEC_COP0, salsa64_rec_prof_now() - t0);
}

/* ERET desde el código generado (emit_eret): lo que hace el intérprete sin
 * redirect, y devuelve el pc: el código sigue por exit_jr si el modo sigue
 * siendo kernel y no hay interrupción (lo revisa él), si no sale al
 * despachador con ese pc. Si cambió ERL, la ktlb se revisa aquí (antes lo
 * hacía el despachador al volver). */
REC_UNUSED static uint64_t rec_h_eret(struct salsa64_cpu *cpu)
{
	struct salsa64_rec *rec = cpu->rec;
	struct salsa64_rec_prof *prof = rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0;
	rec->stats.exec_calls++;
	salsa64_vr4300_eret(cpu);
	cpu->redirect = 0;
	salsa64_rec_x_sync(rec);
	if (prof)
		salsa64_rec_prof_add(prof, SALSA64_PROF_EXEC_COP0, salsa64_rec_prof_now() - t0);
	return cpu->pc;
}

REC_UNUSED static void rec_h_exception(struct salsa64_cpu *cpu, unsigned code)
{
	struct salsa64_rec_prof *prof = cpu->rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0;
	salsa64_vr4300_exception(cpu, (enum salsa64_exc)code, 0);
	if (prof)
		salsa64_rec_prof_add(prof, SALSA64_PROF_EXCEPTION, salsa64_rec_prof_now() - t0);
}

/* Dirección de código de una función de C (descriptor ELFv1: entrada, TOC). */
REC_UNUSED static uint64_t fn_entry(const void *fn, uint64_t *toc)
{
	const uint64_t *d = fn;
	*toc = d[1];
	return d[0];
}

/* ---- Entrada y salidas (al principio de la caché) -------------------------------- */

/*
 * enter(cpu, código): guarda LR, CR y r14–r31 en un marco propio, carga los
 * registros fijos y salta al bloque. Las salidas dejan en r3 la dirección
 * MIPS donde sigue la ejecución:
 *   exit_link: viene de `bl` en un enlace; el nop parcheable está en LR - 16.
 *   exit_mid, exit_jump: tipo de salida en r4.
 *   común: guarda cycles y el tipo, restaura y vuelve a C.
 *   exit_jr: destino de JR/JALR en r3; si ya hay un bloque compilado ahí,
 *     salta directo a su entrada (que revisa el próximo evento), si no,
 *     sigue como exit_jump.
 */
static uint32_t pre_li64(uint32_t *w, uint32_t n, unsigned rt, uint64_t v)
{
	if ((int64_t)v == (int32_t)v) {
		w[n++] = ppc_lis(rt, (int16_t)(v >> 16));
		w[n++] = ppc_ori(rt, rt, (uint32_t)v & 0xFFFF);
	} else {
		w[n++] = ppc_lis(rt, (int16_t)(v >> 48));
		w[n++] = ppc_ori(rt, rt, (uint32_t)(v >> 32) & 0xFFFF);
		w[n++] = ppc_sldi(rt, rt, 32);
		w[n++] = ppc_oris(rt, rt, (uint32_t)(v >> 16) & 0xFFFF);
		w[n++] = ppc_ori(rt, rt, (uint32_t)v & 0xFFFF);
	}
	return n;
}

/* bc a una rutina ya escrita del preámbulo. */
static uint32_t pre_bc(uint32_t *w, uint32_t n, unsigned bo, unsigned bi, uint32_t to)
{
	w[n] = ppc_bc(bo, bi, ((int32_t)to - (int32_t)n) * 4);
	return n + 1;
}

/* Llamada a una función de C desde el preámbulo (como emit_call). */
REC_UNUSED static uint32_t pre_call(uint32_t *w, uint32_t n, const struct salsa64_rec *rec, uint64_t entry)
{
	if (rec->calls_near) {
		w[n] = ppc_bl((int32_t)((int64_t)entry - (int64_t)(uintptr_t)&rec->code[n]));
		return n + 1;
	}
	n = pre_li64(w, n, 12, entry);
	w[n++] = ppc_mtctr(12);
	w[n++] = PPC_BCTRL;
	return n;
}

REC_UNUSED static uint32_t emit_preamble(struct salsa64_rec *rec)
{
	uint32_t *w = rec->code, n = 0;

	w[n++] = ppc_mflr(0);
	w[n++] = ppc_std(0, 16, 1);
	w[n++] = ppc_mfcr(12);
	w[n++] = ppc_stw(12, 8, 1);
	w[n++] = ppc_stdu(1, -FRAME, 1);
	for (unsigned r = 14; r <= 31; r++)
		w[n++] = ppc_std(r, SAVE + 8 * (int32_t)(r - 14), 1);
	for (unsigned r = 14; r <= 31; r++)  /* la FPU residente (rec_emit_fpu.h) usa f14–f31, no volátiles */
		w[n++] = ppc_stfd(r, FSAVE + 8 * (int32_t)(r - 14), 1);
	n = pre_li64(w, n, 11, (uintptr_t)salsa64_rec_fconst);
	for (unsigned k = 0; k < 6; k++)
		w[n++] = ppc_lfd(26 + k, 8 * (int32_t)k, 11);
	w[n++] = ppc_mr(31, 3);
	w[n++] = ppc_ld(30, OFF(rdram), 31);
	w[n++] = ppc_ld(29, OFF(cycles), 31);
	w[n++] = ppc_ld(28, OFF(code_flags), 31);
	w[n++] = ppc_ld(11, OFF(sched), 31);
	w[n++] = ppc_ld(27, (int32_t)offsetof(struct salsa64_sched, next), 11);
	if (rec->opts & SALSA64_REC_OPT_PIN)
		for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++)
			w[n++] = ppc_ld(SALSA64_REC_PIN_PPC + k, OFF(gpr) + 8 * salsa64_rec_pin[k], 31);
	w[n++] = ppc_mtctr(4);
	w[n++] = PPC_BCTR;

	uint32_t common = n + 16;
	rec->exit_link = n;
	w[n++] = ppc_mflr(4);
	w[n++] = ppc_addi(4, 4, -16);
	w[n++] = ppc_std(4, OFF(rec_site), 31);
	w[n++] = ppc_li(4, SALSA64_REC_EXIT_LINK);
	w[n] = ppc_b((int32_t)(common - n) * 4);
	n++;
	rec->exit_mid = n;
	w[n++] = ppc_li(4, SALSA64_REC_EXIT_MID);
	w[n] = ppc_b((int32_t)(common - n) * 4);
	n++;
	rec->exit_jump = n;
	w[n++] = ppc_li(4, SALSA64_REC_EXIT_JUMP);
	w[n] = ppc_b((int32_t)(common - n) * 4);
	n++;
	rec->exit_idle = n;
	w[n++] = ppc_li(4, SALSA64_REC_EXIT_IDLE);
	w[n] = ppc_b((int32_t)(common - n) * 4);
	n++;
	rec->exit_start = n;
	w[n++] = ppc_li(4, SALSA64_REC_EXIT_START);
	w[n] = ppc_b((int32_t)(common - n) * 4);
	n++;
	rec->exit_end = n;
	w[n++] = ppc_li(4, SALSA64_REC_EXIT_END);
	w[n] = ppc_b((int32_t)(common - n) * 4);
	n++;
	while (n < common)
		w[n++] = PPC_NOP;

	w[n++] = ppc_std(29, OFF(cycles), 31);
	w[n++] = ppc_stw(4, OFF(rec_kind), 31);
	if (rec->opts & SALSA64_REC_OPT_PIN)
		for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++)
			w[n++] = ppc_std(SALSA64_REC_PIN_PPC + k, OFF(gpr) + 8 * salsa64_rec_pin[k], 31);
	for (unsigned r = 14; r <= 31; r++)
		w[n++] = ppc_ld(r, SAVE + 8 * (int32_t)(r - 14), 1);
	for (unsigned r = 14; r <= 31; r++)
		w[n++] = ppc_lfd(r, FSAVE + 8 * (int32_t)(r - 14), 1);
	w[n++] = ppc_addi(1, 1, FRAME);
	w[n++] = ppc_ld(0, 16, 1);
	w[n++] = ppc_mtlr(0);
	w[n++] = ppc_lwz(12, 8, 1);
	w[n++] = ppc_mtocrf(0x20, 12);  /* cr2–cr4, los no volátiles (mtcrf de varios campos es microcódigo) */
	w[n++] = ppc_mtocrf(0x10, 12);
	w[n++] = ppc_mtocrf(0x08, 12);
	w[n++] = PPC_BLR;

	/* exit_jr: KSEG0, alineada y dentro de la RDRAM → map → blocks → código.
	 * Fuera de KSEG0, xjr (abajo). */
	const unsigned ne = PPC_BO_FALSE_UNLIKELY, eq = PPC_BO_TRUE_UNLIKELY, EQ = PPC_BI(0, PPC_EQ);
	rec->exit_jr = n;
	w[n++] = ppc_sradi(11, 3, 29);
	w[n++] = ppc_cmpdi(0, 11, -4);
	uint32_t to_xjr = n++;
	w[n++] = ppc_clrldi(12, 3, 35);
	w[n++] = ppc_srdi(11, 12, rec->rdram_shift);
	w[n++] = ppc_clrldi(0, 12, 62);
	w[n++] = ppc_or(11, 11, 0);
	w[n++] = ppc_cmpdi(0, 11, 0);
	n = pre_bc(w, n, ne, EQ, rec->exit_jump);
	w[n++] = ppc_srdi(12, 12, 1);  /* map es de uint16_t por palabra */
	n = pre_li64(w, n, 11, (uintptr_t)rec->map);
	w[n++] = ppc_lhzx(12, 11, 12);
	w[n++] = ppc_cmpwi(0, 12, 0);
	n = pre_bc(w, n, eq, EQ, rec->exit_jump);
	uint32_t id_ok = n;  /* r12 = id */
	w[n++] = ppc_sldi(12, 12, 4);
	n = pre_li64(w, n, 11, (uintptr_t)rec->blocks);
	w[n++] = ppc_lwzx(12, 11, 12);
	w[n++] = ppc_cmpwi(0, 12, 0);
	n = pre_bc(w, n, eq, EQ, rec->exit_jump);
	w[n++] = ppc_sldi(12, 12, 2);
	n = pre_li64(w, n, 11, (uintptr_t)rec->code);
	w[n++] = ppc_add(12, 11, 12);
	w[n++] = ppc_mtctr(12);
	w[n++] = PPC_BCTR;

	/* xjr (rec_tlb.h): extensión de signo de 32 bits → su cadena de xhash,
	 * el primero con la misma vpc → blocks → código. Si es un bloque
	 * mapeado con otra física, su entrada lo nota y sale (el despachador
	 * pone al frente el que vale). */
	w[to_xjr] = ppc_bc(ne, EQ, (int32_t)(n - to_xjr) * 4);
	w[n++] = ppc_extsw(11, 3);
	w[n++] = ppc_cmpd(0, 11, 3);
	n = pre_bc(w, n, ne, EQ, rec->exit_jump);
	w[n++] = ppc_rlwinm(12, 3, 31, 17, 30);  /* ((vpc >> 2) & (XHASH - 1)) * 2 */
	n = pre_li64(w, n, 11, (uintptr_t)rec->x.xhash);
	w[n++] = ppc_lhzx(12, 11, 12);
	if (SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_CHAIN)) {  /* solo la cabeza, como antes */
		w[n++] = ppc_cmpwi(0, 12, 0);
		n = pre_bc(w, n, eq, EQ, rec->exit_jump);
		w[n++] = ppc_sldi(0, 12, 3);
		n = pre_li64(w, n, 11, (uintptr_t)rec->x.xb);
		w[n++] = ppc_lwzx(0, 11, 0);
		w[n++] = ppc_cmplw(0, 0, 3);
		n = pre_bc(w, n, ne, EQ, rec->exit_jump);
		w[n] = ppc_b((int32_t)(id_ok - n) * 4);
		n++;  /* y siguen las rutinas compartidas (el camino lento las usa también) */
	} else {
		n = pre_li64(w, n, 11, (uintptr_t)rec->x.xb);
		w[n++] = ppc_addi(10, 11, (int32_t)offsetof(struct salsa64_rec_xblock, hnext));
		uint32_t chain = n;  /* r12 = id, r11 = xb, r10 = xb + hnext */
		w[n++] = ppc_cmpwi(0, 12, 0);
		n = pre_bc(w, n, eq, EQ, rec->exit_jump);
		w[n++] = ppc_sldi(0, 12, 3);
		w[n++] = ppc_lwzx(9, 11, 0);
		w[n++] = ppc_cmplw(0, 9, 3);
		n = pre_bc(w, n, eq, EQ, id_ok);
		w[n++] = ppc_lhzx(12, 10, 0);
		w[n] = ppc_b((int32_t)(chain - n) * 4);
		n++;
	}

	/* Traducción de datos por ktlb[] (rec_emit.c, emit_addr; rec_tlb.h): la
	 * dirección en r3 es extensión de signo de 32 bits y está alineada, su
	 * entrada tiene la etiqueta y es válida (y D al almacenar) y la física
	 * cae en la RDRAM: cr1 eq y r12 = desplazamiento; si no, cr1 ne. Usa r0,
	 * r11 y r12. Compartida para no repetir ~20 palabras en cada acceso. */
	static const uint8_t aligns[4] = { 0, 1, 3, 7 };
	const unsigned NE1 = PPC_BI(1, PPC_EQ);
	for (unsigned st = 0; st < 2; st++) {
		for (unsigned a = 0; a < 4; a++) {
			unsigned align = aligns[a];
			rec->ktlb_rtn[st][a] = n;
			w[n++] = ppc_extsw(11, 3);
			w[n++] = ppc_cmpd(1, 11, 3);
			w[n++] = ppc_bclr(PPC_BO_FALSE, NE1);
			if (align) {
				w[n++] = ppc_clrldi(0, 3, align == 1 ? 63 : align == 3 ? 62 : 61);
				w[n++] = ppc_cmpdi(1, 0, 0);
				w[n++] = ppc_bclr(PPC_BO_FALSE, NE1);
			}
			w[n++] = ppc_rlwinm(11, 3, 22, 16, 29);   /* ((va >> 12) & 16383) * 4 */
			w[n++] = ppc_addis(12, 28, -1);            /* ktlb[] termina donde empieza flags (r28) */
			w[n++] = ppc_lwzx(11, 12, 11);
			if (st) {
				w[n++] = ppc_rlwinm(0, 11, 8, 24, 31);     /* etiqueta, válida y D */
				w[n++] = ppc_rlwinm(12, 3, 8, 24, 29);
				w[n++] = ppc_ori(12, 12, 3);
			} else {
				w[n++] = ppc_rlwinm(0, 11, 7, 25, 31);     /* etiqueta y válida */
				w[n++] = ppc_rlwinm(12, 3, 7, 25, 30);
				w[n++] = ppc_ori(12, 12, 1);
			}
			w[n++] = ppc_cmpw(1, 0, 12);
			w[n++] = ppc_bclr(PPC_BO_FALSE, NE1);
			w[n++] = ppc_rlwinm(12, 11, 12, 3, 19);    /* página física << 12 */
			w[n++] = ppc_rlwinm(0, 3, 0, 20, 31);
			w[n++] = ppc_or(12, 12, 0);
			w[n++] = ppc_srdi(0, 12, rec->rdram_shift);  /* dentro de la RDRAM: eq */
			w[n++] = ppc_cmpdi(1, 0, 0);
			w[n++] = PPC_BLR;
		}
	}

	/* Camino lento de cargas y almacenamientos, compartido por tipo
	 * (rec_emit.c, emit_load/emit_store): r3 = va (r5 = valor al almacenar),
	 * r6 = (k + 1) | delay slot << 8 | n << 16, r7 = pc de la instrucción.
	 * Deja cycles, cur_pc y delay_slot como el intérprete, llama a C y recarga
	 * sched->next (r27). Vuelve con r3 = valor y cr1 ne si hubo excepción
	 * (redirect); si no, cr5 gt si hay que salir (interrupción, código
	 * invalidado, modo, o el bloque ya no cabe antes del evento). Guarda LR y
	 * el fin del bloque en cpu->rec_lr y rec_tmp. Antes se repetía todo esto
	 * (~25 palabras) en cada carga y almacenamiento. */
	for (unsigned st = 0; st < 2; st++) {
		for (unsigned kind = 0; kind < (st ? 4u : 7u); kind++) {
			(st ? rec->slow_store : rec->slow_load)[kind] = n;
			w[n++] = ppc_rlwinm(0, 6, 0, 24, 31);       /* k + 1 */
			w[n++] = ppc_add(0, 0, 29);
			w[n++] = ppc_std(0, OFF(cycles), 31);
			w[n++] = ppc_std(7, OFF(cur_pc), 31);
			w[n++] = ppc_rlwinm(0, 6, 24, 31, 31);      /* delay slot (bit 8) */
			w[n++] = ppc_stb(0, OFF(delay_slot), 31);
			w[n++] = ppc_rlwinm(0, 6, 16, 24, 31);      /* n */
			w[n++] = ppc_add(0, 0, 29);
			w[n++] = ppc_std(0, OFF(rec_tmp), 31);
			w[n++] = ppc_mflr(0);
			w[n++] = ppc_std(0, OFF(rec_lr), 31);
			w[n++] = ppc_mr(4, 3);
			w[n++] = ppc_mr(3, 31);
			w[n++] = ppc_li(st ? 6 : 5, (int32_t)kind);
			n = pre_call(w, n, rec, st ? rec->h_store : rec->h_load);
			w[n++] = ppc_ld(0, OFF(rec_lr), 31);
			w[n++] = ppc_mtlr(0);
			w[n++] = ppc_lbz(0, OFF(redirect), 31);
			w[n++] = ppc_cmpwi(1, 0, 0);
			w[n++] = ppc_bclr(PPC_BO_FALSE, PPC_BI(1, PPC_EQ));  /* excepción: cr1 ne */
			w[n++] = ppc_lwz(0, OFF(irq_pending), 31);         /* irq, redirect, abort y modo */
			w[n++] = ppc_cmpwi(5, 0, 0);
			w[n++] = ppc_bclr(PPC_BO_FALSE, PPC_BI(5, PPC_EQ));  /* salir: cr5 gt */
			w[n++] = ppc_ld(11, OFF(sched), 31);
			w[n++] = ppc_ld(27, (int32_t)offsetof(struct salsa64_sched, next), 11);
			w[n++] = ppc_ld(0, OFF(rec_tmp), 31);
			w[n++] = ppc_cmpld(5, 0, 27);                       /* no cabe: cr5 gt */
			w[n++] = PPC_BLR;
		}
	}
	return n;
}

typedef uint64_t (*rec_enter_fn)(struct salsa64_cpu *cpu, const uint32_t *code);

static uint64_t rec_enter(struct salsa64_rec *rec, uint32_t entry)
{
	rec_enter_fn f = (rec_enter_fn)(void *)rec->enter_opd;
	return f(rec->cpu, rec->code + entry);
}

/* ---- Bloques, enlaces y banderas ------------------------------------------------- */

static uint32_t preamble_words(void)
{
	return 1024;  /* emit_preamble usa menos; los bloques empiezan alineados */
}

/* ---- Tramos de la caché (docs/rec-ppe.md, «Desalojo por tramos») ------------- */

static uint32_t seg_start(const struct salsa64_rec *rec, unsigned s)
{
	return preamble_words() + s * rec->seg_words;
}

static unsigned seg_of(const struct salsa64_rec *rec, uint32_t word)
{
	unsigned s = (word - preamble_words()) / rec->seg_words;
	return s < SALSA64_REC_SEGMENTS ? s : SALSA64_REC_SEGMENTS - 1;
}

/* Al vaciar: el primer tramo (o toda la zona de bloques, sin desalojo). */
static void seg_reset(struct salsa64_rec *rec)
{
	rec->cur_seg = 0;
	rec->used = preamble_words();
	rec->seg_end = SALSA64_REC_OLD(rec, SALSA64_REC_OPT_NO_EVICT) ? rec->scratch : seg_start(rec, 1);
}

/* ¿Las funciones de C quedan a menos de CALL_REACH de un bloque que empieza
 * en la palabra `base` (y ocupa hasta MAX_BLOCK_WORDS)? Si no, ese bloque las
 * llama por mtctr/bctrl (rec_emit.c). */
bool salsa64_rec_calls_near_at(const struct salsa64_rec *rec, uint32_t base)
{
	uint64_t lo = (uintptr_t)(rec->code + base), hi = lo + 4ull * MAX_BLOCK_WORDS;
	uint64_t h[6] = { rec->h_load, rec->h_store, rec->h_exec, rec->h_exception, rec->h_tlbw, rec->h_eret };
	for (unsigned k = 0; k < 6; k++) {
		uint64_t far = h[k] < lo ? hi - h[k] : h[k] > hi ? h[k] - lo : hi - lo;
		if (far >= CALL_REACH)
			return false;
	}
	return true;
}

void salsa64_rec_flush(struct salsa64_rec *rec)
{
	if (rec->prof && rec->prof->blocks)
		for (unsigned id = 1; id < rec->next_block; id++)
			if (rec->blocks[id].code)
				salsa64_rec_prof_fold(rec->prof, id, false);
	if (rec->map_dirty) {
		memset(rec->map, 0, (rec->rdram_size >> 2) * sizeof rec->map[0]);
		memset(rec->flags, 0, rec->rdram_size >> SALSA64_REC_FLAG_SHIFT);
		salsa64_rec_x_flush(rec);
		rec->map_dirty = false;
		rec->stats.flushes++;
	}
	rec->free_block = 0;
	rec->next_block = 1;
	rec->free_link = 0;
	rec->next_link = 0;
	seg_reset(rec);
	rec->generation++;
	rec->cpu->rec_abort = 1;
}

static void flags_add(struct salsa64_rec *rec, uint32_t pa, uint32_t end, int delta)
{
	for (uint32_t r = pa >> SALSA64_REC_FLAG_SHIFT; r <= (end - 1) >> SALSA64_REC_FLAG_SHIFT; r++) {
		uint8_t f = rec->flags[r];
		if (f == 255)
			continue;  /* saturada: queda marcada hasta el próximo vaciado */
		rec->flags[r] = (uint8_t)(f + delta);
	}
}

static void unlink_all(struct salsa64_rec *rec, struct salsa64_rec_block *b)
{
	uint32_t l = b->links;
	while (l) {
		struct salsa64_rec_link *lk = &rec->links[l - 1];
		uint32_t next = lk->next;
		if (rec->seg_gen[seg_of(rec, lk->site)] == lk->gen) {  /* si no, el tramo del sitio se desalojó */
			rec->code[lk->site] = PPC_NOP;
			salsa64_rec_sync(&rec->code[lk->site], 4);
		}
		lk->next = rec->free_link;
		rec->free_link = l;
		l = next;
	}
	b->links = 0;
}

static void kill_block(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_block *b = &rec->blocks[id];
	bool detached = rec->x.state[id] == SALSA64_REC_X_DETACHED;  /* ya sin banderas, enlaces ni código */
	if (rec->prof && rec->prof->blocks)
		salsa64_rec_prof_fold(rec->prof, id, false);
	if (!salsa64_rec_x_remove(rec, id))
		rec->map[b->pa >> 2] = 0;
	if (b->pa < rec->rdram_size && !detached)  /* no en la ROM */
		flags_add(rec, b->pa, b->pa + b->bytes, -1);
	unlink_all(rec, b);
	b->code = 0;
	b->links = rec->free_block;
	rec->free_block = (uint16_t)id;
	rec->stats.invalidated++;
	rec->cpu->rec_abort = 1;
}

void salsa64_rec_kill_block(struct salsa64_rec *rec, unsigned id)
{
	kill_block(rec, id);
}

void salsa64_rec_unlink_block(struct salsa64_rec *rec, unsigned id)
{
	unlink_all(rec, &rec->blocks[id]);
}

bool salsa64_rec_block_rekey(struct salsa64_rec *rec, unsigned id, uint32_t pa, uint32_t vt, uint32_t code,
			     bool had_flags)
{
	struct salsa64_rec_block *b = &rec->blocks[id];
	uint32_t *w = &rec->code[code];
	/* La entrada de un bloque mapeado (rec_emit.c): [addis] lwz ; xoris hi ; cmplwi lo ; bne. */
	unsigned k = (w[0] >> 26) == 15 ? 2 : 1;
	if ((w[k] >> 26) != 27 || (w[k + 1] >> 26) != 10)
		return false;
	if (had_flags)
		flags_add(rec, b->pa, b->pa + b->bytes, -1);
	flags_add(rec, pa, pa + b->bytes, +1);
	b->pa = pa;
	b->code = code;
	w[k] = (w[k] & 0xFFFF0000u) | (vt >> 16);
	w[k + 1] = (w[k + 1] & 0xFFFF0000u) | (vt & 0xFFFF);
	salsa64_rec_sync(w, 4 * (k + 2));
	rec->map_dirty = true;
	return true;
}

void salsa64_rec_block_detach(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_block *b = &rec->blocks[id];
	unlink_all(rec, b);
	flags_add(rec, b->pa, b->pa + b->bytes, -1);
	rec->x.saved_code[id] = b->code;
	b->code = 0;
	rec->stats.invalidated++;
	rec->cpu->rec_abort = 1;
}

void salsa64_rec_invalidate(struct salsa64_rec *rec, uint32_t pa, uint32_t len)
{
	if (!len || pa >= rec->rdram_size || !rec->map_dirty)
		return;
	if (len > rec->rdram_size - pa)
		len = rec->rdram_size - pa;
	if (pa == 0 && len == rec->rdram_size) {
		salsa64_rec_flush(rec);
		return;
	}
	uint32_t end = pa + len, lo = pa > MAX_BLOCK_BYTES ? pa - MAX_BLOCK_BYTES : 0;
	/* Un bloque que empieza en una región la marca: basta mirar las
	 * regiones marcadas entre lo y end. */
	for (uint32_t r = lo >> SALSA64_REC_FLAG_SHIFT; r <= (end - 1) >> SALSA64_REC_FLAG_SHIFT; r++) {
		if (!rec->flags[r])
			continue;
		uint32_t w0 = r << SALSA64_REC_FLAG_SHIFT, w1 = w0 + (1u << SALSA64_REC_FLAG_SHIFT);
		if (w0 < lo)
			w0 = lo;
		if (w1 > end)
			w1 = end;
		for (uint32_t w = w0; w < w1; w += 4) {
			unsigned id = rec->map[w >> 2];
			if (id && rec->blocks[id].pa + rec->blocks[id].bytes > pa)
				kill_block(rec, id);
		}
	}
	salsa64_rec_x_invalidate(rec, pa, end);
}

void salsa64_rec_code_written(struct salsa64_rec *rec, uint32_t pa, uint32_t len)
{
	salsa64_rec_invalidate(rec, pa, len);
}

static bool rec_ids_ok(const struct salsa64_rec *rec)
{
	return (rec->free_block || rec->next_block <= SALSA64_REC_MAX_BLOCKS) &&
	       (rec->free_link || rec->next_link < SALSA64_REC_MAX_LINKS);
}

static bool rec_space(const struct salsa64_rec *rec)
{
	return rec->used + MAX_BLOCK_WORDS <= rec->seg_end && rec_ids_ok(rec);
}

/* El tramo en uso se llenó: pasar al siguiente (el más viejo) matando solo
 * los bloques cuyo código, o el guardado si están desprendidos, está en él.
 * Los enlaces que salen de ellos quedan en las listas de sus destinos con
 * el sitio en este tramo: seg_gen[] sube para que no se parcheen al
 * deshacerlos, y generation para los sitios anotados en el despachador. */
static void evict_next_segment(struct salsa64_rec *rec)
{
	unsigned s = (rec->cur_seg + 1) % SALSA64_REC_SEGMENTS;
	uint32_t lo = seg_start(rec, s), hi = seg_start(rec, s + 1);
	for (unsigned id = 1; id < rec->next_block; id++) {
		const struct salsa64_rec_block *b = &rec->blocks[id];
		uint32_t code = b->code ? b->code : rec->x.state[id] == SALSA64_REC_X_DETACHED ? rec->x.saved_code[id] : 0;
		if (code >= lo && code < hi)
			kill_block(rec, id);
	}
	rec->seg_gen[s]++;
	rec->generation++;
	rec->cur_seg = s;
	rec->used = lo;
	rec->seg_end = hi;
	rec->stats.evictions++;
	rec->cpu->rec_abort = 1;
}

static unsigned new_block(struct salsa64_rec *rec, uint64_t pc, uint32_t pa, unsigned seg)
{
	if (!rec_space(rec)) {
		if (!rec_ids_ok(rec) || SALSA64_REC_OLD(rec, SALSA64_REC_OPT_NO_EVICT))
			salsa64_rec_flush(rec);
		else
			evict_next_segment(rec);
	}
	unsigned id;
	if (rec->free_block) {
		id = rec->free_block;
		rec->free_block = (uint16_t)rec->blocks[id].links;
	} else {
		id = rec->next_block++;
	}
	unsigned n = 0;
	uint32_t bytes = 4;
	rec->cur_id = id;
	uint32_t code = salsa64_rec_compile(rec, pc, pa, seg, SALSA64_REC_MAX_INSNS, false, &n, &bytes);
	rec->cur_id = 0;
	struct salsa64_rec_block *b = &rec->blocks[id];
	b->pa = pa;
	b->code = code;
	b->n = (uint16_t)n;
	b->bytes = (uint16_t)(code ? bytes : 4);
	b->links = 0;
	if (seg == SALSA64_REC_KSEG0)
		rec->map[pa >> 2] = (uint16_t)id;
	salsa64_rec_x_add(rec, id, pc, pa, seg, code != 0);
	rec->map_dirty = true;
	if (pa < rec->rdram_size)  /* no en la ROM */
		flags_add(rec, b->pa, b->pa + b->bytes, +1);
	if (code) {
		rec->stats.compiled++;
	} else {
		rec->stats.interp_only++;
	}
	return id;
}

/* Parchea el nop de `site` con un `b` al bloque. */
static void link_block(struct salsa64_rec *rec, uint32_t site, unsigned id)
{
	struct salsa64_rec_block *b = &rec->blocks[id];
	int64_t disp = ((int64_t)b->code - site) * 4;
	if (site < preamble_words() || site >= rec->scratch || rec->code[site] != PPC_NOP || !ppc_fits_b(disp))
		return;
	uint32_t l;
	if (rec->free_link) {
		l = rec->free_link;
		rec->free_link = rec->links[l - 1].next;
	} else if (rec->next_link < SALSA64_REC_MAX_LINKS) {
		l = ++rec->next_link;
	} else {
		return;
	}
	rec->links[l - 1].site = site;
	rec->links[l - 1].gen = rec->seg_gen[seg_of(rec, site)];
	rec->links[l - 1].next = b->links;
	b->links = l;
	rec->code[site] = ppc_b((int32_t)disp);
	salsa64_rec_sync(&rec->code[site], 4);
	rec->stats.linked++;
}

/* ---- Despachador ----------------------------------------------------------------- */

/* Despacha los eventos vencidos y, con el perfil, los mide por tipo. */
static void rec_dispatch(struct salsa64_rec *rec)
{
	struct salsa64_cpu *cpu = rec->cpu;
	struct salsa64_sched *s = cpu->sched;
	struct salsa64_rec_prof *prof = rec->prof;
	if (!prof) {
		salsa64_sched_dispatch(s, cpu->cycles);
		return;
	}
	unsigned ev = due_event(s, cpu->cycles);
	uint64_t t0 = salsa64_rec_prof_now();
	salsa64_sched_dispatch(s, cpu->cycles);
	prof->t[ev] += salsa64_rec_prof_now() - t0;
	prof->n[ev]++;
}

/* El intérprete avanza hasta terminar un delay slot, tomar una excepción o
 * (en modo estricto) una sola instrucción. Atiende los eventos que lleguen
 * (como salsa64_cpu_step: después de cada instrucción). `why` es la causa
 * para el perfil. */
static void rec_interp(struct salsa64_rec *rec, bool to_boundary, unsigned why)
{
	struct salsa64_cpu *cpu = rec->cpu;
	struct salsa64_sched *s = cpu->sched;
	struct salsa64_rec_prof *prof = rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0, t_ev = 0;
	uint32_t exc = cpu->exc_count;
	unsigned k = 0;
	do {
		bool ds = cpu->branch != 0;
		salsa64_cpu_step_only(cpu);
		k++;
		if (cpu->cycles >= s->next) {
			uint64_t te = prof ? salsa64_rec_prof_now() : 0;
			rec_dispatch(rec);
			if (prof)
				t_ev += salsa64_rec_prof_now() - te;
		}
		if (!to_boundary || cpu->exc_count != exc || (ds && !cpu->branch))
			break;
	} while (k < 4096 && cpu->running);
	rec->stats.interp_steps += k;
	if (prof) {
		prof->t[why] += salsa64_rec_prof_now() - t0 - t_ev;
		prof->n[why] += k;
	}
}

/* El código salió en un salto tomado hacia atrás y corto (EXIT_IDLE): lo
 * mismo que hace el intérprete en idle_hint, con el mismo estado (registros
 * en memoria, cycles con el salto contado, el delay slot por delante). */
static void rec_idle_exit(struct salsa64_rec *rec, uint64_t bpc)
{
	struct salsa64_cpu *cpu = rec->cpu;
	uint32_t i = (uint32_t)cpu->rec_site;  /* la instrucción del salto (rec_emit.c) */
	uint64_t target = I_OP(i) == 0x02 ? ((bpc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2
					  : bpc + 4 + (I_SIMM(i) << 2);
	cpu->pc = bpc + 4;
	cpu->next_pc = target;
	cpu->branch = 1;
	cpu->delay_slot = 0;
	cpu->cur_pc = bpc;
	salsa64_vr4300_idle(cpu, bpc, target, i);
	cpu->redirect = 0;  /* si saltó tiempo: pc, next_pc, branch y cycles ya están */
	if (cpu->idle_reject[(bpc >> 2) & (SALSA64_IDLE_CACHE - 1)] != ((uint32_t)bpc ^ i))
		return;
	/* Recompilar sin la consulta. */
	if ((bpc >> 29) == 0x7FFFFFFFCull)
		salsa64_rec_invalidate(rec, (uint32_t)bpc & 0x1FFFFFFF, 4);
	else
		salsa64_rec_x_kill_at(rec, bpc);
}

void salsa64_rec_run(struct salsa64_rec *rec)
{
	struct salsa64_cpu *cpu = rec->cpu;
	struct salsa64_sched *s = cpu->sched;
	struct salsa64_rec_prof *prof = rec->prof;
	bool to_boundary = false, from_start = false;
	uint32_t site = 0, site_gen = 0;
	uint64_t t_run = prof ? salsa64_rec_prof_now() : 0, t0 = 0;

	cpu->running = 1;
	while (cpu->running) {
		if (cpu->cycles >= s->next) {
			rec_dispatch(rec);
			continue;
		}
		uint64_t pc = cpu->pc;
		uint32_t pa = 0;
		unsigned seg = SALSA64_REC_SEG_NONE, why = 0;
		if (!to_boundary && !cpu->irq_pending && !cpu->branch && cpu->mode == SALSA64_MODE_KERNEL) {
			salsa64_rec_x_sync(rec);  /* antes de correr cualquier bloque (rec_tlb.h) */
			seg = salsa64_rec_seg(rec, pc, &pa);
		}
		if (seg == SALSA64_REC_SEG_NONE) {
			if (prof)
				why = to_boundary ? (from_start ? SALSA64_PROF_INTERP_START : SALSA64_PROF_INTERP_MID)
				      : cpu->irq_pending ? SALSA64_PROF_INTERP_IRQ
				      : cpu->branch ? SALSA64_PROF_INTERP_DS
				      : cpu->mode != SALSA64_MODE_KERNEL ? SALSA64_PROF_INTERP_MODE
				      : SALSA64_PROF_INTERP_SEG;
			rec_interp(rec, !rec->strict, why);
			to_boundary = false;
			site = 0;
			continue;
		}
		if ((cpu->status & SALSA64_ST_FR) != rec->fr) {
			salsa64_rec_flush(rec);
			rec->fr = cpu->status & SALSA64_ST_FR;
		}

		unsigned id = seg == SALSA64_REC_KSEG0 ? rec->map[pa >> 2] : salsa64_rec_x_find(rec, pc, pa);
		if (!id && seg == SALSA64_REC_MAPPED && !SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_DETACH))
			id = salsa64_rec_x_rekey(rec, pc, pa);
		if (!id) {
			if (prof)
				t0 = salsa64_rec_prof_now();
			id = new_block(rec, pc, pa, seg);
			if (prof) {
				prof->t[SALSA64_PROF_COMPILE] += salsa64_rec_prof_now() - t0;
				prof->n[SALSA64_PROF_COMPILE]++;
			}
		}
		const struct salsa64_rec_block *b = &rec->blocks[id];
		uint32_t entry = b->code;
		if (!entry) {
			rec_interp(rec, !rec->strict, SALSA64_PROF_INTERP_NOCODE);
			site = 0;
			continue;
		}
		if (cpu->cycles + b->n > s->next) {
			/* No cabe antes del evento. Normalmente lo termina el
			 * intérprete; en modo estricto se compila recortado. */
			unsigned n = 0;
			uint32_t bytes = 0;
			if (rec->strict) {
				if (prof)
					t0 = salsa64_rec_prof_now();
				entry = salsa64_rec_compile(rec, pc, pa, seg, (unsigned)(s->next - cpu->cycles), true, &n,
							    &bytes);
				if (prof) {
					prof->t[SALSA64_PROF_COMPILE] += salsa64_rec_prof_now() - t0;
					prof->n[SALSA64_PROF_COMPILE]++;
				}
			} else {
				entry = 0;
			}
			if (!entry) {
				rec_interp(rec, !rec->strict, SALSA64_PROF_INTERP_NOFIT);
				site = 0;
				continue;
			}
			rec->stats.temp_compiled++;
		} else if (site && site_gen == rec->generation && !rec->no_link) {
			link_block(rec, site, id);
		}
		site = 0;

		uint32_t exc = cpu->exc_count;
		uint64_t cycles = cpu->cycles, nested = 0;
		cpu->redirect = 0;  /* salsa64_cpu_set_pc lo deja en 1 para el intérprete; aquí pc ya está leído */
		cpu->rec_site = 0;
		cpu->rec_abort = 0;
		rec->stats.entries++;
		if (prof) {
			nested = prof->nested;
			t0 = salsa64_rec_prof_now();
		}
		uint64_t next = rec_enter(rec, entry);
		if (prof) {
			/* Lo que tardó el código generado sin sus llamadas a C. */
			prof->t[SALSA64_PROF_NATIVE] += salsa64_rec_prof_now() - t0 - (prof->nested - nested);
			prof->n[SALSA64_PROF_NATIVE]++;
			prof->n[cpu->redirect ? SALSA64_PROF_EXIT_REDIRECT
					      : SALSA64_PROF_EXIT_LINK + (cpu->rec_kind - SALSA64_REC_EXIT_LINK)]++;
		}
		from_start = false;
		if (cpu->redirect) {
			/* Excepción o ERET: pc, next_pc y branch ya están puestos; el
			 * bloque que sigue se busca como cualquier otro (con LEGACY, tras
			 * un ERET el intérprete hasta el fin del bloque básico, como antes). */
			cpu->redirect = 0;
			to_boundary = SALSA64_REC_OLD(rec, SALSA64_REC_DBG_ERET_BOUND) && cpu->exc_count == exc && !rec->strict;
		} else if (cpu->rec_kind == SALSA64_REC_EXIT_IDLE) {
			/* Deja el delay slot pendiente: lo corre el intérprete. */
			if (prof)
				t0 = salsa64_rec_prof_now();
			rec_idle_exit(rec, next);
			if (prof) {
				prof->t[SALSA64_PROF_IDLE] += salsa64_rec_prof_now() - t0;
				prof->n[SALSA64_PROF_IDLE]++;
			}
		} else {
			cpu->pc = next;
			cpu->next_pc = next + 4;
			cpu->branch = 0;
			if (cpu->rec_kind == SALSA64_REC_EXIT_LINK && cpu->rec_site) {
				site = (uint32_t)((cpu->rec_site - (uintptr_t)rec->code) / 4);
				site_gen = rec->generation;
			} else if (cpu->rec_kind == SALSA64_REC_EXIT_MID || cpu->rec_kind == SALSA64_REC_EXIT_START) {
				/* En modo estricto solo si el bloque no avanzó (salió en
				 * la entrada, p. ej. con COP1 inusable): una instrucción. */
				to_boundary = !rec->strict || cpu->cycles == cycles;
				from_start = cpu->rec_kind == SALSA64_REC_EXIT_START;
			}
		}
		if (rec->after_exit)
			rec->after_exit(rec, pc, rec->after_exit_ctx);
	}
	if (prof)
		prof->total += salsa64_rec_prof_now() - t_run;
}

/* ---- Inicio --------------------------------------------------------------------- */

bool salsa64_rec_init(struct salsa64_rec *rec, struct salsa64_cpu *cpu, void *code, size_t bytes)
{
#if !REC_NATIVE
	(void)rec, (void)cpu, (void)code, (void)bytes;
	return false;
#else
	uint32_t size = cpu->rdram_size;
	if (bytes < 4u * (preamble_words() + SCRATCH_WORDS + 4 * MAX_BLOCK_WORDS) || ((uintptr_t)code & 127) ||
	    size < (1u << 20) || (size & (size - 1)) || size > SALSA64_RDRAM_MAX)
		return false;

	/* Los ganchos de depuración y el perfil los pone quien llama antes de iniciar. */
	uint32_t opts = rec->opts;
	void (*after_exit)(struct salsa64_rec *, uint64_t, void *) = rec->after_exit;
	void (*on_compile)(struct salsa64_rec *, uint64_t, uint32_t, uint32_t, bool) = rec->on_compile;
	void *after_exit_ctx = rec->after_exit_ctx;
	struct salsa64_rec_prof *prof = rec->prof;
	memset(rec, 0, sizeof *rec);
	rec->opts = opts;
	rec->after_exit = after_exit;
	rec->after_exit_ctx = after_exit_ctx;
	rec->on_compile = on_compile;
	rec->prof = prof;
	rec->cpu = cpu;
	rec->code = code;
	rec->code_words = (uint32_t)(bytes / 4 < UINT32_MAX ? bytes / 4 : UINT32_MAX);
	rec->scratch = rec->code_words - SCRATCH_WORDS;
	rec->rdram_size = size;
	while ((1u << rec->rdram_shift) < size)
		rec->rdram_shift++;

	uint64_t toc, my_toc, t;
	__asm__("mr %0,2" : "=r"(my_toc));
	rec->h_load = fn_entry((const void *)rec_h_load, &toc);
	bool ok = toc == my_toc;
	rec->h_store = fn_entry((const void *)rec_h_store, &t);
	ok &= t == my_toc;
	rec->h_exec = fn_entry((const void *)rec_h_exec, &t);
	ok &= t == my_toc;
	rec->h_exception = fn_entry((const void *)rec_h_exception, &t);
	ok &= t == my_toc;
	rec->h_tlbw = fn_entry((const void *)rec_h_tlbw, &t);
	ok &= t == my_toc;
	rec->h_eret = fn_entry((const void *)rec_h_eret, &t);
	ok &= t == my_toc;
	if (!ok)
		return false;  /* el código generado no cambia de TOC */

	/* Tramos de la caché: el espacio de bloques entre el preámbulo y la zona
	 * temporal, en partes iguales (múltiplos de 128 bytes). */
	rec->seg_words = ((rec->scratch - preamble_words()) / SALSA64_REC_SEGMENTS) & ~31u;
	rec->calls_near = salsa64_rec_calls_near_at(rec, 0);  /* el preámbulo y sus rutinas */

	uint32_t n = emit_preamble(rec);
	if (n > preamble_words())
		return false;
	salsa64_rec_sync(rec->code, n * 4);
	rec->enter_opd[0] = (uintptr_t)code;
	rec->enter_opd[1] = my_toc;
	rec->enter_opd[2] = 0;

	seg_reset(rec);
	rec->next_block = 1;
	rec->fr = cpu->status & SALSA64_ST_FR;
	salsa64_rec_x_init(rec);
	cpu->rec = rec;
	cpu->code_flags = rec->flags;
	return true;
#endif
}

void salsa64_rec_detach(struct salsa64_rec *rec)
{
	if (rec->cpu && rec->cpu->rec == rec) {
		rec->cpu->rec = NULL;
		rec->cpu->code_flags = NULL;
	}
}
