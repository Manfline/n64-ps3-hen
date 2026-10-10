/*
 * Salsa64 — Peripheral Interface (PI): DMA entre el cartucho y la RDRAM.
 *
 * Primer hito: el DMA cartucho → RDRAM (PI_WR_LEN) se completa al instante y
 * levanta la interrupción del PI. Faltan las latencias del bus (dominios 1 y
 * 2), el DMA RDRAM → cartucho (SRAM, FlashRAM) y el estado "ocupado".
 */
#include <string.h>

#include "core/bus.h"

static void pi_dma_to_rdram(struct salsa64_bus *bus, uint32_t len)
{
	uint32_t dram = bus->pi[SALSA64_PI_DRAM_ADDR] & 0x00FFFFFE;
	uint32_t cart = bus->pi[SALSA64_PI_CART_ADDR] & 0xFFFFFFFE;

	/* Solo la ROM (dominio 1, dirección 2) tiene datos por ahora; lo que
	 * caiga fuera de la ROM o de la RDRAM se lee como 0 o se descarta. */
	for (uint32_t done = 0; done < len;) {
		uint32_t d = dram + done, c = cart + done;
		if (d >= bus->rdram_size)
			break;
		uint32_t chunk = len - done;
		if (chunk > bus->rdram_size - d)
			chunk = bus->rdram_size - d;
		uint32_t off = c - SALSA64_CART_ROM_ADDR;
		if (c >= SALSA64_CART_ROM_ADDR && off < bus->rom_size) {
			if (chunk > bus->rom_size - off)
				chunk = bus->rom_size - off;
			memcpy(bus->rdram + d, bus->rom + off, chunk);
		} else {
			uint32_t gap = chunk;
			if (c < SALSA64_CART_ROM_ADDR && SALSA64_CART_ROM_ADDR - c < gap)
				gap = SALSA64_CART_ROM_ADDR - c;
			memset(bus->rdram + d, 0, gap);
			chunk = gap;
		}
		done += chunk;
	}
	if (dram < bus->rdram_size)
		salsa64_bus_rdram_written(bus, dram, len);

	bus->pi[SALSA64_PI_DRAM_ADDR] = (dram + len + 7) & 0x00FFFFF8;
	bus->pi[SALSA64_PI_CART_ADDR] = (cart + len + 1) & 0xFFFFFFFE;
	salsa64_mi_raise(bus, SALSA64_IRQ_PI);
}

uint32_t salsa64_pi_read(struct salsa64_bus *bus, uint32_t reg)
{
	uint32_t i = reg >> 2;
	if (i >= SALSA64_PI_NREGS)
		return 0;
	if (i == SALSA64_PI_STATUS)
		return (bus->mi.intr & (1u << SALSA64_IRQ_PI)) ? SALSA64_PI_STATUS_INTR : 0;
	return bus->pi[i];
}

void salsa64_pi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	uint32_t i = reg >> 2;
	switch (i) {
	case SALSA64_PI_DRAM_ADDR:
		bus->pi[i] = v & 0x00FFFFFE;
		return;
	case SALSA64_PI_CART_ADDR:
		bus->pi[i] = v & 0xFFFFFFFE;
		return;
	case SALSA64_PI_RD_LEN:
		/* RDRAM → cartucho: SRAM/FlashRAM llegarán después. */
		bus->pi[i] = v & 0x00FFFFFF;
		salsa64_mi_raise(bus, SALSA64_IRQ_PI);
		return;
	case SALSA64_PI_WR_LEN:
		bus->pi[i] = v & 0x00FFFFFF;
		pi_dma_to_rdram(bus, (v & 0x00FFFFFF) + 1);
		return;
	case SALSA64_PI_STATUS:
		if (v & 0x2)
			salsa64_mi_clear(bus, SALSA64_IRQ_PI);
		return;
	default:
		if (i < SALSA64_PI_NREGS)
			bus->pi[i] = v & 0xFF;  /* latencias y anchos de pulso BSD */
		return;
	}
}
