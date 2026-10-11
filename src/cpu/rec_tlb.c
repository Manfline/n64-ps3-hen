/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — recompilador: código fuera de KSEG0 (KSEG1 y mapeado por la
 * TLB). Ver rec_tlb.h y docs/rec-ppe.md, «Código fuera de KSEG0».
 */
#include "cpu/rec_tlb.h"

#include <stddef.h>
#include <string.h>

#include "cpu/rec.h"
#include "cpu/vr4300_internal.h"

_Static_assert(offsetof(struct salsa64_rec, flags) ==
		       offsetof(struct salsa64_rec, x) + offsetof(struct salsa64_rec_x, vtlb) +
			       SALSA64_REC_VT_PAGES * sizeof(uint32_t) &&
	       SALSA64_REC_VT_PAGES * sizeof(uint32_t) <= 32768,
	       "el código mapeado lee vtlb[] con un desplazamiento negativo desde flags (r28)");
_Static_assert(sizeof(struct salsa64_rec_xblock) == 8, "exit_jr indexa xb[] con << 3");
_Static_assert(SALSA64_REC_MAX_BLOCKS + 1 <= SALSA64_REC_X_IDS, "ids de 16 bits en xb[], xhash y xpage");
_Static_assert(SALSA64_REC_XHASH == 16384, "exit_jr calcula la cabeza de xhash con rlwinm 31, 17, 30");

#define PAGE_MASK ((1u << SALSA64_REC_PAGE_SHIFT) - 1)

/* ---- Dónde está el código --------------------------------------------------------- */

/* Código compilable en la dirección física `pa`: RDRAM, o la ROM del
 * cartucho (solo lectura) salvo el ISViewer, que se escribe. */
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
	if (seg != SALSA64_REC_KSEG0 && (pa | PAGE_MASK) + 1 < end)
		end = (pa | PAGE_MASK) + 1;
	return end;
}

/* ---- TLB del recompilador (vtlb) -------------------------------------------------- */

static void vt_set(struct salsa64_rec *rec, uint64_t va, uint32_t pa)
{
	struct salsa64_rec_x *x = &rec->x;
	unsigned s = salsa64_rec_vt_slot(va);
	if (!x->vtlb[s])
		x->live[x->nlive++] = (uint16_t)s;
	x->vtlb[s] = salsa64_rec_vt_value(va, pa);
}

/* Física de la instrucción mapeada en `va`: la de vtlb[] o, si la página no
 * está, la de la TLB del CPU (y queda en vtlb[]). */
static bool vt_translate(struct salsa64_rec *rec, uint64_t va, uint32_t *pa)
{
	uint32_t v = rec->x.vtlb[salsa64_rec_vt_slot(va)];
	if (v && (v >> 17 & 0xFF) == (uint32_t)va >> 24) {
		*pa = (v & 0x1FFFF) << SALSA64_REC_PAGE_SHIFT | ((uint32_t)va & PAGE_MASK);
		return true;
	}
	rec->stats.vt_probes++;
	if (!salsa64_vr4300_fetch_probe(rec->cpu, va, pa) || !code_pa(rec, *pa))
		return false;
	vt_set(rec, va, *pa);
	return true;
}

/* Si la TLB del CPU cambió (TLBWI, TLBWR, reset) o cambiaron el ASID o ERL,
 * cada página de vtlb[] se vuelve a traducir y se borran las que ya no dan
 * lo mismo. Sus bloques se quedan: al entrar ven que su página no está y
 * salen al despachador, que la traduce de nuevo. Todo cambio de la TLB pasa
 * por C y vuelve al despachador antes del siguiente bloque (COP0 termina el
 * bloque; ERET y las excepciones salen con redirect). */
void salsa64_rec_x_sync(struct salsa64_rec *rec)
{
	struct salsa64_cpu *cpu = rec->cpu;
	struct salsa64_rec_x *x = &rec->x;
	uint8_t asid = (uint8_t)cpu->entry_hi;
	bool erl = (cpu->status & SALSA64_ST_ERL) != 0;
	if (cpu->tlb_writes == x->tlb_writes && asid == x->asid && erl == x->erl)
		return;
	x->tlb_writes = cpu->tlb_writes;
	x->asid = asid;
	x->erl = erl;
	uint32_t keep = 0;
	for (uint32_t k = 0; k < x->nlive; k++) {
		unsigned s = x->live[k];
		uint32_t v = x->vtlb[s], pa;
		uint64_t va = (uint64_t)(int64_t)(int32_t)((v >> 17 & 0xFF) << 24 | (uint32_t)s << SALSA64_REC_PAGE_SHIFT);
		rec->stats.vt_probes++;
		if (salsa64_vr4300_fetch_probe(cpu, va, &pa) && salsa64_rec_vt_value(va, pa) == v)
			x->live[keep++] = (uint16_t)s;
		else
			x->vtlb[s] = 0;
	}
	x->nlive = keep;
}

unsigned salsa64_rec_seg(struct salsa64_rec *rec, uint64_t pc, uint32_t *pa)
{
	if (pc & 3)
		return SALSA64_REC_SEG_NONE;
	if ((pc >> 29) == 0x7FFFFFFFCull) {  /* KSEG0 */
		*pa = (uint32_t)pc & 0x1FFFFFFF;
		return *pa < rec->rdram_size ? SALSA64_REC_KSEG0 : SALSA64_REC_SEG_NONE;
	}
	if ((int64_t)pc != (int32_t)pc)
		return SALSA64_REC_SEG_NONE;
	if ((pc >> 29) == 0x7FFFFFFFDull) {  /* KSEG1 */
		*pa = (uint32_t)pc & 0x1FFFFFFF;
		return code_pa(rec, *pa) ? SALSA64_REC_KSEG1 : SALSA64_REC_SEG_NONE;
	}
	return vt_translate(rec, pc, pa) ? SALSA64_REC_MAPPED : SALSA64_REC_SEG_NONE;
}

/* ---- Bloques fuera de KSEG0 -------------------------------------------------------- */

static unsigned xhash_slot(uint64_t vpc)
{
	return ((uint32_t)vpc >> 2) & (SALSA64_REC_XHASH - 1);
}

void salsa64_rec_x_init(struct salsa64_rec *rec)
{
	struct salsa64_cpu *cpu = rec->cpu;
	rec->x.tlb_writes = cpu->tlb_writes;
	rec->x.asid = (uint8_t)cpu->entry_hi;
	rec->x.erl = (cpu->status & SALSA64_ST_ERL) != 0;
}

void salsa64_rec_x_flush(struct salsa64_rec *rec)
{
	memset(rec->x.xhash, 0, sizeof rec->x.xhash);
	memset(rec->x.xpage, 0, sizeof rec->x.xpage);
	rec->x.count = 0;
}

void salsa64_rec_x_add(struct salsa64_rec *rec, unsigned id, uint64_t pc, uint32_t pa, unsigned seg, bool code)
{
	struct salsa64_rec_x *x = &rec->x;
	struct salsa64_rec_xblock *b = &x->xb[id];
	b->vpc = (uint32_t)pc;
	if (seg == SALSA64_REC_KSEG0)
		return;
	uint16_t *h = &x->xhash[xhash_slot(pc)];
	b->hnext = *h;
	*h = (uint16_t)id;
	b->pnext = 0;
	if (pa < rec->rdram_size) {
		uint16_t *p = &x->xpage[pa >> SALSA64_REC_PAGE_SHIFT];
		b->pnext = *p;
		*p = (uint16_t)id;
	}
	x->count++;
	rec->stats.compiled_x += code;
}

bool salsa64_rec_x_remove(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_x *x = &rec->x;
	struct salsa64_rec_xblock *b = &x->xb[id];
	if (b->vpc >> 29 == 4)
		return false;  /* KSEG0 */
	uint16_t *p = &x->xhash[xhash_slot(b->vpc)];
	while (*p != id)
		p = &x->xb[*p].hnext;
	*p = b->hnext;
	uint32_t pa = rec->blocks[id].pa;
	if (pa < rec->rdram_size) {
		p = &x->xpage[pa >> SALSA64_REC_PAGE_SHIFT];
		while (*p != id)
			p = &x->xb[*p].pnext;
		*p = b->pnext;
	}
	x->count--;
	return true;
}

/* Lo pasa al frente de su cabeza de xhash: exit_jr solo mira la primera.
 *
 * Los de la misma vpc con otra física (la TLB la remapeó) se desenlazan: si
 * no, cada enlace que llega a ellos saldría en su entrada todas las veces;
 * así vuelven a C una vez y se enlazan con el bloque que vale ahora. Un
 * bloque con enlaces pasó por aquí al frente después de enlazarse, así que
 * se lo encuentra antes que al de la otra física. */
unsigned salsa64_rec_x_find(struct salsa64_rec *rec, uint64_t pc, uint32_t pa)
{
	struct salsa64_rec_x *x = &rec->x;
	uint16_t *head = &x->xhash[xhash_slot(pc)], *p = head;
	for (unsigned id = *p; id; p = &x->xb[id].hnext, id = *p) {
		if (x->xb[id].vpc != (uint32_t)pc)
			continue;
		if (rec->blocks[id].pa != pa) {
			if (rec->blocks[id].links)
				salsa64_rec_unlink_block(rec, id);
			continue;
		}
		if (p != head) {
			*p = x->xb[id].hnext;
			x->xb[id].hnext = *head;
			*head = (uint16_t)id;
		}
		return id;
	}
	return 0;
}

void salsa64_rec_x_invalidate(struct salsa64_rec *rec, uint32_t pa, uint32_t end)
{
	struct salsa64_rec_x *x = &rec->x;
	for (uint32_t p = pa >> SALSA64_REC_PAGE_SHIFT; x->count && p <= (end - 1) >> SALSA64_REC_PAGE_SHIFT; p++) {
		for (unsigned id = x->xpage[p], next; id; id = next) {
			next = x->xb[id].pnext;
			const struct salsa64_rec_block *b = &rec->blocks[id];
			if (b->pa < end && b->pa + b->bytes > pa)
				salsa64_rec_kill_block(rec, id);
		}
	}
}

void salsa64_rec_x_kill_at(struct salsa64_rec *rec, uint64_t vpc)
{
	struct salsa64_rec_x *x = &rec->x;
	uint32_t v = (uint32_t)vpc, page = v & ~PAGE_MASK;
	for (uint32_t k = 0; k < SALSA64_REC_MAX_INSNS && 4 * k <= v - page; k++) {
		uint32_t start = v - 4 * k;
		for (unsigned id = x->xhash[xhash_slot(start)], next; id; id = next) {
			next = x->xb[id].hnext;
			if (x->xb[id].vpc == start && start + rec->blocks[id].bytes > v)
				salsa64_rec_kill_block(rec, id);
		}
	}
}
