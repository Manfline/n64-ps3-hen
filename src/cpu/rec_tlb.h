/*
 * Salsa64 — recompilador: código fuera de KSEG0 (KSEG1 y mapeado por la TLB).
 *
 * Interno del recompilador (rec.c, rec_emit.c, rec_tlb.c). Diseño en
 * docs/rec-ppe.md, «Código fuera de KSEG0». Resumen:
 *   - Los bloques de KSEG0 van en rec->map[] por dirección física. Los demás
 *     van aquí, con clave (vpc, física): xhash por vpc y, si el código está
 *     en la RDRAM, xpage por página física para invalidarlos al escribir.
 *     Ninguno cruza una página de 4 KB. El código puede estar en la RDRAM o
 *     en la ROM del cartucho (solo lectura: nunca se invalida).
 *   - vtlb[] guarda, por página virtual de 4 KB, la traducción con que se
 *     compiló; el bloque mapeado la compara al entrar (también si llega por
 *     un enlace o por JR) y, si cambió, sale al despachador.
 *   - Escribir la TLB no toca los bloques: antes de correr código,
 *     salsa64_rec_x_sync vuelve a traducir las páginas anotadas si cambiaron
 *     la TLB, el ASID o ERL, y borra las que ya no dan lo mismo.
 */
#ifndef SALSA64_CPU_REC_TLB_H
#define SALSA64_CPU_REC_TLB_H

#include "core/bus.h"
#include "core/types.h"

struct salsa64_rec;

#define SALSA64_REC_X_IDS       65536u    /* ids de bloque de 16 bits (rec.h: MAX_BLOCKS + 1) */
#define SALSA64_REC_XHASH       16384     /* cabezas de xhash */
#define SALSA64_REC_VT_PAGES    4096      /* entradas de vtlb[]: bits 23–12 de la virtual */
#define SALSA64_REC_PAGE_SHIFT  12

/* Dónde está el código de un bloque. */
enum {
	SALSA64_REC_KSEG0,     /* rec->map[], sin traducción */
	SALSA64_REC_KSEG1,     /* xhash, sin traducción */
	SALSA64_REC_MAPPED,    /* xhash; revisa vtlb[] al entrar */
	SALSA64_REC_SEG_NONE,  /* no se compila: lo corre el intérprete */
};

/* 8 bytes: exit_jr indexa xb[] con << 3 y lee vpc en +0. */
struct salsa64_rec_xblock {
	uint32_t vpc;        /* dirección virtual (extensión de signo de 32 bits), también en KSEG0 */
	uint16_t hnext;      /* siguiente en la misma cabeza de xhash */
	uint16_t pnext;      /* siguiente en la misma página de xpage */
};

/* Va dentro de struct salsa64_rec, justo antes de flags: vtlb[] es lo último
 * y el código generado la lee con un desplazamiento negativo desde
 * code_flags (r28). */
struct salsa64_rec_x {
	uint32_t count;          /* bloques vivos en xhash */
	uint32_t tlb_writes;     /* cpu->tlb_writes, ASID y ERL con que se revisó vtlb[] */
	uint8_t asid;
	bool erl;
	uint32_t nlive;          /* entradas de live[] */
	struct salsa64_rec_xblock xb[SALSA64_REC_X_IDS];
	uint16_t xhash[SALSA64_REC_XHASH];
	uint16_t xpage[SALSA64_RDRAM_MAX >> SALSA64_REC_PAGE_SHIFT];
	uint16_t live[SALSA64_REC_VT_PAGES];  /* entradas de vtlb[] distintas de 0 */
	uint32_t vtlb[SALSA64_REC_VT_PAGES];
};

/* Entrada de vtlb[] de `va`, y su valor con la página traducida a la de `pa`
 * (pa < 2^29): bit 31, los bits 31–24 de va y la página física. */
static inline unsigned salsa64_rec_vt_slot(uint64_t va)
{
	return ((uint32_t)va >> SALSA64_REC_PAGE_SHIFT) & (SALSA64_REC_VT_PAGES - 1);
}

static inline uint32_t salsa64_rec_vt_value(uint64_t va, uint32_t pa)
{
	return 0x80000000u | ((uint32_t)va >> 24) << 17 | pa >> SALSA64_REC_PAGE_SHIFT;
}

/* La instrucción en la dirección física `pa` (RDRAM o ROM). */
uint32_t salsa64_rec_fetch(const struct salsa64_rec *rec, uint32_t pa);
/* Hasta dónde puede llegar un bloque que empieza en `pa` (fin de la RDRAM o
 * de la ROM; fuera de KSEG0, fin de la página de 4 KB). */
uint32_t salsa64_rec_code_end(const struct salsa64_rec *rec, uint32_t pa, unsigned seg);

void salsa64_rec_x_init(struct salsa64_rec *rec);
void salsa64_rec_x_flush(struct salsa64_rec *rec);
/* Antes de correr código generado (KSEG0 enlaza con mapeados). */
void salsa64_rec_x_sync(struct salsa64_rec *rec);
/* Segmento y física del código en `pc` (modo kernel), o SALSA64_REC_SEG_NONE. */
unsigned salsa64_rec_seg(struct salsa64_rec *rec, uint64_t pc, uint32_t *pa);
/* El bloque fuera de KSEG0 de (pc, pa), o 0. */
unsigned salsa64_rec_x_find(struct salsa64_rec *rec, uint64_t pc, uint32_t pa);
/* Bloque nuevo `id` (de cualquier segmento): anota su vpc y, fuera de KSEG0,
 * lo agrega a xhash y xpage. */
void salsa64_rec_x_add(struct salsa64_rec *rec, unsigned id, uint64_t pc, uint32_t pa, unsigned seg, bool code);
/* Lo saca de xhash y xpage; false si es de KSEG0 (está en map[]). */
bool salsa64_rec_x_remove(struct salsa64_rec *rec, unsigned id);
/* Mata los bloques fuera de KSEG0 con código en la RDRAM [pa, end). */
void salsa64_rec_x_invalidate(struct salsa64_rec *rec, uint32_t pa, uint32_t end);
/* Mata los bloques fuera de KSEG0 que contienen `vpc`. */
void salsa64_rec_x_kill_at(struct salsa64_rec *rec, uint64_t vpc);

/* rec.c, para rec_tlb.c. */
void salsa64_rec_kill_block(struct salsa64_rec *rec, unsigned id);
void salsa64_rec_unlink_block(struct salsa64_rec *rec, unsigned id);

#endif
