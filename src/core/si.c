/*
 * Retro64 — Serial Interface (SI): DMA de 64 bytes entre la RDRAM y la PIF RAM.
 *
 * Primer hito: el DMA es instantáneo y levanta la interrupción del SI. Falta
 * que el PIF procese los comandos (controles, Controller Pak, EEPROM) antes
 * de una lectura, y la latencia real (~2 ms en la consola).
 */
#include <string.h>

#include "core/bus.h"

static uint32_t si_dram(struct retro64_bus *bus)
{
	uint32_t dram = bus->si[RETRO64_SI_DRAM_ADDR] & 0x00FFFFF8;
	return dram + RETRO64_PIF_RAM_SIZE <= bus->rdram_size ? dram : UINT32_MAX;
}

uint32_t retro64_si_read(struct retro64_bus *bus, uint32_t reg)
{
	uint32_t i = reg >> 2;
	if (i >= RETRO64_SI_NREGS)
		return 0;
	if (i == RETRO64_SI_STATUS)
		return (bus->mi.intr & (1u << RETRO64_IRQ_SI)) ? RETRO64_SI_STATUS_INTR : 0;
	return bus->si[i];
}

void retro64_si_write(struct retro64_bus *bus, uint32_t reg, uint32_t v)
{
	uint32_t i = reg >> 2, dram;
	switch (i) {
	case RETRO64_SI_DRAM_ADDR:
		bus->si[i] = v & 0x00FFFFFF;
		return;
	case RETRO64_SI_PIF_ADDR_RD64B:  /* PIF RAM → RDRAM */
		bus->si[i] = v;
		if ((dram = si_dram(bus)) != UINT32_MAX)
			memcpy(bus->rdram + dram, bus->pif_ram, RETRO64_PIF_RAM_SIZE);
		retro64_mi_raise(bus, RETRO64_IRQ_SI);
		return;
	case RETRO64_SI_PIF_ADDR_WR64B:  /* RDRAM → PIF RAM */
		bus->si[i] = v;
		if ((dram = si_dram(bus)) != UINT32_MAX)
			memcpy(bus->pif_ram, bus->rdram + dram, RETRO64_PIF_RAM_SIZE);
		retro64_mi_raise(bus, RETRO64_IRQ_SI);
		return;
	case RETRO64_SI_STATUS:
		retro64_mi_clear(bus, RETRO64_IRQ_SI);
		return;
	default:
		return;
	}
}
