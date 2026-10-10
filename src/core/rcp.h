/*
 * Salsa64 — registros del RCP mapeados en memoria (MI, SP, DP, VI, AI, PI,
 * RI, SI) y de la RDRAM.
 *
 * Estado de la Fase 1 (primer hito):
 *   MI  completo: modo, versión, interrupciones y máscara → Cause.IP2.
 *   PI  DMA cartucho → RDRAM inmediato, con interrupción. Sin latencias.
 *   SI  DMA RDRAM ↔ PIF RAM inmediato, con interrupción; comandos joybus
 *       de los controles en pif.c. Falta la EEPROM y la latencia.
 *   SP  bits de SP_STATUS y semáforo. Falta el DMA y arrancar el RSP.
 *   VI  contador de líneas, interrupción en VI_V_INTR y campos con la
 *       temporización del tipo de TV (vi.c); describe el framebuffer que
 *       se ve (salsa64_vi_frame). Sin filtros (AA, divot, gamma).
 *   AI, DP, RI, RDRAM: solo guardan los registros; escribir AI_STATUS baja
 *       su interrupción. Falta la temporización.
 *
 * Cada bloque recibe el desplazamiento del registro dentro de su página
 * (paddr & 0xFFFFF) y accesos de 32 bits.
 */
#ifndef SALSA64_CORE_RCP_H
#define SALSA64_CORE_RCP_H

#include "core/rom.h"
#include "core/types.h"

struct salsa64_bus;
struct salsa64_sched;

/* Líneas de interrupción del MI (bits de MI_INTR y MI_INTR_MASK). */
enum salsa64_irq {
	SALSA64_IRQ_SP = 0,
	SALSA64_IRQ_SI = 1,
	SALSA64_IRQ_AI = 2,
	SALSA64_IRQ_VI = 3,
	SALSA64_IRQ_PI = 4,
	SALSA64_IRQ_DP = 5,
};

#define SALSA64_MI_VERSION_VALUE 0x02020102u

struct salsa64_mi {
	uint32_t mode;
	uint32_t intr;
	uint32_t mask;
};

enum salsa64_sp_reg {
	SALSA64_SP_MEM_ADDR, SALSA64_SP_DRAM_ADDR, SALSA64_SP_RD_LEN,
	SALSA64_SP_WR_LEN, SALSA64_SP_STATUS, SALSA64_SP_DMA_FULL,
	SALSA64_SP_DMA_BUSY, SALSA64_SP_SEMAPHORE, SALSA64_SP_NREGS
};

#define SALSA64_SP_STATUS_HALT  0x0001u
#define SALSA64_SP_STATUS_BROKE 0x0002u

struct salsa64_sp {
	uint32_t regs[SALSA64_SP_NREGS];
	uint32_t pc;
};

enum salsa64_dp_reg {
	SALSA64_DP_START, SALSA64_DP_END, SALSA64_DP_CURRENT, SALSA64_DP_STATUS,
	SALSA64_DP_CLOCK, SALSA64_DP_BUFBUSY, SALSA64_DP_PIPEBUSY, SALSA64_DP_TMEM,
	SALSA64_DP_NREGS
};

enum salsa64_vi_reg {
	SALSA64_VI_CONTROL, SALSA64_VI_ORIGIN, SALSA64_VI_WIDTH, SALSA64_VI_V_INTR,
	SALSA64_VI_V_CURRENT, SALSA64_VI_BURST, SALSA64_VI_V_SYNC, SALSA64_VI_H_SYNC,
	SALSA64_VI_H_SYNC_LEAP, SALSA64_VI_H_VIDEO, SALSA64_VI_V_VIDEO,
	SALSA64_VI_V_BURST, SALSA64_VI_X_SCALE, SALSA64_VI_Y_SCALE, SALSA64_VI_NREGS
};

/* VI_CONTROL[1:0]: formato del framebuffer. */
enum salsa64_vi_type {
	SALSA64_VI_BLANK = 0,
	SALSA64_VI_RESERVED = 1,
	SALSA64_VI_RGBA5551 = 2,
	SALSA64_VI_RGBA8888 = 3,
};

#define SALSA64_VI_CONTROL_TYPE    0x0003u
#define SALSA64_VI_CONTROL_SERRATE 0x0040u  /* entrelazado */
#define SALSA64_VI_V_INTR_RESET    0x3FFu   /* nunca coincide: no interrumpe */

struct salsa64_vi {
	uint32_t regs[SALSA64_VI_NREGS];
	uint32_t clock_hz;      /* reloj del VI, según el tipo de TV */
	uint32_t lines;         /* líneas del campo en curso */
	uint32_t field;         /* 0/1: campo en curso (solo entrelazado) */
	uint32_t fields;        /* campos completos desde el reset */
	uint64_t field_start;   /* ciclo del VR4300 en que empezó el campo */
	uint64_t field_cycles;  /* duración del campo en curso */
	struct salsa64_sched *sched;
};

/* Imagen que saca el VI: la parte visible del framebuffer en la RDRAM. */
struct salsa64_vi_frame {
	enum salsa64_vi_type type;
	uint32_t origin;  /* dirección física de la primera línea */
	uint32_t width;   /* píxeles visibles por línea */
	uint32_t height;  /* líneas visibles */
	uint32_t stride;  /* bytes por línea (VI_WIDTH × bytes por píxel) */
	uint32_t bpp;     /* bytes por píxel: 2 o 4 */
};

enum salsa64_ai_reg {
	SALSA64_AI_DRAM_ADDR, SALSA64_AI_LEN, SALSA64_AI_CONTROL, SALSA64_AI_STATUS,
	SALSA64_AI_DACRATE, SALSA64_AI_BITRATE, SALSA64_AI_NREGS
};

enum salsa64_pi_reg {
	SALSA64_PI_DRAM_ADDR, SALSA64_PI_CART_ADDR, SALSA64_PI_RD_LEN,
	SALSA64_PI_WR_LEN, SALSA64_PI_STATUS, SALSA64_PI_BSD_DOM1_LAT,
	SALSA64_PI_BSD_DOM1_PWD, SALSA64_PI_BSD_DOM1_PGS, SALSA64_PI_BSD_DOM1_RLS,
	SALSA64_PI_BSD_DOM2_LAT, SALSA64_PI_BSD_DOM2_PWD, SALSA64_PI_BSD_DOM2_PGS,
	SALSA64_PI_BSD_DOM2_RLS, SALSA64_PI_NREGS
};

#define SALSA64_PI_STATUS_DMA_BUSY 0x01u
#define SALSA64_PI_STATUS_IO_BUSY  0x02u
#define SALSA64_PI_STATUS_INTR     0x08u

enum salsa64_si_reg {
	SALSA64_SI_DRAM_ADDR, SALSA64_SI_PIF_ADDR_RD64B, SALSA64_SI_R2,
	SALSA64_SI_R3, SALSA64_SI_PIF_ADDR_WR64B, SALSA64_SI_R5,
	SALSA64_SI_STATUS, SALSA64_SI_NREGS
};

#define SALSA64_SI_STATUS_INTR 0x1000u

#define SALSA64_RI_NREGS    8
#define SALSA64_RDRAM_NREGS    16
#define SALSA64_RDRAM_REGS_END 0x03F80000u  /* después: escrituras "broadcast" */

/* Interrupciones del MI. Actualizan Cause.IP2 del CPU. */
void salsa64_mi_raise(struct salsa64_bus *bus, enum salsa64_irq irq);
void salsa64_mi_clear(struct salsa64_bus *bus, enum salsa64_irq irq);

void salsa64_rcp_reset(struct salsa64_bus *bus);

uint32_t salsa64_mi_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_mi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_sp_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_sp_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_dp_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_dp_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_vi_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_vi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);

/* Temporización del VI (vi.c). salsa64_vi_init registra sus eventos en el
 * planificador; salsa64_vi_start empieza un campo en el ciclo actual del CPU
 * (después de cada reset). */
void salsa64_vi_init(struct salsa64_bus *bus, struct salsa64_sched *sched);
void salsa64_vi_start(struct salsa64_bus *bus);
/* Reloj del VI según el tipo de TV; se aplica desde el campo siguiente. */
void salsa64_vi_set_tv(struct salsa64_bus *bus, enum salsa64_tv_type tv);
/* Ciclos que faltan para que termine el campo en curso (al menos 1). */
uint64_t salsa64_vi_cycles_left(const struct salsa64_bus *bus);
/* Describe la imagen visible; false si el VI está apagado o sus registros no
 * forman una imagen dentro de la RDRAM. */
bool salsa64_vi_frame(const struct salsa64_bus *bus, struct salsa64_vi_frame *frame);
uint32_t salsa64_ai_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_ai_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_pi_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_pi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);
uint32_t salsa64_si_read(struct salsa64_bus *bus, uint32_t reg);
void salsa64_si_write(struct salsa64_bus *bus, uint32_t reg, uint32_t value);

#endif
