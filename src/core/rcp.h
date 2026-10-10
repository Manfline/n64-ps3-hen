/*
 * Retro64 — registros del RCP mapeados en memoria (MI, SP, DP, VI, AI, PI,
 * RI, SI) y de la RDRAM.
 *
 * Estado de la Fase 1 (primer hito):
 *   MI  completo: modo, versión, interrupciones y máscara → Cause.IP2.
 *   PI  DMA cartucho → RDRAM inmediato, con interrupción. Sin latencias.
 *   SI  DMA RDRAM ↔ PIF RAM inmediato, con interrupción. Falta procesar los
 *       comandos del PIF (controles, EEPROM).
 *   SP  bits de SP_STATUS y semáforo. Falta el DMA y arrancar el RSP.
 *   VI, AI, DP, RI, RDRAM: solo guardan los registros; escribir VI_V_CURRENT
 *       o AI_STATUS baja su interrupción. Falta la temporización.
 *
 * Cada bloque recibe el desplazamiento del registro dentro de su página
 * (paddr & 0xFFFFF) y accesos de 32 bits.
 */
#ifndef RETRO64_CORE_RCP_H
#define RETRO64_CORE_RCP_H

#include "core/types.h"

struct retro64_bus;

/* Líneas de interrupción del MI (bits de MI_INTR y MI_INTR_MASK). */
enum retro64_irq {
	RETRO64_IRQ_SP = 0,
	RETRO64_IRQ_SI = 1,
	RETRO64_IRQ_AI = 2,
	RETRO64_IRQ_VI = 3,
	RETRO64_IRQ_PI = 4,
	RETRO64_IRQ_DP = 5,
};

#define RETRO64_MI_VERSION_VALUE 0x02020102u

struct retro64_mi {
	uint32_t mode;
	uint32_t intr;
	uint32_t mask;
};

enum retro64_sp_reg {
	RETRO64_SP_MEM_ADDR, RETRO64_SP_DRAM_ADDR, RETRO64_SP_RD_LEN,
	RETRO64_SP_WR_LEN, RETRO64_SP_STATUS, RETRO64_SP_DMA_FULL,
	RETRO64_SP_DMA_BUSY, RETRO64_SP_SEMAPHORE, RETRO64_SP_NREGS
};

#define RETRO64_SP_STATUS_HALT  0x0001u
#define RETRO64_SP_STATUS_BROKE 0x0002u

struct retro64_sp {
	uint32_t regs[RETRO64_SP_NREGS];
	uint32_t pc;
};

enum retro64_dp_reg {
	RETRO64_DP_START, RETRO64_DP_END, RETRO64_DP_CURRENT, RETRO64_DP_STATUS,
	RETRO64_DP_CLOCK, RETRO64_DP_BUFBUSY, RETRO64_DP_PIPEBUSY, RETRO64_DP_TMEM,
	RETRO64_DP_NREGS
};

enum retro64_vi_reg {
	RETRO64_VI_CONTROL, RETRO64_VI_ORIGIN, RETRO64_VI_WIDTH, RETRO64_VI_V_INTR,
	RETRO64_VI_V_CURRENT, RETRO64_VI_BURST, RETRO64_VI_V_SYNC, RETRO64_VI_H_SYNC,
	RETRO64_VI_H_SYNC_LEAP, RETRO64_VI_H_VIDEO, RETRO64_VI_V_VIDEO,
	RETRO64_VI_V_BURST, RETRO64_VI_X_SCALE, RETRO64_VI_Y_SCALE, RETRO64_VI_NREGS
};

enum retro64_ai_reg {
	RETRO64_AI_DRAM_ADDR, RETRO64_AI_LEN, RETRO64_AI_CONTROL, RETRO64_AI_STATUS,
	RETRO64_AI_DACRATE, RETRO64_AI_BITRATE, RETRO64_AI_NREGS
};

enum retro64_pi_reg {
	RETRO64_PI_DRAM_ADDR, RETRO64_PI_CART_ADDR, RETRO64_PI_RD_LEN,
	RETRO64_PI_WR_LEN, RETRO64_PI_STATUS, RETRO64_PI_BSD_DOM1_LAT,
	RETRO64_PI_BSD_DOM1_PWD, RETRO64_PI_BSD_DOM1_PGS, RETRO64_PI_BSD_DOM1_RLS,
	RETRO64_PI_BSD_DOM2_LAT, RETRO64_PI_BSD_DOM2_PWD, RETRO64_PI_BSD_DOM2_PGS,
	RETRO64_PI_BSD_DOM2_RLS, RETRO64_PI_NREGS
};

#define RETRO64_PI_STATUS_DMA_BUSY 0x01u
#define RETRO64_PI_STATUS_IO_BUSY  0x02u
#define RETRO64_PI_STATUS_INTR     0x08u

enum retro64_si_reg {
	RETRO64_SI_DRAM_ADDR, RETRO64_SI_PIF_ADDR_RD64B, RETRO64_SI_R2,
	RETRO64_SI_R3, RETRO64_SI_PIF_ADDR_WR64B, RETRO64_SI_R5,
	RETRO64_SI_STATUS, RETRO64_SI_NREGS
};

#define RETRO64_SI_STATUS_INTR 0x1000u

#define RETRO64_RI_NREGS    8
#define RETRO64_RDRAM_NREGS    16
#define RETRO64_RDRAM_REGS_END 0x03F80000u  /* después: escrituras "broadcast" */

/* Interrupciones del MI. Actualizan Cause.IP2 del CPU. */
void retro64_mi_raise(struct retro64_bus *bus, enum retro64_irq irq);
void retro64_mi_clear(struct retro64_bus *bus, enum retro64_irq irq);

void retro64_rcp_reset(struct retro64_bus *bus);

uint32_t retro64_mi_read(struct retro64_bus *bus, uint32_t reg);
void retro64_mi_write(struct retro64_bus *bus, uint32_t reg, uint32_t value);
uint32_t retro64_sp_read(struct retro64_bus *bus, uint32_t reg);
void retro64_sp_write(struct retro64_bus *bus, uint32_t reg, uint32_t value);
uint32_t retro64_dp_read(struct retro64_bus *bus, uint32_t reg);
void retro64_dp_write(struct retro64_bus *bus, uint32_t reg, uint32_t value);
uint32_t retro64_vi_read(struct retro64_bus *bus, uint32_t reg);
void retro64_vi_write(struct retro64_bus *bus, uint32_t reg, uint32_t value);
uint32_t retro64_ai_read(struct retro64_bus *bus, uint32_t reg);
void retro64_ai_write(struct retro64_bus *bus, uint32_t reg, uint32_t value);
uint32_t retro64_pi_read(struct retro64_bus *bus, uint32_t reg);
void retro64_pi_write(struct retro64_bus *bus, uint32_t reg, uint32_t value);
uint32_t retro64_si_read(struct retro64_bus *bus, uint32_t reg);
void retro64_si_write(struct retro64_bus *bus, uint32_t reg, uint32_t value);

#endif
