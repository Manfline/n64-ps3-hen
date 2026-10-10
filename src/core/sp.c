/*
 * Salsa64 — registros del RSP (SP) y del RDP (DP, comandos).
 *
 * SP_STATUS (halt, broke, interrupción, señales) y el semáforo se comportan
 * como en la consola. El DMA del SP entre la RDRAM y la DMEM/IMEM es
 * inmediato. El RSP no ejecuta microcódigo: quitarle HALT le pasa la tarea
 * de la DMEM a rsp.c, que la procesa por HLE o, si no la reconoce, lo
 * detiene en el acto como si hubiera ejecutado BREAK (BROKE y, si está
 * habilitada, la interrupción), para que nadie espere para siempre.
 * Los registros del DP solo se guardan.
 */
#include "core/bus.h"

#include <string.h>

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
	/* RD_LEN y WR_LEN son el mismo registro al leerlos. */
	if (i == SALSA64_SP_WR_LEN)
		i = SALSA64_SP_RD_LEN;
	return bus->sp.regs[i];
}

/* DMA del SP (n64brew.dev: RSP, "DMA"): `count` filas de `len` bytes
 * (múltiplos de 8), con `skip` bytes entre filas en la RDRAM; la dirección
 * de la DMEM/IMEM da la vuelta dentro de su banco de 4 KB. Al terminar,
 * las direcciones quedan donde acabó la copia y el largo se lee como 0xFF8
 * con cuenta 0. */
static void sp_dma(struct salsa64_bus *bus, uint32_t v, bool to_rdram)
{
	uint32_t len = ((v & 0xFFF) | 7) + 1;
	uint32_t count = ((v >> 12) & 0xFF) + 1;
	uint32_t skip = (v >> 20) & 0xFF8;
	uint32_t mem = bus->sp.regs[SALSA64_SP_MEM_ADDR] & 0x1FF8;
	uint32_t bank = mem & 0x1000, off = mem & 0xFF8;
	uint32_t dram = bus->sp.regs[SALSA64_SP_DRAM_ADDR] & 0xFFFFF8;

	for (uint32_t r = 0; r < count; r++) {
		uint32_t row = dram;
		for (uint32_t k = 0; k < len; k += 8) {
			uint8_t *m = bus->sp_mem + bank + off;
			if (to_rdram) {
				if (dram + 8 <= bus->rdram_size)
					memcpy(bus->rdram + dram, m, 8);
			} else if (dram + 8 <= bus->rdram_size) {
				memcpy(m, bus->rdram + dram, 8);
			} else {
				memset(m, 0, 8);
			}
			off = (off + 8) & 0xFF8;
			dram = (dram + 8) & 0xFFFFF8;
		}
		if (to_rdram)
			salsa64_bus_rdram_written(bus, row, len);  /* código recompilado ahí: descartarlo */
		dram = (dram + skip) & 0xFFFFF8;
	}
	bus->sp.regs[SALSA64_SP_MEM_ADDR] = bank | off;
	bus->sp.regs[SALSA64_SP_DRAM_ADDR] = dram;
	bus->sp.regs[SALSA64_SP_RD_LEN] = (v & 0xFFF00000u) | 0xFF8;
}

static void sp_write_status(struct salsa64_bus *bus, uint32_t v)
{
	uint32_t s = bus->sp.regs[SALSA64_SP_STATUS];
	uint32_t was_halted = s & SALSA64_SP_STATUS_HALT;

	if (v & 0x001) s &= ~SALSA64_SP_STATUS_HALT;
	if (v & 0x002) s |= SALSA64_SP_STATUS_HALT;
	if (v & 0x004) s &= ~SALSA64_SP_STATUS_BROKE;
	if (v & 0x008) salsa64_mi_clear(bus, SALSA64_IRQ_SP);
	if (v & 0x010) salsa64_mi_raise(bus, SALSA64_IRQ_SP);
	if (v & 0x020) s &= ~0x20u;  /* single step */
	if (v & 0x040) s |= 0x20;
	if (v & 0x080) s &= ~SALSA64_SP_STATUS_INTR_BREAK;
	if (v & 0x100) s |= SALSA64_SP_STATUS_INTR_BREAK;
	/* Señales 0–7: bits de estado 7–14, pares (limpiar, poner) desde el 9. */
	for (int i = 0; i < 8; i++) {
		if (v & (0x200u << (2 * i)))
			s &= ~SALSA64_SP_STATUS_SIG(i);
		if (v & (0x400u << (2 * i)))
			s |= SALSA64_SP_STATUS_SIG(i);
	}
	bus->sp.regs[SALSA64_SP_STATUS] = s;
	if (was_halted && !(s & SALSA64_SP_STATUS_HALT))
		salsa64_rsp_start(bus);
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
	case SALSA64_SP_RD_LEN:
		sp_dma(bus, v, false);
		return;
	case SALSA64_SP_WR_LEN:
		sp_dma(bus, v, true);
		return;
	default:
		bus->sp.regs[i] = v;
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
