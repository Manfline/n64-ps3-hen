/*
 * Retro64 — piezas internas del intérprete del VR4300: decodificación,
 * traducción de direcciones y accesos a memoria.
 *
 * Camino rápido: KSEG0/KSEG1 en modo kernel se traducen restando la base y,
 * si la dirección física cae en la RDRAM, se lee directo del búfer con los
 * accesores de core/endian.h (carga directa en la PS3). Todo lo demás pasa
 * por retro64_vr4300_translate_slow (TLB, modos) y por el bus.
 */
#ifndef RETRO64_CPU_VR4300_INTERNAL_H
#define RETRO64_CPU_VR4300_INTERNAL_H

#include "core/bus.h"
#include "core/endian.h"
#include "cpu/vr4300.h"

#define I_OP(i)     ((i) >> 26)
#define I_RS(i)     (((i) >> 21) & 31)
#define I_RT(i)     (((i) >> 16) & 31)
#define I_RD(i)     (((i) >> 11) & 31)
#define I_SA(i)     (((i) >> 6) & 31)
#define I_FUNCT(i)  ((i) & 63)
#define I_SIMM(i)   ((uint64_t)(int64_t)(int16_t)(i))  /* inmediato con signo, 64 bits */
#define I_UIMM(i)   ((uint64_t)((i) & 0xFFFF))
#define I_TARGET(i) ((i) & 0x03FFFFFF)
/* COP1 */
#define I_FMT(i)    I_RS(i)
#define I_FT(i)     I_RT(i)
#define I_FS(i)     I_RD(i)
#define I_FD(i)     I_SA(i)

enum retro64_access {
	RETRO64_ACC_FETCH,
	RETRO64_ACC_LOAD,
	RETRO64_ACC_STORE,
};

/* cop0.c */
void retro64_vr4300_exception(struct retro64_cpu *cpu, enum retro64_exc code, unsigned ce);
void retro64_vr4300_address_error(struct retro64_cpu *cpu, uint64_t vaddr, enum retro64_access acc);
bool retro64_vr4300_translate_slow(struct retro64_cpu *cpu, uint64_t vaddr, uint32_t *paddr,
				   enum retro64_access acc);
void retro64_vr4300_update_status(struct retro64_cpu *cpu);
void retro64_vr4300_cop0(struct retro64_cpu *cpu, uint32_t instr);
/* cop1.c */
void retro64_vr4300_cop1(struct retro64_cpu *cpu, uint32_t instr);

static inline void vr4300_reserved(struct retro64_cpu *cpu)
{
	retro64_vr4300_exception(cpu, RETRO64_EXC_RI, 0);
}

/* ---- Saltos ----------------------------------------------------------- */

static inline void vr4300_jump(struct retro64_cpu *cpu, uint64_t target)
{
	cpu->next_pc = target;
	cpu->branch = 1;
}

/* Salto condicional: la siguiente instrucción es delay slot, se tome o no. */
static inline void vr4300_branch(struct retro64_cpu *cpu, bool taken, uint32_t instr)
{
	if (taken)
		cpu->next_pc = cpu->cur_pc + 4 + (I_SIMM(instr) << 2);
	cpu->branch = 1;
}

/* Branch-likely: si no se toma, el delay slot se anula (no se ejecuta). */
static inline void vr4300_branch_likely(struct retro64_cpu *cpu, bool taken, uint32_t instr)
{
	if (taken) {
		cpu->next_pc = cpu->cur_pc + 4 + (I_SIMM(instr) << 2);
		cpu->branch = 1;
	} else {
		cpu->pc = cpu->next_pc;
		cpu->next_pc += 4;
	}
}

/* ---- Traducción ------------------------------------------------------- */

static inline bool vr4300_translate(struct retro64_cpu *cpu, uint64_t vaddr, uint32_t *paddr,
				    enum retro64_access acc)
{
	/* 0xFFFFFFFF80000000–0xFFFFFFFFBFFFFFFF: KSEG0 y KSEG1, sin TLB. */
	if (RETRO64_LIKELY((vaddr >> 30) == 0x3FFFFFFFEull && cpu->mode == RETRO64_MODE_KERNEL)) {
		*paddr = (uint32_t)vaddr & 0x1FFFFFFF;
		return true;
	}
	return retro64_vr4300_translate_slow(cpu, vaddr, paddr, acc);
}

/* Verifica alineación (align = tamaño - 1) y traduce. Si falla, la
 * excepción ya quedó lanzada y la instrucción no debe escribir nada. */
static inline bool vr4300_vaddr(struct retro64_cpu *cpu, uint64_t vaddr, uint32_t align,
				enum retro64_access acc, uint32_t *paddr)
{
	if (RETRO64_UNLIKELY(vaddr & align)) {
		retro64_vr4300_address_error(cpu, vaddr, acc);
		return false;
	}
	return vr4300_translate(cpu, vaddr, paddr, acc);
}

/* ---- Accesos físicos (RDRAM directo, resto por el bus) ------------------ */

static inline uint8_t vr4300_read8(struct retro64_cpu *cpu, uint32_t pa)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size))
		return retro64_rd8(cpu->rdram + pa);
	return (uint8_t)(retro64_bus_read32(cpu->bus, pa) >> (8 * (3 - (pa & 3))));
}

static inline uint16_t vr4300_read16(struct retro64_cpu *cpu, uint32_t pa)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size))
		return retro64_rd16(cpu->rdram + pa);
	return (uint16_t)(retro64_bus_read32(cpu->bus, pa) >> (8 * (2 - (pa & 2))));
}

static inline uint32_t vr4300_read32(struct retro64_cpu *cpu, uint32_t pa)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size))
		return retro64_rd32(cpu->rdram + pa);
	return retro64_bus_read32(cpu->bus, pa);
}

static inline uint64_t vr4300_read64(struct retro64_cpu *cpu, uint32_t pa)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size))
		return retro64_rd64(cpu->rdram + pa);
	return retro64_bus_read64(cpu->bus, pa);
}

static inline void vr4300_write8(struct retro64_cpu *cpu, uint32_t pa, uint8_t v)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size)) {
		retro64_wr8(cpu->rdram + pa, v);
		return;
	}
	unsigned shift = 8 * (3 - (pa & 3));
	retro64_bus_write32(cpu->bus, pa, (uint32_t)v << shift, 0xFFu << shift);
}

static inline void vr4300_write16(struct retro64_cpu *cpu, uint32_t pa, uint16_t v)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size)) {
		retro64_wr16(cpu->rdram + pa, v);
		return;
	}
	unsigned shift = 8 * (2 - (pa & 2));
	retro64_bus_write32(cpu->bus, pa, (uint32_t)v << shift, 0xFFFFu << shift);
}

static inline void vr4300_write32(struct retro64_cpu *cpu, uint32_t pa, uint32_t v)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size)) {
		retro64_wr32(cpu->rdram + pa, v);
		return;
	}
	retro64_bus_write32(cpu->bus, pa, v, 0xFFFFFFFFu);
}

static inline void vr4300_write64(struct retro64_cpu *cpu, uint32_t pa, uint64_t v)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size)) {
		retro64_wr64(cpu->rdram + pa, v);
		return;
	}
	retro64_bus_write64(cpu->bus, pa, v, ~0ull);
}

/* SWL/SWR y SDL/SDR: solo los bytes de `mask` (orden de la N64). */
static inline void vr4300_write32_masked(struct retro64_cpu *cpu, uint32_t pa, uint32_t v, uint32_t mask)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size)) {
		uint8_t *p = cpu->rdram + pa;
		retro64_wr32(p, (retro64_rd32(p) & ~mask) | (v & mask));
		return;
	}
	retro64_bus_write32(cpu->bus, pa, v, mask);
}

static inline void vr4300_write64_masked(struct retro64_cpu *cpu, uint32_t pa, uint64_t v, uint64_t mask)
{
	if (RETRO64_LIKELY(pa < cpu->rdram_size)) {
		uint8_t *p = cpu->rdram + pa;
		retro64_wr64(p, (retro64_rd64(p) & ~mask) | (v & mask));
		return;
	}
	retro64_bus_write64(cpu->bus, pa, v, mask);
}

/* ---- Registros de la FPU según Status.FR ------------------------------- */
/* FR = 1: 32 registros de 64 bits. FR = 0: 16 pares; un acceso de 32 bits a
 * un registro impar usa la mitad alta del par, y uno de 64 bits ignora el
 * bit bajo del número de registro. */

static inline uint32_t vr4300_fpr_get32(const struct retro64_cpu *cpu, unsigned r)
{
	if ((cpu->status & RETRO64_ST_FR) || !(r & 1))
		return (uint32_t)cpu->fpr[r];
	return (uint32_t)(cpu->fpr[r & ~1u] >> 32);
}

static inline void vr4300_fpr_set32(struct retro64_cpu *cpu, unsigned r, uint32_t v)
{
	if ((cpu->status & RETRO64_ST_FR) || !(r & 1))
		cpu->fpr[r] = (cpu->fpr[r] & 0xFFFFFFFF00000000ull) | v;
	else
		cpu->fpr[r & ~1u] = (cpu->fpr[r & ~1u] & 0xFFFFFFFFull) | (uint64_t)v << 32;
}

static inline uint64_t vr4300_fpr_get64(const struct retro64_cpu *cpu, unsigned r)
{
	return cpu->fpr[(cpu->status & RETRO64_ST_FR) ? r : r & ~1u];
}

static inline void vr4300_fpr_set64(struct retro64_cpu *cpu, unsigned r, uint64_t v)
{
	cpu->fpr[(cpu->status & RETRO64_ST_FR) ? r : r & ~1u] = v;
}

#endif
