/*
 * Retro64 — bus físico de la N64. Ver bus.h.
 */
#include "core/bus.h"

#include <string.h>

#include "core/endian.h"

void retro64_bus_init(struct retro64_bus *bus, uint8_t *rdram, uint32_t rdram_size)
{
	memset(bus, 0, sizeof *bus);
	bus->rdram = rdram;
	bus->rdram_size = rdram_size > RETRO64_RDRAM_MAX ? RETRO64_RDRAM_MAX : rdram_size;
	retro64_bus_reset(bus);
}

void retro64_bus_reset(struct retro64_bus *bus)
{
	memset(bus->rdram, 0, bus->rdram_size);
	memset(bus->sp_mem, 0, sizeof bus->sp_mem);
	memset(bus->pif_ram, 0, sizeof bus->pif_ram);
	memset(bus->rdram_regs, 0, sizeof bus->rdram_regs);
	memset(bus->ri, 0, sizeof bus->ri);
	retro64_rcp_reset(bus);
}

void retro64_bus_set_rom(struct retro64_bus *bus, const uint8_t *rom, uint32_t size)
{
	uint32_t max = RETRO64_CART_ROM_END - RETRO64_CART_ROM_ADDR;
	bus->rom = rom;
	bus->rom_size = size > max ? max : size;
}

/* Registros del RCP: página de 1 MB dentro de 0x04000000–0x048FFFFF. */
static uint32_t rcp_read(struct retro64_bus *bus, uint32_t paddr)
{
	uint32_t reg = paddr & 0xFFFFF;
	switch ((paddr >> 20) & 0xF) {
	case 0x0:
		if (reg < 0x40000)
			return retro64_rd32(bus->sp_mem + (reg & (RETRO64_SP_MEM_SIZE - 1)));
		return retro64_sp_read(bus, reg);
	case 0x1: return retro64_dp_read(bus, reg);
	case 0x3: return retro64_mi_read(bus, reg);
	case 0x4: return retro64_vi_read(bus, reg);
	case 0x5: return retro64_ai_read(bus, reg);
	case 0x6: return retro64_pi_read(bus, reg);
	case 0x7: return reg < RETRO64_RI_NREGS * 4 ? bus->ri[reg >> 2] : 0;
	case 0x8: return retro64_si_read(bus, reg);
	default:  return 0;  /* DP span (0x042) y huecos */
	}
}

static void rcp_write(struct retro64_bus *bus, uint32_t paddr, uint32_t value, uint32_t mask)
{
	uint32_t reg = paddr & 0xFFFFF;
	switch ((paddr >> 20) & 0xF) {
	case 0x0:
		if (reg < 0x40000) {
			uint8_t *p = bus->sp_mem + (reg & (RETRO64_SP_MEM_SIZE - 1));
			retro64_wr32(p, (retro64_rd32(p) & ~mask) | (value & mask));
			return;
		}
		retro64_sp_write(bus, reg, value & mask);
		return;
	case 0x1: retro64_dp_write(bus, reg, value & mask); return;
	case 0x3: retro64_mi_write(bus, reg, value & mask); return;
	case 0x4: retro64_vi_write(bus, reg, value & mask); return;
	case 0x5: retro64_ai_write(bus, reg, value & mask); return;
	case 0x6: retro64_pi_write(bus, reg, value & mask); return;
	case 0x7:
		if (reg < RETRO64_RI_NREGS * 4)
			bus->ri[reg >> 2] = value & mask;
		return;
	case 0x8: retro64_si_write(bus, reg, value & mask); return;
	default:  return;
	}
}

uint32_t retro64_bus_read32(struct retro64_bus *bus, uint32_t paddr)
{
	paddr &= ~3u;
	if (paddr < bus->rdram_size)
		return retro64_rd32(bus->rdram + paddr);
	if (paddr < 0x03F00000)
		return 0;
	if (paddr < 0x04000000) {
		uint32_t reg = (paddr & 0xFFFFF) >> 2;
		return reg < RETRO64_RDRAM_NREGS ? bus->rdram_regs[reg] : 0;
	}
	if (paddr < 0x04900000)
		return rcp_read(bus, paddr);
	if (paddr >= RETRO64_CART_ROM_ADDR && paddr < RETRO64_CART_ROM_END) {
		uint32_t off = paddr - RETRO64_CART_ROM_ADDR;
		return off < bus->rom_size ? retro64_rd32(bus->rom + off) : 0;
	}
	if (paddr >= RETRO64_PIF_RAM_ADDR && paddr < RETRO64_PIF_RAM_ADDR + RETRO64_PIF_RAM_SIZE)
		return retro64_rd32(bus->pif_ram + (paddr - RETRO64_PIF_RAM_ADDR));
	return 0;
}

void retro64_bus_write32(struct retro64_bus *bus, uint32_t paddr, uint32_t value, uint32_t mask)
{
	paddr &= ~3u;
	if (paddr < bus->rdram_size) {
		uint8_t *p = bus->rdram + paddr;
		retro64_wr32(p, (retro64_rd32(p) & ~mask) | (value & mask));
		return;
	}
	if (paddr < 0x03F00000)
		return;
	if (paddr < 0x04000000) {
		uint32_t reg = (paddr & 0xFFFFF) >> 2;
		if (reg < RETRO64_RDRAM_NREGS)
			bus->rdram_regs[reg] = value & mask;
		return;
	}
	if (paddr < 0x04900000) {
		rcp_write(bus, paddr, value, mask);
		return;
	}
	if (paddr >= RETRO64_PIF_RAM_ADDR && paddr < RETRO64_PIF_RAM_ADDR + RETRO64_PIF_RAM_SIZE) {
		uint8_t *p = bus->pif_ram + (paddr - RETRO64_PIF_RAM_ADDR);
		retro64_wr32(p, (retro64_rd32(p) & ~mask) | (value & mask));
		return;
	}
	/* ROM del cartucho y huecos: se ignora. */
}

uint64_t retro64_bus_read64(struct retro64_bus *bus, uint32_t paddr)
{
	paddr &= ~7u;
	if (paddr < bus->rdram_size)
		return retro64_rd64(bus->rdram + paddr);
	return (uint64_t)retro64_bus_read32(bus, paddr) << 32 | retro64_bus_read32(bus, paddr + 4);
}

void retro64_bus_write64(struct retro64_bus *bus, uint32_t paddr, uint64_t value, uint64_t mask)
{
	paddr &= ~7u;
	if (paddr < bus->rdram_size) {
		uint8_t *p = bus->rdram + paddr;
		retro64_wr64(p, (retro64_rd64(p) & ~mask) | (value & mask));
		return;
	}
	if (mask >> 32)
		retro64_bus_write32(bus, paddr, (uint32_t)(value >> 32), (uint32_t)(mask >> 32));
	if ((uint32_t)mask)
		retro64_bus_write32(bus, paddr + 4, (uint32_t)value, (uint32_t)mask);
}
