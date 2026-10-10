/*
 * Salsa64 — bus físico de la N64.
 *
 * Mapa de direcciones físicas (32 bits):
 *   0x00000000–0x03EFFFFF  RDRAM (rdram_size bytes; el resto lee 0)
 *   0x03F00000–0x03FFFFFF  registros de la RDRAM (16, repetidos cada 0x40 bytes
 *                          hasta SALSA64_RDRAM_REGS_END)
 *   0x04000000–0x0403FFFF  SP DMEM (0x0000) e IMEM (0x1000), espejo cada 8 KB
 *   0x04040000–0x040FFFFF  registros del SP; SP_PC en 0x04080000
 *   0x04100000             DP (RDP), comandos
 *   0x04300000 MI · 0x04400000 VI · 0x04500000 AI · 0x04600000 PI
 *   0x04700000 RI · 0x04800000 SI
 *   0x08000000–0x0FFFFFFF  SRAM del cartucho (dominio 2), si la hay (save.c)
 *   0x10000000–0x1FBFFFFF  ROM del cartucho (dominio 1, dirección 2)
 *   0x13FF0000–0x13FFFFFF  ISViewer, solo si se activa (tapa la ROM ahí)
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
#ifndef SALSA64_CORE_BUS_H
#define SALSA64_CORE_BUS_H

#include "core/pif.h"
#include "core/rcp.h"
#include "core/types.h"

#define SALSA64_RDRAM_MAX       (8u << 20)
#define SALSA64_SP_MEM_SIZE     0x2000u
#define SALSA64_PIF_RAM_SIZE    64u
#define SALSA64_PIF_RAM_ADDR    0x1FC007C0u
#define SALSA64_CART_ROM_ADDR   0x10000000u
#define SALSA64_CART_ROM_END    0x1FC00000u

/* ISViewer (IS-Viewer 64): consola de depuración de los kits de desarrollo
 * que imitan los emuladores. Las ROMs escriben texto en el búfer (desde
 * +0x20) y su longitud en +0x14; esa escritura lo imprime. Ocupa los últimos
 * 64 KB del espacio de una ROM de 64 MB, por eso está apagado por omisión. */
#define SALSA64_ISV_ADDR        0x13FF0000u
#define SALSA64_ISV_SIZE        0x10000u
#define SALSA64_ISV_WRITE_LEN   0x14u
#define SALSA64_ISV_BUFFER      0x20u

typedef void (*salsa64_isv_fn)(void *ctx, const uint8_t *text, uint32_t len);

struct salsa64_cpu;
struct salsa64_save;

struct salsa64_bus {
	uint8_t *rdram;
	uint32_t rdram_size;
	uint32_t rom_size;
	const uint8_t *rom;
	struct salsa64_cpu *cpu;  /* destino de las interrupciones del MI */
	uint8_t *isv;             /* búfer del ISViewer o NULL (apagado) */
	salsa64_isv_fn isv_out;
	void *isv_ctx;
	struct salsa64_save *save;  /* guardado del cartucho o NULL (save.c) */

	struct salsa64_mi mi;
	struct salsa64_sp sp;
	struct salsa64_rsp rsp;
	uint32_t dp[SALSA64_DP_NREGS];
	struct salsa64_vi vi;
	struct salsa64_ai ai;
	uint32_t pi[SALSA64_PI_NREGS];
	uint32_t ri[SALSA64_RI_NREGS];
	uint32_t si[SALSA64_SI_NREGS];
	uint32_t rdram_regs[SALSA64_RDRAM_NREGS];
	struct salsa64_pif pif;

	uint8_t sp_mem[SALSA64_SP_MEM_SIZE] __attribute__((aligned(128)));
	uint8_t pif_ram[SALSA64_PIF_RAM_SIZE] __attribute__((aligned(16)));
};

/* rdram_size: 4 u 8 MB (Expansion Pak). */
void salsa64_bus_init(struct salsa64_bus *bus, uint8_t *rdram, uint32_t rdram_size);
void salsa64_bus_reset(struct salsa64_bus *bus);
void salsa64_bus_set_rom(struct salsa64_bus *bus, const uint8_t *rom, uint32_t size);
/* Activa el ISViewer con un búfer de SALSA64_ISV_SIZE bytes que pone quien
 * llama (NULL lo apaga). `out` recibe cada texto impreso. */
void salsa64_bus_set_isviewer(struct salsa64_bus *bus, uint8_t *buf, salsa64_isv_fn out, void *ctx);

/* Accesos de 32 y 64 bits. `mask` marca los bytes a escribir dentro de la
 * palabra, en el orden de la N64 (0xFF000000 = byte en paddr+0), pero solo la
 * RDRAM (y el ISViewer) lo respetan: como en la consola, el RCP, la PIF y el
 * bus PI ignoran el tamaño del acceso y escriben la palabra completa que puso
 * el CPU (con SB/SH, el registro desplazado a su carril), y de una escritura
 * de 64 bits solo la palabra alta (n64brew.dev: Memory map). Las lecturas del
 * bus PI (ROM) de una dirección no alineada a 4 devuelven la palabra que
 * empieza en la dirección par. */
uint32_t salsa64_bus_read32(struct salsa64_bus *bus, uint32_t paddr);
uint64_t salsa64_bus_read64(struct salsa64_bus *bus, uint32_t paddr);
void salsa64_bus_write32(struct salsa64_bus *bus, uint32_t paddr, uint32_t value, uint32_t mask);
void salsa64_bus_write64(struct salsa64_bus *bus, uint32_t paddr, uint64_t value, uint64_t mask);

/* La RDRAM [pa, pa + len) cambió sin pasar por el CPU (DMA, cargador,
 * pruebas): el recompilador descarta el código que dependa de ella. */
void salsa64_bus_rdram_written(struct salsa64_bus *bus, uint32_t pa, uint32_t len);

#endif
