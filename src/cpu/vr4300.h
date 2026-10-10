/*
 * Retro64 — CPU NEC VR4300 (MIPS III, 64 bits): estado y API del intérprete.
 *
 * Modelo de ejecución:
 *   - `pc` es la próxima instrucción a ejecutar y `next_pc` la siguiente.
 *     Un salto escribe `next_pc` y marca `branch`: la instrucción que sigue
 *     (el delay slot) se ejecuta con `delay_slot` = 1 y luego el flujo va al
 *     destino. Un branch-likely no tomado salta el delay slot.
 *   - Toda excepción usa `cur_pc` (dirección de la instrucción en curso) y
 *     `delay_slot` para EPC y Cause.BD, y deja `pc` en el vector.
 *   - El tiempo es `cycles` (PClock, 93,75 MHz). Count avanza a la mitad.
 *     Por ahora cada instrucción cuesta 1 ciclo; el costo real (caché,
 *     multiplicaciones, divisiones) se calibra más adelante.
 *
 * Alcance del primer hito de la Fase 1:
 *   - Direccionamiento de 32 bits (KX = SX = UX = 0). Las direcciones de 64
 *     bits que no son extensión de signo de 32 dan error de dirección.
 *   - TLB de 32 entradas con refill, inválida y modificación.
 *   - FPU: formatos S, D, W, L; modo FR. Aritmética siempre con redondeo al
 *     más cercano; la bandera "inexacto" no se calcula todavía.
 *   - Sin cachés, sin watchpoints, sin errores de bus.
 */
#ifndef RETRO64_CPU_VR4300_H
#define RETRO64_CPU_VR4300_H

#include "core/types.h"

struct retro64_bus;
struct retro64_sched;

/* Status */
#define RETRO64_ST_IE   0x00000001u
#define RETRO64_ST_EXL  0x00000002u
#define RETRO64_ST_ERL  0x00000004u
#define RETRO64_ST_KSU  0x00000018u
#define RETRO64_ST_UX   0x00000020u
#define RETRO64_ST_SX   0x00000040u
#define RETRO64_ST_KX   0x00000080u
#define RETRO64_ST_IM   0x0000FF00u
#define RETRO64_ST_BEV  0x00400000u
#define RETRO64_ST_FR   0x04000000u
#define RETRO64_ST_CU0  0x10000000u
#define RETRO64_ST_CU1  0x20000000u
#define RETRO64_ST_CU2  0x40000000u

/* Cause */
#define RETRO64_CAUSE_BD       0x80000000u
#define RETRO64_CAUSE_CE_SHIFT 28
#define RETRO64_CAUSE_IP_SHIFT 8
#define RETRO64_CAUSE_EXC(c)   (((c) >> 2) & 0x1F)

/* FCR31 */
#define RETRO64_FCR31_C  0x00800000u
#define RETRO64_FCR31_FS 0x01000000u

#define RETRO64_PRID_VALUE   0x00000B22u
#define RETRO64_FCR0_VALUE   0x00000A00u
#define RETRO64_TLB_ENTRIES  32

enum retro64_exc {
	RETRO64_EXC_INT  = 0,
	RETRO64_EXC_MOD  = 1,
	RETRO64_EXC_TLBL = 2,
	RETRO64_EXC_TLBS = 3,
	RETRO64_EXC_ADEL = 4,
	RETRO64_EXC_ADES = 5,
	RETRO64_EXC_IBE  = 6,
	RETRO64_EXC_DBE  = 7,
	RETRO64_EXC_SYS  = 8,
	RETRO64_EXC_BP   = 9,
	RETRO64_EXC_RI   = 10,
	RETRO64_EXC_CPU  = 11,
	RETRO64_EXC_OV   = 12,
	RETRO64_EXC_TR   = 13,
	RETRO64_EXC_FPE  = 15,
	RETRO64_EXC_WATCH = 23,
};

enum retro64_cop0_reg {
	RETRO64_CP0_INDEX = 0, RETRO64_CP0_RANDOM = 1, RETRO64_CP0_ENTRYLO0 = 2,
	RETRO64_CP0_ENTRYLO1 = 3, RETRO64_CP0_CONTEXT = 4, RETRO64_CP0_PAGEMASK = 5,
	RETRO64_CP0_WIRED = 6, RETRO64_CP0_BADVADDR = 8, RETRO64_CP0_COUNT = 9,
	RETRO64_CP0_ENTRYHI = 10, RETRO64_CP0_COMPARE = 11, RETRO64_CP0_STATUS = 12,
	RETRO64_CP0_CAUSE = 13, RETRO64_CP0_EPC = 14, RETRO64_CP0_PRID = 15,
	RETRO64_CP0_CONFIG = 16, RETRO64_CP0_LLADDR = 17, RETRO64_CP0_WATCHLO = 18,
	RETRO64_CP0_WATCHHI = 19, RETRO64_CP0_XCONTEXT = 20,
	RETRO64_CP0_PARITYERROR = 26, RETRO64_CP0_CACHEERROR = 27,
	RETRO64_CP0_TAGLO = 28, RETRO64_CP0_TAGHI = 29, RETRO64_CP0_ERROREPC = 30,
};

/* Modo de operación efectivo (Status.KSU con EXL/ERL). */
enum retro64_cpu_mode {
	RETRO64_MODE_KERNEL = 0,
	RETRO64_MODE_SUPERVISOR = 1,
	RETRO64_MODE_USER = 2,
};

struct retro64_tlb_entry {
	uint64_t entry_hi;     /* VPN2 (enmascarado por page_mask) y ASID */
	uint32_t entry_lo[2];  /* PFN, C, D, V (G aparte) */
	uint32_t page_mask;
	uint32_t vpn_mask;     /* bits de la dirección virtual que se comparan */
	uint32_t odd_bit;      /* bit que elige entry_lo[1] */
	uint32_t global;
};

struct retro64_cpu {
	/* Estado de ejecución: lo más usado al principio de la estructura. */
	uint64_t gpr[32];
	uint64_t hi, lo;
	uint64_t pc;
	uint64_t next_pc;
	uint64_t cur_pc;
	uint64_t cycles;

	uint8_t *rdram;        /* copia de bus->rdram para el camino rápido */
	uint32_t rdram_size;

	uint8_t branch;        /* la instrucción en curso es un salto */
	uint8_t delay_slot;    /* la instrucción en curso está en un delay slot */
	uint8_t irq_pending;   /* hay interrupción habilitada y no enmascarada */
	uint8_t mode;          /* enum retro64_cpu_mode */
	uint8_t allow64;       /* operaciones de 64 bits permitidas en este modo */
	uint8_t llbit;
	uint8_t running;

	/* COP0 */
	uint32_t status;
	uint32_t cause;
	uint32_t compare;
	uint32_t count_offset;   /* Count = (cycles >> 1) + count_offset */
	uint32_t index;
	uint32_t entry_lo0, entry_lo1;
	uint32_t page_mask;
	uint32_t wired;
	uint32_t config;
	uint32_t lladdr;
	uint32_t watch_lo, watch_hi;
	uint32_t parity_error, cache_error;
	uint32_t tag_lo, tag_hi;
	uint64_t random_epoch;   /* ciclo en que Random valía 31 */
	uint64_t context;
	uint64_t xcontext;
	uint64_t bad_vaddr;
	uint64_t entry_hi;
	uint64_t epc;
	uint64_t error_epc;
	uint64_t cop0_latch;     /* último valor escrito: lo leen los registros 7, 21–25, 31 */

	/* COP1 */
	uint64_t fpr[32];
	uint32_t fcr31;

	struct retro64_tlb_entry tlb[RETRO64_TLB_ENTRIES];

	struct retro64_bus *bus;
	struct retro64_sched *sched;
};

void retro64_cpu_init(struct retro64_cpu *cpu, struct retro64_bus *bus,
		      struct retro64_sched *sched);
/* Reset en frío: Status.ERL|BEV, PC = 0xBFC00000 (PIF ROM). */
void retro64_cpu_reset(struct retro64_cpu *cpu);
void retro64_cpu_set_pc(struct retro64_cpu *cpu, uint64_t pc);

/* Ejecuta una instrucción (o atiende una interrupción pendiente). */
void retro64_cpu_step(struct retro64_cpu *cpu);
/* Ejecuta hasta que un evento ponga running = 0 (ver retro64_n64_run). */
void retro64_cpu_run(struct retro64_cpu *cpu);

/* Línea de interrupción externa: 2 = RCP (MI), 7 = Count/Compare. */
void retro64_cpu_set_ip(struct retro64_cpu *cpu, unsigned line, bool level);

uint32_t retro64_cpu_count(const struct retro64_cpu *cpu);
uint64_t retro64_cpu_cop0_read(struct retro64_cpu *cpu, unsigned reg);
void retro64_cpu_cop0_write(struct retro64_cpu *cpu, unsigned reg, uint64_t value);

#endif
