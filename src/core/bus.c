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
	if (bus->isv)
		memset(bus->isv, 0, RETRO64_ISV_SIZE);
	retro64_rcp_reset(bus);
}

void retro64_bus_set_rom(struct retro64_bus *bus, const uint8_t *rom, uint32_t size)
{
	uint32_t max = RETRO64_CART_ROM_END - RETRO64_CART_ROM_ADDR;
	bus->rom = rom;
	bus->rom_size = size > max ? max : size;
}

void retro64_bus_set_isviewer(struct retro64_bus *bus, uint8_t *buf, retro64_isv_fn out, void *ctx)
{
	bus->isv = buf;
	bus->isv_out = out;
	bus->isv_ctx = ctx;
	if (buf)
		memset(buf, 0, RETRO64_ISV_SIZE);
}

static inline bool isv_hit(const struct retro64_bus *bus, uint32_t paddr)
{
	return bus->isv && paddr - RETRO64_ISV_ADDR < RETRO64_ISV_SIZE;
}

static void isv_write(struct retro64_bus *bus, uint32_t off, uint32_t value, uint32_t mask)
{
	uint8_t *p = bus->isv + off;
	retro64_wr32(p, (retro64_rd32(p) & ~mask) | (value & mask));
	if (off == RETRO64_ISV_WRITE_LEN && bus->isv_out) {
		uint32_t len = retro64_rd32(p);
		if (len > RETRO64_ISV_SIZE - RETRO64_ISV_BUFFER)
			len = RETRO64_ISV_SIZE - RETRO64_ISV_BUFFER;
		bus->isv_out(bus->isv_ctx, bus->isv + RETRO64_ISV_BUFFER, len);
	}
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

static void rcp_write(struct retro64_bus *bus, uint32_t paddr, uint32_t value)
{
	uint32_t reg = paddr & 0xFFFFF;
	switch ((paddr >> 20) & 0xF) {
	case 0x0:
		if (reg < 0x40000) {
			retro64_wr32(bus->sp_mem + (reg & (RETRO64_SP_MEM_SIZE - 1)), value);
			return;
		}
		retro64_sp_write(bus, reg, value);
		return;
	case 0x1: retro64_dp_write(bus, reg, value); return;
	case 0x3: retro64_mi_write(bus, reg, value); return;
	case 0x4: retro64_vi_write(bus, reg, value); return;
	case 0x5: retro64_ai_write(bus, reg, value); return;
	case 0x6: retro64_pi_write(bus, reg, value); return;
	case 0x7:
		if (reg < RETRO64_RI_NREGS * 4)
			bus->ri[reg >> 2] = value;
		return;
	case 0x8: retro64_si_write(bus, reg, value); return;
	default:  return;
	}
}

/* Bus PI de 16 bits: el RCP lee dos medias palabras desde la dirección par,
 * así que una lectura de 8/16 bits en paddr & 2 trae la palabra que empieza
 * ahí (n64brew.dev: Memory map, "PI external bus"). */
static uint32_t pi_bus_read(const struct retro64_bus *bus, uint32_t paddr)
{
	paddr &= ~1u;
	if (isv_hit(bus, paddr) && paddr + 4 <= RETRO64_ISV_ADDR + RETRO64_ISV_SIZE)
		return retro64_rd32(bus->isv + (paddr - RETRO64_ISV_ADDR));
	uint32_t off = paddr - RETRO64_CART_ROM_ADDR, v = 0;
	for (unsigned k = 0; k < 4; k++)
		v = v << 8 | (off + k < bus->rom_size ? bus->rom[off + k] : 0);
	return v;
}

uint32_t retro64_bus_read32(struct retro64_bus *bus, uint32_t paddr)
{
	if (paddr >= RETRO64_CART_ROM_ADDR && paddr < RETRO64_CART_ROM_END)
		return pi_bus_read(bus, paddr);
	paddr &= ~3u;
	if (paddr < bus->rdram_size)
		return retro64_rd32(bus->rdram + paddr);
	if (paddr < 0x03F00000)
		return 0;
	if (paddr < 0x04000000)
		return paddr < RETRO64_RDRAM_REGS_END ? bus->rdram_regs[(paddr >> 2) & (RETRO64_RDRAM_NREGS - 1)] : 0;
	if (paddr < 0x04900000)
		return rcp_read(bus, paddr);
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
	/* Fuera de la RDRAM el tamaño del acceso se ignora: se escribe la palabra
	 * del bus completa. El ISViewer es la excepción (dispositivo de
	 * depuración que se usa byte a byte). */
	if (paddr < 0x03F00000)
		return;
	if (paddr < 0x04000000) {
		if (paddr < RETRO64_RDRAM_REGS_END)
			bus->rdram_regs[(paddr >> 2) & (RETRO64_RDRAM_NREGS - 1)] = value;
		return;
	}
	if (paddr < 0x04900000) {
		rcp_write(bus, paddr, value);
		return;
	}
	if (isv_hit(bus, paddr)) {
		isv_write(bus, paddr - RETRO64_ISV_ADDR, value, mask);
		return;
	}
	if (paddr >= RETRO64_PIF_RAM_ADDR && paddr < RETRO64_PIF_RAM_ADDR + RETRO64_PIF_RAM_SIZE) {
		retro64_wr32(bus->pif_ram + (paddr - RETRO64_PIF_RAM_ADDR), value);
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
	/* El VR4300 pone primero la palabra alta y el RCP solo escribe esa. */
	retro64_bus_write32(bus, paddr, (uint32_t)(value >> 32), (uint32_t)(mask >> 32));
}
