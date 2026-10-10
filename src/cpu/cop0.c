/*
 * Retro64 — COP0 del VR4300: registros de control, excepciones, TLB,
 * Count/Compare e interrupciones.
 *
 * Vectores (BEV = 0 → base 0x80000000; BEV = 1 → 0xBFC00200):
 *   +0x000 refill de TLB (32 bits), +0x080 refill XTLB (64 bits),
 *   +0x180 todas las demás. Con EXL = 1 todo va a +0x180 y EPC/BD no cambian.
 */
#include <string.h>

#include "core/sched.h"
#include "cpu/vr4300_internal.h"

#define CAUSE_IP7       0x8000u
#define CAUSE_SW_MASK   0x0300u  /* IP0–IP1: los únicos bits escribibles */
#define STATUS_WMASK    0xFF57FFFFu
#define CONFIG_WMASK    0x0F00800Fu
#define CONFIG_RESET    0x7006E463u
#define ENTRYHI_MASK    0xC00000FFFFFFE0FFull
#define ENTRYLO_MASK    0x3FFFFFFFu
#define PAGEMASK_MASK   0x01FFE000u
#define VEC_BASE        0xFFFFFFFF80000000ull
#define VEC_BASE_BEV    0xFFFFFFFFBFC00200ull

/* ---- Estado derivado ------------------------------------------------------ */

static void update_irq(struct retro64_cpu *cpu)
{
	uint32_t st = cpu->status;
	cpu->irq_pending = (cpu->cause & st & RETRO64_ST_IM) != 0 &&
			   (st & (RETRO64_ST_IE | RETRO64_ST_EXL | RETRO64_ST_ERL)) == RETRO64_ST_IE;
}

void retro64_vr4300_update_status(struct retro64_cpu *cpu)
{
	uint32_t st = cpu->status;
	if (st & (RETRO64_ST_EXL | RETRO64_ST_ERL))
		cpu->mode = RETRO64_MODE_KERNEL;
	else if ((st & RETRO64_ST_KSU) == 0)
		cpu->mode = RETRO64_MODE_KERNEL;
	else if ((st & RETRO64_ST_KSU) == 0x08)
		cpu->mode = RETRO64_MODE_SUPERVISOR;
	else
		cpu->mode = RETRO64_MODE_USER;

	cpu->allow64 = cpu->mode == RETRO64_MODE_KERNEL ||
		       (cpu->mode == RETRO64_MODE_SUPERVISOR && (st & RETRO64_ST_SX)) ||
		       (cpu->mode == RETRO64_MODE_USER && (st & RETRO64_ST_UX));
	update_irq(cpu);
}

void retro64_cpu_set_ip(struct retro64_cpu *cpu, unsigned line, bool level)
{
	uint32_t bit = 1u << (RETRO64_CAUSE_IP_SHIFT + line);
	if (level)
		cpu->cause |= bit;
	else
		cpu->cause &= ~bit;
	update_irq(cpu);
}

/* ---- Count / Compare ------------------------------------------------------- */

uint32_t retro64_cpu_count(const struct retro64_cpu *cpu)
{
	return (uint32_t)(cpu->cycles >> 1) + cpu->count_offset;
}

/* Count avanza 1 cada 2 ciclos; el evento cae en el primer ciclo en que
 * Count llega a Compare. Si ya son iguales, la próxima vez es tras la vuelta
 * completa de 32 bits. */
static void schedule_compare(struct retro64_cpu *cpu)
{
	uint32_t delta = cpu->compare - retro64_cpu_count(cpu);
	uint64_t ticks = delta ? delta : 0x100000000ull;
	retro64_sched_set(cpu->sched, RETRO64_EV_COMPARE, ((cpu->cycles >> 1) + ticks) << 1);
}

static void compare_event(void *ctx, uint64_t when)
{
	struct retro64_cpu *cpu = ctx;
	cpu->cause |= CAUSE_IP7;
	update_irq(cpu);
	retro64_sched_set(cpu->sched, RETRO64_EV_COMPARE, when + (0x100000000ull << 1));
}

static uint32_t cop0_random(const struct retro64_cpu *cpu)
{
	/* Random baja 1 por instrucción de 31 a Wired y vuelve a 31. */
	uint64_t n = cpu->cycles - cpu->random_epoch;
	uint32_t wired = cpu->wired & 31;
	return 31 - (uint32_t)(n % (32 - wired));
}

/* ---- Excepciones ----------------------------------------------------------- */

static void exception_at(struct retro64_cpu *cpu, enum retro64_exc code, unsigned ce, uint32_t offset)
{
	if (!(cpu->status & RETRO64_ST_EXL)) {
		if (cpu->delay_slot) {
			cpu->epc = cpu->cur_pc - 4;
			cpu->cause |= RETRO64_CAUSE_BD;
		} else {
			cpu->epc = cpu->cur_pc;
			cpu->cause &= ~RETRO64_CAUSE_BD;
		}
	} else {
		offset = 0x180;
	}
	cpu->cause = (cpu->cause & ~(0x7Cu | (3u << RETRO64_CAUSE_CE_SHIFT))) |
		     (uint32_t)code << 2 | (ce & 3) << RETRO64_CAUSE_CE_SHIFT;
	cpu->status |= RETRO64_ST_EXL;

	cpu->pc = ((cpu->status & RETRO64_ST_BEV) ? VEC_BASE_BEV : VEC_BASE) + offset;
	cpu->next_pc = cpu->pc + 4;
	cpu->branch = 0;
	retro64_vr4300_update_status(cpu);
}

void retro64_vr4300_exception(struct retro64_cpu *cpu, enum retro64_exc code, unsigned ce)
{
	exception_at(cpu, code, ce, 0x180);
}

/* BadVAddr, Context, XContext y EntryHi.VPN2 apuntan a la dirección que falló. */
static void set_bad_vaddr(struct retro64_cpu *cpu, uint64_t va)
{
	cpu->bad_vaddr = va;
	cpu->context = (cpu->context & 0xFFFFFFFFFF800000ull) | ((va >> 13) & 0x7FFFF) << 4;
	cpu->xcontext = (cpu->xcontext & 0xFFFFFFFE00000000ull) | ((va >> 62) & 3) << 31 |
			((va >> 13) & 0x7FFFFFF) << 4;
	cpu->entry_hi = (va & 0xC00000FFFFFFE000ull) | (cpu->entry_hi & 0xFF);
}

void retro64_vr4300_address_error(struct retro64_cpu *cpu, uint64_t vaddr, enum retro64_access acc)
{
	set_bad_vaddr(cpu, vaddr);
	retro64_vr4300_exception(cpu, acc == RETRO64_ACC_STORE ? RETRO64_EXC_ADES : RETRO64_EXC_ADEL, 0);
}

enum tlb_fault { TLB_MISS, TLB_INVALID, TLB_MODIFIED };

static void tlb_exception(struct retro64_cpu *cpu, uint64_t va, enum retro64_access acc, enum tlb_fault f)
{
	set_bad_vaddr(cpu, va);
	enum retro64_exc code = f == TLB_MODIFIED ? RETRO64_EXC_MOD
				: acc == RETRO64_ACC_STORE ? RETRO64_EXC_TLBS : RETRO64_EXC_TLBL;
	uint32_t offset = 0x180;
	if (f == TLB_MISS) {
		uint32_t xbit = cpu->mode == RETRO64_MODE_KERNEL ? RETRO64_ST_KX
				: cpu->mode == RETRO64_MODE_SUPERVISOR ? RETRO64_ST_SX : RETRO64_ST_UX;
		offset = (cpu->status & xbit) ? 0x080 : 0x000;
	}
	exception_at(cpu, code, 0, offset);
}

/* ---- TLB y traducción ------------------------------------------------------ */

static bool tlb_lookup(struct retro64_cpu *cpu, uint64_t va, uint32_t *pa, enum retro64_access acc)
{
	uint32_t a = (uint32_t)va, asid = (uint32_t)cpu->entry_hi & 0xFF;

	for (int n = 0; n < RETRO64_TLB_ENTRIES; n++) {
		const struct retro64_tlb_entry *e = &cpu->tlb[n];
		if ((a ^ (uint32_t)e->entry_hi) & e->vpn_mask)
			continue;
		if (!e->global && ((uint32_t)e->entry_hi & 0xFF) != asid)
			continue;
		uint32_t lo = e->entry_lo[(a & e->odd_bit) ? 1 : 0];
		if (!(lo & 0x2)) {
			tlb_exception(cpu, va, acc, TLB_INVALID);
			return false;
		}
		if (acc == RETRO64_ACC_STORE && !(lo & 0x4)) {
			tlb_exception(cpu, va, acc, TLB_MODIFIED);
			return false;
		}
		uint32_t offset_mask = e->odd_bit - 1;
		*pa = ((lo >> 6) << 12 & ~offset_mask) | (a & offset_mask);
		return true;
	}
	tlb_exception(cpu, va, acc, TLB_MISS);
	return false;
}

bool retro64_vr4300_translate_slow(struct retro64_cpu *cpu, uint64_t va, uint32_t *pa,
				   enum retro64_access acc)
{
	/* Solo direccionamiento de 32 bits por ahora (ver vr4300.h). */
	if (RETRO64_UNLIKELY(retro64_sx32((uint32_t)va) != va)) {
		retro64_vr4300_address_error(cpu, va, acc);
		return false;
	}

	uint32_t a = (uint32_t)va;
	if (cpu->mode == RETRO64_MODE_USER && a >= 0x80000000u) {
		retro64_vr4300_address_error(cpu, va, acc);
		return false;
	}
	if (cpu->mode == RETRO64_MODE_SUPERVISOR && a >= 0x80000000u &&
	    (a < 0xC0000000u || a >= 0xE0000000u)) {
		retro64_vr4300_address_error(cpu, va, acc);
		return false;
	}

	if (a < 0x80000000u) {
		if (cpu->status & RETRO64_ST_ERL) {  /* kuseg sin mapear con ERL */
			*pa = a;
			return true;
		}
		return tlb_lookup(cpu, va, pa, acc);
	}
	if (a < 0xC0000000u) {
		*pa = a & 0x1FFFFFFF;
		return true;
	}
	return tlb_lookup(cpu, va, pa, acc);
}

static void tlb_write(struct retro64_cpu *cpu, unsigned n)
{
	struct retro64_tlb_entry *e = &cpu->tlb[n & 31];
	e->page_mask = cpu->page_mask & PAGEMASK_MASK;
	e->entry_hi = cpu->entry_hi & ENTRYHI_MASK & ~(uint64_t)e->page_mask;
	e->entry_lo[0] = cpu->entry_lo0 & ENTRYLO_MASK & ~1u;
	e->entry_lo[1] = cpu->entry_lo1 & ENTRYLO_MASK & ~1u;
	e->global = cpu->entry_lo0 & cpu->entry_lo1 & 1;
	e->vpn_mask = ~(e->page_mask | 0x1FFFu);
	e->odd_bit = ((e->page_mask | 0x1FFFu) + 1) >> 1;
}

static void tlb_read(struct retro64_cpu *cpu, unsigned n)
{
	const struct retro64_tlb_entry *e = &cpu->tlb[n & 31];
	cpu->page_mask = e->page_mask;
	cpu->entry_hi = e->entry_hi;
	cpu->entry_lo0 = e->entry_lo[0] | e->global;
	cpu->entry_lo1 = e->entry_lo[1] | e->global;
}

static void tlb_probe(struct retro64_cpu *cpu)
{
	uint32_t hi = (uint32_t)cpu->entry_hi, asid = hi & 0xFF;
	cpu->index = 0x80000000u;
	for (int n = 0; n < RETRO64_TLB_ENTRIES; n++) {
		const struct retro64_tlb_entry *e = &cpu->tlb[n];
		if ((hi ^ (uint32_t)e->entry_hi) & e->vpn_mask)
			continue;
		if (!e->global && ((uint32_t)e->entry_hi & 0xFF) != asid)
			continue;
		cpu->index = (uint32_t)n;
		return;
	}
}

/* ---- Registros ------------------------------------------------------------- */

uint64_t retro64_cpu_cop0_read(struct retro64_cpu *cpu, unsigned reg)
{
	switch (reg) {
	case RETRO64_CP0_INDEX:       return cpu->index;
	case RETRO64_CP0_RANDOM:      return cop0_random(cpu);
	case RETRO64_CP0_ENTRYLO0:    return cpu->entry_lo0;
	case RETRO64_CP0_ENTRYLO1:    return cpu->entry_lo1;
	case RETRO64_CP0_CONTEXT:     return cpu->context;
	case RETRO64_CP0_PAGEMASK:    return cpu->page_mask;
	case RETRO64_CP0_WIRED:       return cpu->wired;
	case RETRO64_CP0_BADVADDR:    return cpu->bad_vaddr;
	case RETRO64_CP0_COUNT:       return retro64_cpu_count(cpu);
	case RETRO64_CP0_ENTRYHI:     return cpu->entry_hi;
	case RETRO64_CP0_COMPARE:     return cpu->compare;
	case RETRO64_CP0_STATUS:      return cpu->status;
	case RETRO64_CP0_CAUSE:       return cpu->cause;
	case RETRO64_CP0_EPC:         return cpu->epc;
	case RETRO64_CP0_PRID:        return RETRO64_PRID_VALUE;
	case RETRO64_CP0_CONFIG:      return cpu->config;
	case RETRO64_CP0_LLADDR:      return cpu->lladdr;
	case RETRO64_CP0_WATCHLO:     return cpu->watch_lo;
	case RETRO64_CP0_WATCHHI:     return cpu->watch_hi;
	case RETRO64_CP0_XCONTEXT:    return cpu->xcontext;
	case RETRO64_CP0_PARITYERROR: return cpu->parity_error;
	case RETRO64_CP0_CACHEERROR:  return cpu->cache_error;
	case RETRO64_CP0_TAGLO:       return cpu->tag_lo;
	case RETRO64_CP0_TAGHI:       return cpu->tag_hi;
	case RETRO64_CP0_ERROREPC:    return cpu->error_epc;
	default:                      return cpu->cop0_latch;  /* 7, 21–25, 31 */
	}
}

void retro64_cpu_cop0_write(struct retro64_cpu *cpu, unsigned reg, uint64_t v)
{
	cpu->cop0_latch = v;
	switch (reg) {
	case RETRO64_CP0_INDEX:
		cpu->index = (cpu->index & 0x80000000u) | ((uint32_t)v & 0x3F);
		return;
	case RETRO64_CP0_ENTRYLO0:    cpu->entry_lo0 = (uint32_t)v & ENTRYLO_MASK; return;
	case RETRO64_CP0_ENTRYLO1:    cpu->entry_lo1 = (uint32_t)v & ENTRYLO_MASK; return;
	case RETRO64_CP0_CONTEXT:
		cpu->context = (v & 0xFFFFFFFFFF800000ull) | (cpu->context & 0x7FFFF0);
		return;
	case RETRO64_CP0_PAGEMASK:    cpu->page_mask = (uint32_t)v & PAGEMASK_MASK; return;
	case RETRO64_CP0_WIRED:
		cpu->wired = (uint32_t)v & 0x3F;
		cpu->random_epoch = cpu->cycles;
		return;
	case RETRO64_CP0_COUNT:
		cpu->count_offset = (uint32_t)v - (uint32_t)(cpu->cycles >> 1);
		schedule_compare(cpu);
		return;
	case RETRO64_CP0_ENTRYHI:     cpu->entry_hi = v & ENTRYHI_MASK; return;
	case RETRO64_CP0_COMPARE:
		cpu->compare = (uint32_t)v;
		cpu->cause &= ~CAUSE_IP7;
		update_irq(cpu);
		schedule_compare(cpu);
		return;
	case RETRO64_CP0_STATUS:
		cpu->status = (uint32_t)v & STATUS_WMASK;
		retro64_vr4300_update_status(cpu);
		return;
	case RETRO64_CP0_CAUSE:
		cpu->cause = (cpu->cause & ~CAUSE_SW_MASK) | ((uint32_t)v & CAUSE_SW_MASK);
		update_irq(cpu);
		return;
	case RETRO64_CP0_EPC:         cpu->epc = v; return;
	case RETRO64_CP0_CONFIG:
		cpu->config = (cpu->config & ~CONFIG_WMASK) | ((uint32_t)v & CONFIG_WMASK);
		return;
	case RETRO64_CP0_LLADDR:      cpu->lladdr = (uint32_t)v; return;
	case RETRO64_CP0_WATCHLO:     cpu->watch_lo = (uint32_t)v & 0xFFFFFFFBu; return;
	case RETRO64_CP0_WATCHHI:     cpu->watch_hi = (uint32_t)v & 0xF; return;
	case RETRO64_CP0_XCONTEXT:
		cpu->xcontext = (v & 0xFFFFFFFE00000000ull) | (cpu->xcontext & 0x1FFFFFFFFull);
		return;
	case RETRO64_CP0_PARITYERROR: cpu->parity_error = (uint32_t)v & 0xFF; return;
	case RETRO64_CP0_TAGLO:       cpu->tag_lo = (uint32_t)v & 0x0FFFFFC0u; return;
	case RETRO64_CP0_TAGHI:       cpu->tag_hi = 0; return;
	case RETRO64_CP0_ERROREPC:    cpu->error_epc = v; return;
	default:                      return;  /* solo lectura o sin implementar */
	}
}

/* ---- Instrucciones --------------------------------------------------------- */

static void op_eret(struct retro64_cpu *cpu)
{
	if (cpu->status & RETRO64_ST_ERL) {
		cpu->pc = cpu->error_epc;
		cpu->status &= ~RETRO64_ST_ERL;
	} else {
		cpu->pc = cpu->epc;
		cpu->status &= ~RETRO64_ST_EXL;
	}
	cpu->next_pc = cpu->pc + 4;
	cpu->branch = 0;  /* ERET no tiene delay slot */
	cpu->llbit = 0;
	retro64_vr4300_update_status(cpu);
}

void retro64_vr4300_cop0(struct retro64_cpu *cpu, uint32_t i)
{
	if (cpu->mode != RETRO64_MODE_KERNEL && !(cpu->status & RETRO64_ST_CU0)) {
		retro64_vr4300_exception(cpu, RETRO64_EXC_CPU, 0);
		return;
	}

	unsigned rt = I_RT(i), rd = I_RD(i);
	switch (I_RS(i)) {
	case 0x00:                                                             /* MFC0 */
		cpu->gpr[rt] = retro64_sx32((uint32_t)retro64_cpu_cop0_read(cpu, rd));
		return;
	case 0x01:                                                             /* DMFC0 */
		if (!cpu->allow64)
			break;
		cpu->gpr[rt] = retro64_cpu_cop0_read(cpu, rd);
		return;
	case 0x04:                                                             /* MTC0 */
		retro64_cpu_cop0_write(cpu, rd, retro64_sx32((uint32_t)cpu->gpr[rt]));
		return;
	case 0x05:                                                             /* DMTC0 */
		if (!cpu->allow64)
			break;
		retro64_cpu_cop0_write(cpu, rd, cpu->gpr[rt]);
		return;
	case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		switch (I_FUNCT(i)) {
		case 0x01: tlb_read(cpu, cpu->index); return;                 /* TLBR */
		case 0x02: tlb_write(cpu, cpu->index); return;                /* TLBWI */
		case 0x06: tlb_write(cpu, cop0_random(cpu)); return;          /* TLBWR */
		case 0x08: tlb_probe(cpu); return;                            /* TLBP */
		case 0x18: op_eret(cpu); return;                              /* ERET */
		default:   break;
		}
		break;
	default:
		break;
	}
	vr4300_reserved(cpu);
}

/* ---- Inicio y reset ---------------------------------------------------------- */

void retro64_cpu_init(struct retro64_cpu *cpu, struct retro64_bus *bus, struct retro64_sched *sched)
{
	memset(cpu, 0, sizeof *cpu);
	cpu->bus = bus;
	cpu->sched = sched;
	cpu->rdram = bus->rdram;
	cpu->rdram_size = bus->rdram_size;
	bus->cpu = cpu;
	retro64_sched_handler(sched, RETRO64_EV_COMPARE, compare_event, cpu);
	retro64_cpu_reset(cpu);
}

void retro64_cpu_reset(struct retro64_cpu *cpu)
{
	memset(cpu->gpr, 0, sizeof cpu->gpr);
	memset(cpu->fpr, 0, sizeof cpu->fpr);
	cpu->hi = cpu->lo = 0;
	cpu->fcr31 = 0;
	cpu->llbit = 0;

	cpu->status = RETRO64_ST_ERL | RETRO64_ST_BEV;
	cpu->cause = 0;
	cpu->config = CONFIG_RESET;
	cpu->index = cpu->entry_lo0 = cpu->entry_lo1 = cpu->page_mask = 0;
	cpu->wired = 0;
	cpu->random_epoch = cpu->cycles;
	cpu->context = cpu->xcontext = cpu->bad_vaddr = cpu->entry_hi = 0;
	cpu->epc = cpu->error_epc = 0;
	cpu->lladdr = cpu->watch_lo = cpu->watch_hi = 0;
	cpu->parity_error = cpu->cache_error = cpu->tag_lo = cpu->tag_hi = 0;
	cpu->cop0_latch = 0;

	/* El contenido real de la TLB es indefinido al arrancar. Aquí quedan
	 * entradas inválidas (V = 0) con VPN distintas dentro de KSEG0, que
	 * nunca pasa por la TLB: así ninguna coincide por accidente (es lo
	 * mismo que hace libultra al inicializarla). */
	memset(cpu->tlb, 0, sizeof cpu->tlb);
	for (int n = 0; n < RETRO64_TLB_ENTRIES; n++) {
		cpu->tlb[n].entry_hi = 0xFFFFFFFF80000000ull + (uint64_t)n * 0x2000;
		cpu->tlb[n].vpn_mask = ~0x1FFFu;
		cpu->tlb[n].odd_bit = 0x1000;
	}

	cpu->compare = 0;
	cpu->count_offset = 0u - (uint32_t)(cpu->cycles >> 1);
	retro64_vr4300_update_status(cpu);
	schedule_compare(cpu);
	retro64_cpu_set_pc(cpu, 0xFFFFFFFFBFC00000ull);
}

void retro64_cpu_set_pc(struct retro64_cpu *cpu, uint64_t pc)
{
	cpu->pc = pc;
	cpu->next_pc = pc + 4;
	cpu->branch = 0;
	cpu->delay_slot = 0;
}
