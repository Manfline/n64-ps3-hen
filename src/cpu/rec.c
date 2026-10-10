/*
 * Salsa64 — recompilador: despachador, caché de bloques, enlaces e
 * invalidación. La traducción está en rec_emit.c; diseño en docs/rec-ppe.md.
 *
 * El despachador reemplaza al bucle del intérprete (salsa64_cpu_run):
 * atiende los eventos, decide si el bloque en pc puede correr en nativo
 * (modo kernel, código en la RDRAM o en la ROM, sin interrupción ni delay
 * slot pendientes, y que quepa entero antes del próximo evento) y, si no,
 * deja que el intérprete avance hasta el final del bloque básico. El
 * resultado es el mismo que con el intérprete solo, ciclo por ciclo.
 *
 * Código fuera de KSEG0 (KSEG1, o mapeado por la TLB): la clave del bloque
 * es la dirección virtual y la física (xhash). La traducción de la TLB se
 * guarda por página de 4 KB en vtlb[]; el bloque mapeado la compara al
 * entrar (también si llega por un enlace) y, si cambió, sale al
 * despachador. Escribir la TLB (o cambiar ASID o ERL) no toca los bloques:
 * el despachador vuelve a traducir las páginas de vtlb[] antes de correr
 * código y borra las que cambiaron (vt_sync).
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
#define FRAME   256   /* marco de la entrada: 112 de enlace y parámetros + r14–r31 */
#define SAVE    112
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
_Static_assert(offsetof(struct salsa64_rec, flags) ==
		       offsetof(struct salsa64_rec, vtlb) + SALSA64_REC_VT_PAGES * sizeof(uint32_t) &&
	       SALSA64_REC_VT_PAGES * sizeof(uint32_t) <= 32768,
	       "el código mapeado lee vtlb[] con un desplazamiento negativo desde flags (r28)");
_Static_assert(sizeof(struct salsa64_rec_xblock) == 8, "exit_jr indexa xb[] con << 3");
_Static_assert(SALSA64_REC_MAX_BLOCKS <= 0xFFFF, "ids de 16 bits en map, xhash y xpage");
_Static_assert(SALSA64_REC_XHASH == 16384, "exit_jr calcula la cabeza de xhash con rlwinm 31, 17, 30");

enum { SEG_NONE = 3 };  /* lo corre el intérprete (junto a SALSA64_REC_KSEG0…) */

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

/* ---- Llamadas desde el código generado ------------------------------------------ */

REC_UNUSED static uint64_t rec_h_load(struct salsa64_cpu *cpu, uint64_t va, unsigned kind)
{
	static const uint8_t align[] = { 0, 0, 1, 1, 3, 3, 7 };
	uint32_t pa;
	if (!vr4300_vaddr(cpu, va, align[kind], SALSA64_ACC_LOAD, &pa))
		return 0;
	switch (kind) {
	case SALSA64_REC_LB:  return salsa64_sx8(vr4300_read8(cpu, pa));
	case SALSA64_REC_LBU: return vr4300_read8(cpu, pa);
	case SALSA64_REC_LH:  return salsa64_sx16(vr4300_read16(cpu, pa));
	case SALSA64_REC_LHU: return vr4300_read16(cpu, pa);
	case SALSA64_REC_LW:  return salsa64_sx32(vr4300_read32(cpu, pa));
	case SALSA64_REC_LWU: return vr4300_read32(cpu, pa);
	default:              return vr4300_read64(cpu, pa);
	}
}

REC_UNUSED static void rec_h_store(struct salsa64_cpu *cpu, uint64_t va, uint64_t v, unsigned kind)
{
	static const uint8_t align[] = { 0, 1, 3, 7 };
	uint32_t pa;
	if (!vr4300_vaddr(cpu, va, align[kind], SALSA64_ACC_STORE, &pa))
		return;
	switch (kind) {
	case SALSA64_REC_SB: vr4300_write8(cpu, pa, v); return;
	case SALSA64_REC_SH: vr4300_write16(cpu, pa, v); return;
	case SALSA64_REC_SW: vr4300_write32(cpu, pa, (uint32_t)v); return;
	default:             vr4300_write64(cpu, pa, v); return;
	}
}

REC_UNUSED static void rec_h_exec(struct salsa64_cpu *cpu, uint32_t instr)
{
	salsa64_vr4300_exec_one(cpu, instr);
}

REC_UNUSED static void rec_h_exception(struct salsa64_cpu *cpu, unsigned code)
{
	salsa64_vr4300_exception(cpu, (enum salsa64_exc)code, 0);
}

/* Dirección de código de una función de C (descriptor ELFv1: entrada, TOC). */
REC_UNUSED static uint64_t fn_entry(const void *fn, uint64_t *toc)
{
	const uint64_t *d = fn;
	*toc = d[1];
	return d[0];
}

/* ---- Dónde está el código --------------------------------------------------------- */

/* Código compilable en la dirección física `pa`: RDRAM, o la ROM del
 * cartucho (solo lectura: nunca hay que invalidarla) salvo el ISViewer. */
static bool code_pa(const struct salsa64_rec *rec, uint32_t pa)
{
	const struct salsa64_bus *bus = rec->cpu->bus;
	if (pa < rec->rdram_size)
		return true;
	if (pa < SALSA64_CART_ROM_ADDR || pa >= SALSA64_CART_ROM_END)
		return false;
	return !bus->isv || pa - SALSA64_ISV_ADDR >= SALSA64_ISV_SIZE;
}

uint32_t salsa64_rec_fetch(const struct salsa64_rec *rec, uint32_t pa)
{
	if (pa < rec->rdram_size)
		return salsa64_rd32(rec->cpu->rdram + pa);
	return salsa64_bus_read32(rec->cpu->bus, pa);  /* ROM: lectura sin efectos */
}

uint32_t salsa64_rec_code_end(const struct salsa64_rec *rec, uint32_t pa, unsigned seg)
{
	/* El ISViewer empieza en una página: el límite de página lo deja fuera. */
	uint32_t end = pa < rec->rdram_size ? rec->rdram_size : SALSA64_CART_ROM_END;
	if (seg != SALSA64_REC_KSEG0) {
		uint32_t page = (pa | ((1u << SALSA64_REC_PAGE_SHIFT) - 1)) + 1;
		if (page < end)
			end = page;
	}
	return end;
}

/* ---- TLB del recompilador (vtlb) -------------------------------------------------- */

static void vt_set(struct salsa64_rec *rec, uint64_t va, uint32_t pa)
{
	unsigned s = salsa64_rec_vt_slot(va);
	if (!rec->vtlb[s])
		rec->vt_live[rec->vt_nlive++] = (uint16_t)s;
	rec->vtlb[s] = salsa64_rec_vt_value(va, pa);
}

/* Física de la instrucción mapeada en `va`: la de vtlb[] o, si la página no
 * está, la de la TLB del CPU (y queda en vtlb[]). */
static bool vt_translate(struct salsa64_rec *rec, uint64_t va, uint32_t *pa)
{
	uint32_t v = rec->vtlb[salsa64_rec_vt_slot(va)];
	if (v && (v >> 17 & 0xFF) == (uint32_t)va >> 24) {
		*pa = (v & 0x1FFFF) << SALSA64_REC_PAGE_SHIFT | ((uint32_t)va & ((1u << SALSA64_REC_PAGE_SHIFT) - 1));
		return true;
	}
	rec->stats.vt_probes++;
	if (!salsa64_vr4300_fetch_probe(rec->cpu, va, pa) || !code_pa(rec, *pa))
		return false;
	vt_set(rec, va, *pa);
	return true;
}

/* Antes de correr código generado: si la TLB del CPU cambió (TLBWI, TLBWR,
 * reset) o cambiaron el ASID o ERL, cada página de vtlb[] se vuelve a
 * traducir y se borran las que ya no dan lo mismo. Sus bloques se quedan:
 * al entrar ven que su página no está y salen al despachador, que la
 * traduce de nuevo. Todo cambio de la TLB pasa por C y vuelve aquí antes
 * del siguiente bloque (COP0 termina el bloque; ERET y las excepciones
 * salen con redirect). */
static void vt_sync(struct salsa64_rec *rec)
{
	struct salsa64_cpu *cpu = rec->cpu;
	uint8_t asid = (uint8_t)cpu->entry_hi;
	bool erl = (cpu->status & SALSA64_ST_ERL) != 0;
	if (cpu->tlb_writes == rec->vt_tlb_writes && asid == rec->vt_asid && erl == rec->vt_erl)
		return;
	rec->vt_tlb_writes = cpu->tlb_writes;
	rec->vt_asid = asid;
	rec->vt_erl = erl;
	uint32_t keep = 0;
	for (uint32_t k = 0; k < rec->vt_nlive; k++) {
		unsigned s = rec->vt_live[k];
		uint32_t v = rec->vtlb[s], pa;
		uint64_t va = (uint64_t)(int64_t)(int32_t)((v >> 17 & 0xFF) << 24 | (uint32_t)s << SALSA64_REC_PAGE_SHIFT);
		rec->stats.vt_probes++;
		if (salsa64_vr4300_fetch_probe(cpu, va, &pa) && salsa64_rec_vt_value(va, pa) == v)
			rec->vt_live[keep++] = (uint16_t)s;
		else
			rec->vtlb[s] = 0;
	}
	rec->vt_nlive = keep;
}

/* Segmento y dirección física del código en `pc` (modo kernel), o SEG_NONE
 * si no se compila (lo corre el intérprete, que lanza lo que haga falta). */
static unsigned code_seg(struct salsa64_rec *rec, uint64_t pc, uint32_t *pa)
{
	if (pc & 3)
		return SEG_NONE;
	if ((pc >> 29) == 0x7FFFFFFFCull) {  /* KSEG0 */
		*pa = (uint32_t)pc & 0x1FFFFFFF;
		return *pa < rec->rdram_size ? SALSA64_REC_KSEG0 : SEG_NONE;
	}
	if ((int64_t)pc != (int32_t)pc)
		return SEG_NONE;
	if ((pc >> 29) == 0x7FFFFFFFDull) {  /* KSEG1 */
		*pa = (uint32_t)pc & 0x1FFFFFFF;
		return code_pa(rec, *pa) ? SALSA64_REC_KSEG1 : SEG_NONE;
	}
	return vt_translate(rec, pc, pa) ? SALSA64_REC_MAPPED : SEG_NONE;
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

	uint32_t common = n + 12;
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
	while (n < common)
		w[n++] = PPC_NOP;

	w[n++] = ppc_std(29, OFF(cycles), 31);
	w[n++] = ppc_stw(4, OFF(rec_kind), 31);
	if (rec->opts & SALSA64_REC_OPT_PIN)
		for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++)
			w[n++] = ppc_std(SALSA64_REC_PIN_PPC + k, OFF(gpr) + 8 * salsa64_rec_pin[k], 31);
	for (unsigned r = 14; r <= 31; r++)
		w[n++] = ppc_ld(r, SAVE + 8 * (int32_t)(r - 14), 1);
	w[n++] = ppc_addi(1, 1, FRAME);
	w[n++] = ppc_ld(0, 16, 1);
	w[n++] = ppc_mtlr(0);
	w[n++] = ppc_lwz(12, 8, 1);
	w[n++] = ppc_mtcrf(0x38, 12);  /* cr2–cr4, los no volátiles */
	w[n++] = PPC_BLR;

	/* exit_jr: KSEG0, alineada y dentro de la RDRAM → map → blocks → código.
	 * Fuera de KSEG0 (xjr): extensión de signo de 32 bits → la primera de
	 * su cabeza de xhash, si es la misma vpc → blocks → código. Si es un
	 * bloque mapeado con otra física, su entrada lo nota y sale. */
	const unsigned ne = PPC_BO_FALSE_UNLIKELY, eq = PPC_BO_TRUE_UNLIKELY, EQ = PPC_BI(0, PPC_EQ);
	rec->exit_jr = n;
	w[n++] = ppc_sradi(11, 3, 29);
	w[n++] = ppc_cmpdi(0, 11, -4);
	uint32_t to_xjr = n++;  /* bne xjr, se escribe abajo */
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

	w[to_xjr] = ppc_bc(ne, EQ, (int32_t)(n - to_xjr) * 4);
	w[n++] = ppc_extsw(11, 3);
	w[n++] = ppc_cmpd(0, 11, 3);
	n = pre_bc(w, n, ne, EQ, rec->exit_jump);
	w[n++] = ppc_rlwinm(12, 3, 31, 17, 30);  /* ((vpc >> 2) & (XHASH - 1)) * 2 */
	n = pre_li64(w, n, 11, (uintptr_t)rec->xhash);
	w[n++] = ppc_lhzx(12, 11, 12);
	w[n++] = ppc_cmpwi(0, 12, 0);
	n = pre_bc(w, n, eq, EQ, rec->exit_jump);
	w[n++] = ppc_sldi(0, 12, 3);
	n = pre_li64(w, n, 11, (uintptr_t)rec->xb);
	w[n++] = ppc_lwzx(0, 11, 0);
	w[n++] = ppc_cmplw(0, 0, 3);
	n = pre_bc(w, n, ne, EQ, rec->exit_jump);
	w[n] = ppc_b((int32_t)(id_ok - n) * 4);
	n++;
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
	return 256;  /* emit_preamble usa menos; los bloques empiezan alineados */
}

void salsa64_rec_flush(struct salsa64_rec *rec)
{
	if (rec->map_dirty) {
		memset(rec->map, 0, (rec->rdram_size >> 2) * sizeof rec->map[0]);
		memset(rec->flags, 0, rec->rdram_size >> SALSA64_REC_FLAG_SHIFT);
		memset(rec->xhash, 0, sizeof rec->xhash);
		memset(rec->xpage, 0, sizeof rec->xpage);
		rec->xcount = 0;
		rec->map_dirty = false;
		rec->stats.flushes++;
	}
	rec->free_block = 0;
	rec->next_block = 1;
	rec->free_link = 0;
	rec->next_link = 0;
	rec->used = preamble_words();
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
		rec->code[lk->site] = PPC_NOP;
		salsa64_rec_sync(&rec->code[lk->site], 4);
		lk->next = rec->free_link;
		rec->free_link = l;
		l = next;
	}
	b->links = 0;
}

static unsigned xhash_slot(uint64_t vpc)
{
	return ((uint32_t)vpc >> 2) & (SALSA64_REC_XHASH - 1);
}

static bool kseg0_vpc(uint32_t vpc)
{
	return vpc >> 29 == 4;
}

static void kill_block(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_block *b = &rec->blocks[id];
	struct salsa64_rec_xblock *x = &rec->xb[id];
	if (kseg0_vpc(x->vpc)) {
		rec->map[b->pa >> 2] = 0;
	} else {
		uint16_t *p = &rec->xhash[xhash_slot(x->vpc)];
		while (*p != id)
			p = &rec->xb[*p].hnext;
		*p = x->hnext;
		if (b->pa < rec->rdram_size) {
			p = &rec->xpage[b->pa >> SALSA64_REC_PAGE_SHIFT];
			while (*p != id)
				p = &rec->xb[*p].pnext;
			*p = x->pnext;
		}
		rec->xcount--;
	}
	if (b->pa < rec->rdram_size)
		flags_add(rec, b->pa, b->pa + b->bytes, -1);
	unlink_all(rec, b);
	b->code = 0;
	b->links = rec->free_block;
	rec->free_block = (uint16_t)id;
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
	/* Fuera de KSEG0: ningún bloque cruza una página de 4 KB. */
	for (uint32_t p = pa >> SALSA64_REC_PAGE_SHIFT; rec->xcount && p <= (end - 1) >> SALSA64_REC_PAGE_SHIFT; p++) {
		for (unsigned id = rec->xpage[p], next; id; id = next) {
			next = rec->xb[id].pnext;
			const struct salsa64_rec_block *b = &rec->blocks[id];
			if (b->pa < end && b->pa + b->bytes > pa)
				kill_block(rec, id);
		}
	}
}

void salsa64_rec_code_written(struct salsa64_rec *rec, uint32_t pa, uint32_t len)
{
	salsa64_rec_invalidate(rec, pa, len);
}

static bool rec_space(struct salsa64_rec *rec)
{
	return rec->used + MAX_BLOCK_WORDS <= rec->scratch &&
	       (rec->free_block || rec->next_block <= SALSA64_REC_MAX_BLOCKS) &&
	       (rec->free_link || rec->next_link < SALSA64_REC_MAX_LINKS);
}

static unsigned new_block(struct salsa64_rec *rec, uint64_t pc, uint32_t pa, unsigned seg)
{
	if (!rec_space(rec))
		salsa64_rec_flush(rec);
	unsigned n = 0;
	uint32_t bytes = 4;
	uint32_t code = salsa64_rec_compile(rec, pc, pa, seg, SALSA64_REC_MAX_INSNS, false, &n, &bytes);
	unsigned id;
	if (rec->free_block) {
		id = rec->free_block;
		rec->free_block = (uint16_t)rec->blocks[id].links;
	} else {
		id = rec->next_block++;
	}
	struct salsa64_rec_block *b = &rec->blocks[id];
	b->pa = pa;
	b->code = code;
	b->n = (uint16_t)n;
	b->bytes = (uint16_t)(code ? bytes : 4);
	b->links = 0;
	struct salsa64_rec_xblock *x = &rec->xb[id];
	x->vpc = (uint32_t)pc;
	if (seg == SALSA64_REC_KSEG0) {
		rec->map[pa >> 2] = (uint16_t)id;
	} else {
		uint16_t *h = &rec->xhash[xhash_slot(pc)];
		x->hnext = *h;
		*h = (uint16_t)id;
		x->pnext = 0;
		if (pa < rec->rdram_size) {
			uint16_t *p = &rec->xpage[pa >> SALSA64_REC_PAGE_SHIFT];
			x->pnext = *p;
			*p = (uint16_t)id;
		}
		rec->xcount++;
		rec->stats.compiled_x += code != 0;
	}
	rec->map_dirty = true;
	if (pa < rec->rdram_size)
		flags_add(rec, b->pa, b->pa + b->bytes, +1);
	if (code) {
		rec->stats.compiled++;
	} else {
		rec->stats.interp_only++;
	}
	return id;
}

/* El bloque fuera de KSEG0 de `pc` con la instrucción en `pa`. Lo pasa al
 * frente de su cabeza de xhash: exit_jr solo mira la primera.
 *
 * Los de la misma vpc con otra física (la TLB la remapeó) se desenlazan: si
 * no, cada enlace que llega a ellos saldría en su entrada todas las veces;
 * así vuelven a C una vez y se enlazan con el bloque que vale ahora. Un
 * bloque con enlaces pasó por aquí al frente después de enlazarse, así que
 * se lo encuentra antes que al de la otra física. */
static unsigned xfind(struct salsa64_rec *rec, uint64_t pc, uint32_t pa)
{
	uint16_t *head = &rec->xhash[xhash_slot(pc)], *p = head;
	for (unsigned id = *p; id; p = &rec->xb[id].hnext, id = *p) {
		if (rec->xb[id].vpc != (uint32_t)pc)
			continue;
		if (rec->blocks[id].pa != pa) {
			if (rec->blocks[id].links)
				unlink_all(rec, &rec->blocks[id]);
			continue;
		}
		if (p != head) {
			*p = rec->xb[id].hnext;
			rec->xb[id].hnext = *head;
			*head = (uint16_t)id;
		}
		return id;
	}
	return 0;
}

/* Bloques fuera de KSEG0 que contienen `vpc` (en su página). */
static void kill_x_at(struct salsa64_rec *rec, uint64_t vpc)
{
	uint32_t v = (uint32_t)vpc, page = v & ~((1u << SALSA64_REC_PAGE_SHIFT) - 1);
	for (uint32_t k = 0; k < SALSA64_REC_MAX_INSNS && 4 * k <= v - page; k++) {
		uint32_t start = v - 4 * k;
		for (unsigned id = rec->xhash[xhash_slot(start)], next; id; id = next) {
			next = rec->xb[id].hnext;
			if (rec->xb[id].vpc == start && start + rec->blocks[id].bytes > v)
				kill_block(rec, id);
		}
	}
}

/* Parchea el nop de `site` con un `b` al bloque. */
static void link_block(struct salsa64_rec *rec, uint32_t site, unsigned id)
{
	struct salsa64_rec_block *b = &rec->blocks[id];
	int64_t disp = ((int64_t)b->code - site) * 4;
	if (site >= rec->used || rec->code[site] != PPC_NOP || !ppc_fits_b(disp))
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
	rec->links[l - 1].next = b->links;
	b->links = l;
	rec->code[site] = ppc_b((int32_t)disp);
	salsa64_rec_sync(&rec->code[site], 4);
	rec->stats.linked++;
}

/* ---- Despachador ----------------------------------------------------------------- */

/* El intérprete avanza hasta terminar un delay slot, tomar una excepción o
 * (en modo estricto) una sola instrucción. Atiende los eventos que lleguen. */
static void rec_interp(struct salsa64_rec *rec, bool to_boundary)
{
	struct salsa64_cpu *cpu = rec->cpu;
	uint32_t exc = cpu->exc_count;
	unsigned k = 0;
	do {
		bool ds = cpu->branch != 0;
		salsa64_cpu_step(cpu);
		k++;
		if (!to_boundary || cpu->exc_count != exc || (ds && !cpu->branch))
			break;
	} while (k < 4096 && cpu->running);
	rec->stats.interp_steps += k;
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
		kill_x_at(rec, bpc);
}

void salsa64_rec_run(struct salsa64_rec *rec)
{
	struct salsa64_cpu *cpu = rec->cpu;
	struct salsa64_sched *s = cpu->sched;
	bool to_boundary = false;
	uint32_t site = 0, site_gen = 0;

	cpu->running = 1;
	while (cpu->running) {
		if (cpu->cycles >= s->next) {
			salsa64_sched_dispatch(s, cpu->cycles);
			continue;
		}
		uint64_t pc = cpu->pc;
		uint32_t pa = 0;
		unsigned seg = SEG_NONE;
		if (!to_boundary && !cpu->irq_pending && !cpu->branch && cpu->mode == SALSA64_MODE_KERNEL) {
			vt_sync(rec);  /* antes de correr cualquier bloque: KSEG0 enlaza con mapeados */
			seg = code_seg(rec, pc, &pa);
		}
		if (seg == SEG_NONE) {
			rec_interp(rec, !rec->strict);
			to_boundary = false;
			site = 0;
			continue;
		}
		if ((cpu->status & SALSA64_ST_FR) != rec->fr) {
			salsa64_rec_flush(rec);
			rec->fr = cpu->status & SALSA64_ST_FR;
		}

		unsigned id = seg == SALSA64_REC_KSEG0 ? rec->map[pa >> 2] : xfind(rec, pc, pa);
		if (!id)
			id = new_block(rec, pc, pa, seg);
		const struct salsa64_rec_block *b = &rec->blocks[id];
		uint32_t entry = b->code;
		if (!entry) {
			rec_interp(rec, !rec->strict);
			site = 0;
			continue;
		}
		if (cpu->cycles + b->n > s->next) {
			/* No cabe antes del evento. Normalmente lo termina el
			 * intérprete; en modo estricto se compila recortado. */
			unsigned n = 0;
			uint32_t bytes = 0;
			if (rec->strict)
				entry = salsa64_rec_compile(rec, pc, pa, seg, (unsigned)(s->next - cpu->cycles), true, &n,
							    &bytes);
			else
				entry = 0;
			if (!entry) {
				rec_interp(rec, !rec->strict);
				site = 0;
				continue;
			}
			rec->stats.temp_compiled++;
		} else if (site && site_gen == rec->generation && !rec->no_link) {
			link_block(rec, site, id);
		}
		site = 0;

		uint32_t exc = cpu->exc_count;
		uint64_t cycles = cpu->cycles;
		cpu->redirect = 0;  /* salsa64_cpu_set_pc lo deja en 1 para el intérprete; aquí pc ya está leído */
		cpu->rec_site = 0;
		cpu->rec_abort = 0;
		rec->stats.entries++;
		uint64_t next = rec_enter(rec, entry);
		if (cpu->redirect) {
			/* Excepción o ERET: pc, next_pc y branch ya están puestos. */
			cpu->redirect = 0;
			to_boundary = cpu->exc_count == exc && !rec->strict;
		} else if (cpu->rec_kind == SALSA64_REC_EXIT_IDLE) {
			rec_idle_exit(rec, next);  /* deja el delay slot pendiente: lo corre el intérprete */
		} else {
			cpu->pc = next;
			cpu->next_pc = next + 4;
			cpu->branch = 0;
			if (cpu->rec_kind == SALSA64_REC_EXIT_LINK && cpu->rec_site) {
				site = (uint32_t)((cpu->rec_site - (uintptr_t)rec->code) / 4);
				site_gen = rec->generation;
			} else if (cpu->rec_kind == SALSA64_REC_EXIT_MID) {
				/* En modo estricto solo si el bloque no avanzó (salió en
				 * la entrada, p. ej. con COP1 inusable): una instrucción. */
				to_boundary = !rec->strict || cpu->cycles == cycles;
			}
		}
		if (rec->after_exit)
			rec->after_exit(rec, pc, rec->after_exit_ctx);
	}
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

	/* Los ganchos de depuración los pone quien llama antes de iniciar. */
	uint32_t opts = rec->opts;
	void (*after_exit)(struct salsa64_rec *, uint64_t, void *) = rec->after_exit;
	void (*on_compile)(struct salsa64_rec *, uint64_t, uint32_t, uint32_t, bool) = rec->on_compile;
	void *after_exit_ctx = rec->after_exit_ctx;
	memset(rec, 0, sizeof *rec);
	rec->opts = opts;
	rec->after_exit = after_exit;
	rec->after_exit_ctx = after_exit_ctx;
	rec->on_compile = on_compile;
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
	if (!ok)
		return false;  /* el código generado no cambia de TOC */

	uint64_t lo = (uintptr_t)code, hi = lo + bytes, h[4] = { rec->h_load, rec->h_store, rec->h_exec, rec->h_exception };
	rec->calls_near = true;
	for (unsigned k = 0; k < 4; k++) {
		uint64_t far = h[k] < lo ? hi - h[k] : h[k] > hi ? h[k] - lo : hi - lo;
		if (far >= CALL_REACH)
			rec->calls_near = false;
	}

	uint32_t n = emit_preamble(rec);
	if (n > preamble_words())
		return false;
	salsa64_rec_sync(rec->code, n * 4);
	rec->enter_opd[0] = (uintptr_t)code;
	rec->enter_opd[1] = my_toc;
	rec->enter_opd[2] = 0;

	rec->used = preamble_words();
	rec->next_block = 1;
	rec->fr = cpu->status & SALSA64_ST_FR;
	rec->vt_tlb_writes = cpu->tlb_writes;
	rec->vt_asid = (uint8_t)cpu->entry_hi;
	rec->vt_erl = (cpu->status & SALSA64_ST_ERL) != 0;
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
