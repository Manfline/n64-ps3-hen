/*
 * Salsa64 — registros del RSP (SP) y del RDP (DP, comandos).
 *
 * Primer hito: SP_STATUS (halt, broke, interrupción, señales) y el semáforo
 * se comportan como en la consola; el DMA del SP no existe todavía y el RSP
 * no ejecuta nada (Fase 2, SPE 0). Mientras tanto, quitarle HALT lo detiene
 * en el acto como si hubiera ejecutado BREAK (BROKE y, si está habilitada, la
 * interrupción): así ningún programa espera para siempre a que termine.
 * Los registros del DP solo se guardan.
 */
#include "core/bus.h"

#define SP_PC_REG 0x80000u

uint32_t salsa64_sp_read(struct salsa64_bus *bus, uint32_t reg)
{
	if (reg == SP_PC_REG)
		return bus->sp.pc;
	if ((reg & ~0x1Fu) != 0x40000)
		return 0;

	uint32_t i = (reg >> 2) & 7;
	if (i == SALSA64_SP_SEMAPHORE) {
		uint32_t v = bus->sp.regs[i];
		bus->sp.regs[i] = 1;
		return v;
	}
	if (i == SALSA64_SP_DMA_FULL || i == SALSA64_SP_DMA_BUSY)
		return 0;
	return bus->sp.regs[i];
}

static void sp_write_status(struct salsa64_bus *bus, uint32_t v)
{
	uint32_t s = bus->sp.regs[SALSA64_SP_STATUS];

	if (v & 0x001) s &= ~SALSA64_SP_STATUS_HALT;
	if (v & 0x002) s |= SALSA64_SP_STATUS_HALT;
	if (v & 0x004) s &= ~SALSA64_SP_STATUS_BROKE;
	if (v & 0x008) salsa64_mi_clear(bus, SALSA64_IRQ_SP);
	if (v & 0x010) salsa64_mi_raise(bus, SALSA64_IRQ_SP);
	if (v & 0x020) s &= ~0x20u;  /* single step */
	if (v & 0x040) s |= 0x20;
	if (v & 0x080) s &= ~0x40u;  /* interrupción en BREAK */
	if (v & 0x100) s |= 0x40;
	/* Señales 0–7: bits de estado 7–14, pares (limpiar, poner) desde el 9. */
	for (int i = 0; i < 8; i++) {
		if (v & (0x200u << (2 * i)))
			s &= ~(0x80u << i);
		if (v & (0x400u << (2 * i)))
			s |= 0x80u << i;
	}
	/* RSP provisional: arrancar es detenerse en un BREAK inmediato. */
	if (!(s & SALSA64_SP_STATUS_HALT)) {
		s |= SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE;
		if (s & 0x40)
			salsa64_mi_raise(bus, SALSA64_IRQ_SP);
	}
	bus->sp.regs[SALSA64_SP_STATUS] = s;
}

void salsa64_sp_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	if (reg == SP_PC_REG) {
		bus->sp.pc = v & 0xFFC;
		return;
	}
	if ((reg & ~0x1Fu) != 0x40000)
		return;

	uint32_t i = (reg >> 2) & 7;
	switch (i) {
	case SALSA64_SP_STATUS:
		sp_write_status(bus, v);
		return;
	case SALSA64_SP_SEMAPHORE:
		bus->sp.regs[i] = 0;
		return;
	case SALSA64_SP_DMA_FULL:
	case SALSA64_SP_DMA_BUSY:
		return;
	default:
		bus->sp.regs[i] = v;  /* DMA del SP: Fase 2 */
		return;
	}
}

uint32_t salsa64_dp_read(struct salsa64_bus *bus, uint32_t reg)
{
	uint32_t i = reg >> 2;
	return i < SALSA64_DP_NREGS ? bus->dp[i] : 0;
}

void salsa64_dp_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	uint32_t i = reg >> 2;
	if (i < SALSA64_DP_NREGS && i != SALSA64_DP_CURRENT)
		bus->dp[i] = v;
}
