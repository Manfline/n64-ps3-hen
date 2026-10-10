/*
 * Salsa64 — recompilador: despachador, caché de bloques, enlaces e
 * invalidación. La traducción está en rec_emit.c; diseño en docs/rec-ppe.md.
 *
 * El despachador reemplaza al bucle del intérprete (salsa64_cpu_run):
 * atiende los eventos, decide si el bloque en pc puede correr en nativo
 * (modo kernel, KSEG0, sin interrupción ni delay slot pendientes, y que
 * quepa entero antes del próximo evento) y, si no, deja que el intérprete
 * avance hasta el final del bloque básico. El resultado es el mismo que con
 * el intérprete solo, ciclo por ciclo.
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

/* ---- Entrada y salidas (al principio de la caché) -------------------------------- */

/*
 * enter(cpu, código): guarda LR, CR y r14–r31 en un marco propio, carga los
 * registros fijos y salta al bloque. Las salidas dejan en r3 la dirección
 * MIPS donde sigue la ejecución:
 *   exit_link: viene de `bl` en un enlace; el nop parcheable está en LR - 16.
 *   exit_mid, exit_jump: tipo de salida en r4.
 *   común: guarda cycles y el tipo, restaura y vuelve a C.
 */
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
	w[n++] = ppc_mtctr(4);
	w[n++] = PPC_BCTR;

	uint32_t common = n + 10;
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
	while (n < common)
		w[n++] = PPC_NOP;

	w[n++] = ppc_std(29, OFF(cycles), 31);
	w[n++] = ppc_stw(4, OFF(rec_kind), 31);
	for (unsigned r = 14; r <= 31; r++)
		w[n++] = ppc_ld(r, SAVE + 8 * (int32_t)(r - 14), 1);
	w[n++] = ppc_addi(1, 1, FRAME);
	w[n++] = ppc_ld(0, 16, 1);
	w[n++] = ppc_mtlr(0);
	w[n++] = ppc_lwz(12, 8, 1);
	w[n++] = ppc_mtcrf(0x38, 12);  /* cr2–cr4, los no volátiles */
	w[n++] = PPC_BLR;
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
	return 128;  /* emit_preamble usa menos; los bloques empiezan alineados */
}

void salsa64_rec_flush(struct salsa64_rec *rec)
{
	if (rec->map_dirty) {
		memset(rec->map, 0, (rec->rdram_size >> 2) * sizeof rec->map[0]);
		memset(rec->flags, 0, rec->rdram_size >> SALSA64_REC_FLAG_SHIFT);
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

static void kill_block(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_block *b = &rec->blocks[id];
	rec->map[b->pa >> 2] = 0;
	flags_add(rec, b->pa, b->end, -1);
	unlink_all(rec, b);
	b->code = 0;
	b->next_free = rec->free_block;
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
			if (id && rec->blocks[id].end > pa)
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

static unsigned new_block(struct salsa64_rec *rec, uint64_t pc, uint32_t pa)
{
	if (!rec_space(rec))
		salsa64_rec_flush(rec);
	unsigned n = 0;
	uint32_t bytes = 4;
	uint32_t code = salsa64_rec_compile(rec, pc, SALSA64_REC_MAX_INSNS, false, &n, &bytes);
	unsigned id;
	if (rec->free_block) {
		id = rec->free_block;
		rec->free_block = rec->blocks[id].next_free;
	} else {
		id = rec->next_block++;
	}
	struct salsa64_rec_block *b = &rec->blocks[id];
	b->pa = pa;
	b->code = code;
	b->n = (uint16_t)n;
	b->end = pa + (code ? bytes : 4);
	b->links = 0;
	rec->map[pa >> 2] = (uint16_t)id;
	rec->map_dirty = true;
	flags_add(rec, b->pa, b->end, +1);
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

static bool native_pc(const struct salsa64_rec *rec, uint64_t pc)
{
	/* KSEG0 (0xFFFFFFFF80000000–9FFFFFFF), alineada y dentro de la RDRAM. */
	return (pc >> 29) == 0x7FFFFFFFCull && !(pc & 3) && ((uint32_t)pc & 0x1FFFFFFF) < rec->rdram_size;
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
		if (to_boundary || cpu->irq_pending || cpu->branch || cpu->mode != SALSA64_MODE_KERNEL ||
		    !native_pc(rec, pc)) {
			rec_interp(rec, !rec->strict);
			to_boundary = false;
			site = 0;
			continue;
		}
		if ((cpu->status & SALSA64_ST_FR) != rec->fr) {
			salsa64_rec_flush(rec);
			rec->fr = cpu->status & SALSA64_ST_FR;
		}

		uint32_t pa = (uint32_t)pc & 0x1FFFFFFF;
		unsigned id = rec->map[pa >> 2];
		if (!id)
			id = new_block(rec, pc, pa);
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
				entry = salsa64_rec_compile(rec, pc, (unsigned)(s->next - cpu->cycles), true, &n, &bytes);
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
	void (*after_exit)(struct salsa64_rec *, uint64_t, void *) = rec->after_exit;
	void (*on_compile)(struct salsa64_rec *, uint64_t, uint32_t, uint32_t, bool) = rec->on_compile;
	void *after_exit_ctx = rec->after_exit_ctx;
	memset(rec, 0, sizeof *rec);
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
	salsa64_rec_sync(rec->code, n * 4);
	rec->enter_opd[0] = (uintptr_t)code;
	rec->enter_opd[1] = my_toc;
	rec->enter_opd[2] = 0;

	rec->used = preamble_words();
	rec->next_block = 1;
	rec->fr = cpu->status & SALSA64_ST_FR;
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
