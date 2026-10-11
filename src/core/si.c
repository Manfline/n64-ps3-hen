/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — Serial Interface (SI): DMA de 64 bytes entre la RDRAM y la PIF RAM.
 *
 * El DMA es instantáneo y levanta la interrupción del SI. El PIF prepara los
 * comandos joybus al recibir una escritura y los ejecuta antes de cada
 * lectura (pif.c). Falta la latencia real (~2 ms en la consola).
 */
#include <string.h>

#include "core/bus.h"

static uint32_t si_dram(struct salsa64_bus *bus)
{
	uint32_t dram = bus->si[SALSA64_SI_DRAM_ADDR] & 0x00FFFFF8;
	return dram + SALSA64_PIF_RAM_SIZE <= bus->rdram_size ? dram : UINT32_MAX;
}

uint32_t salsa64_si_read(struct salsa64_bus *bus, uint32_t reg)
{
	uint32_t i = reg >> 2;
	if (i >= SALSA64_SI_NREGS)
		return 0;
	if (i == SALSA64_SI_STATUS)
		return (bus->mi.intr & (1u << SALSA64_IRQ_SI)) ? SALSA64_SI_STATUS_INTR : 0;
	return bus->si[i];
}

void salsa64_si_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	uint32_t i = reg >> 2, dram;
	switch (i) {
	case SALSA64_SI_DRAM_ADDR:
		bus->si[i] = v & 0x00FFFFFF;
		return;
	case SALSA64_SI_PIF_ADDR_RD64B:  /* PIF RAM → RDRAM */
		bus->si[i] = v;
		salsa64_pif_execute(bus);
		if ((dram = si_dram(bus)) != UINT32_MAX) {
			memcpy(bus->rdram + dram, bus->pif_ram, SALSA64_PIF_RAM_SIZE);
			salsa64_bus_rdram_written(bus, dram, SALSA64_PIF_RAM_SIZE);
		}
		salsa64_mi_raise(bus, SALSA64_IRQ_SI);
		return;
	case SALSA64_SI_PIF_ADDR_WR64B:  /* RDRAM → PIF RAM */
		bus->si[i] = v;
		if ((dram = si_dram(bus)) != UINT32_MAX) {
			memcpy(bus->pif_ram, bus->rdram + dram, SALSA64_PIF_RAM_SIZE);
			salsa64_pif_written(bus);
		}
		salsa64_mi_raise(bus, SALSA64_IRQ_SI);
		return;
	case SALSA64_SI_STATUS:
		salsa64_mi_clear(bus, SALSA64_IRQ_SI);
		return;
	default:
		return;
	}
}
