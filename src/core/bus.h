/*
 * Retro64 — bus físico de la N64.
 *
 * Mapa de direcciones físicas (32 bits):
 *   0x00000000–0x03EFFFFF  RDRAM (rdram_size bytes; el resto lee 0)
 *   0x03F00000–0x03FFFFFF  registros de la RDRAM
 *   0x04000000–0x0403FFFF  SP DMEM (0x0000) e IMEM (0x1000), espejo cada 8 KB
 *   0x04040000–0x040FFFFF  registros del SP; SP_PC en 0x04080000
 *   0x04100000             DP (RDP), comandos
 *   0x04300000 MI · 0x04400000 VI · 0x04500000 AI · 0x04600000 PI
 *   0x04700000 RI · 0x04800000 SI
 *   0x10000000–0x1FBFFFFF  ROM del cartucho (dominio 1, dirección 2)
 *   0x1FC00000–0x1FC007BF  PIF ROM: no se incluye (arranque por HLE), lee 0
 *   0x1FC007C0–0x1FC007FF  PIF RAM (64 bytes)
 * Todo lo demás lee 0 e ignora escrituras por ahora.
 *
 * El CPU accede directo a la RDRAM por su camino rápido; el bus atiende el
 * resto y también la RDRAM cuando lo llama otro componente.
 *
 * La memoria de la RDRAM la pone quien llama (preasignada al arrancar): en
 * la PS3 un búfer alineado a 128 bytes; en las pruebas, un arreglo estático.
 */
#ifndef RETRO64_CORE_BUS_H
#define RETRO64_CORE_BUS_H

#include "core/rcp.h"
#include "core/types.h"

#define RETRO64_RDRAM_MAX       (8u << 20)
#define RETRO64_SP_MEM_SIZE     0x2000u
#define RETRO64_PIF_RAM_SIZE    64u
#define RETRO64_PIF_RAM_ADDR    0x1FC007C0u
#define RETRO64_CART_ROM_ADDR   0x10000000u
#define RETRO64_CART_ROM_END    0x1FC00000u

struct retro64_cpu;

struct retro64_bus {
	uint8_t *rdram;
	uint32_t rdram_size;
	uint32_t rom_size;
	const uint8_t *rom;
	struct retro64_cpu *cpu;  /* destino de las interrupciones del MI */

	struct retro64_mi mi;
	struct retro64_sp sp;
	uint32_t dp[RETRO64_DP_NREGS];
	uint32_t vi[RETRO64_VI_NREGS];
	uint32_t ai[RETRO64_AI_NREGS];
	uint32_t pi[RETRO64_PI_NREGS];
	uint32_t ri[RETRO64_RI_NREGS];
	uint32_t si[RETRO64_SI_NREGS];
	uint32_t rdram_regs[RETRO64_RDRAM_NREGS];

	uint8_t sp_mem[RETRO64_SP_MEM_SIZE] __attribute__((aligned(128)));
	uint8_t pif_ram[RETRO64_PIF_RAM_SIZE] __attribute__((aligned(16)));
};

/* rdram_size: 4 u 8 MB (Expansion Pak). */
void retro64_bus_init(struct retro64_bus *bus, uint8_t *rdram, uint32_t rdram_size);
void retro64_bus_reset(struct retro64_bus *bus);
void retro64_bus_set_rom(struct retro64_bus *bus, const uint8_t *rom, uint32_t size);

/* Accesos alineados. `mask` marca los bytes a escribir dentro de la palabra,
 * en el orden de la N64 (0xFF000000 = byte en paddr+0). Los registros del
 * RCP reciben la palabra completa con los bytes fuera de la máscara en 0. */
uint32_t retro64_bus_read32(struct retro64_bus *bus, uint32_t paddr);
uint64_t retro64_bus_read64(struct retro64_bus *bus, uint32_t paddr);
void retro64_bus_write32(struct retro64_bus *bus, uint32_t paddr, uint32_t value, uint32_t mask);
void retro64_bus_write64(struct retro64_bus *bus, uint32_t paddr, uint64_t value, uint64_t mask);

#endif
