/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — arranque HLE. Ver boot.h y docs/cpu-vr4300.md ("Arranque HLE").
 *
 * Fuentes (documentación pública, ningún código de Nintendo):
 *   - n64brew.dev: "Initial Program Load" (IPL1: Status, Config, VI_V_INTR,
 *     sp), "PIF-NUS" (s3–s7 y semillas), "ROM Header" (dirección de arranque,
 *     tiempos de PI DOM1), "CIC-NUS" (ajuste de la dirección por variante),
 *     "RDRAM Interface" (RI_SELECT).
 *   - libdragon, boot/README.md, ipl3.c, loader.c y cleanup.c (Unlicense):
 *     búsqueda y carga del ELF, boot flags en DMEM, $sp y variables de
 *     libultra en 0x80000300.
 *   - CIC 6105: solo direcciones y tamaños, de varias fuentes cotejadas
 *     (docs/THIRD_PARTY.md); los bytes los pone el cartucho del usuario.
 *   - CIC 6103: lo mismo para su copia a la RDRAM (docs/THIRD_PARTY.md).
 */
#include "core/boot.h"

#include <string.h>

#include "core/endian.h"

#define STATUS_BOOT      0x34000000u  /* CU1 | CU0 | FR, lo que escribe el IPL1 */
#define RI_SELECT_BOOT   0x14u        /* TSEL = 1, RSEL = 4 */
#define VI_V_INTR_BOOT   0x3FFu
#define FLAT_LOAD_SIZE   0x100000u    /* 1 MB, fijo en el IPL3 de Nintendo */
#define FLAT_SP          0xFFFFFFFFA4001FF0ull
/* Count al saltar al juego con un IPL3 de Nintendo: lo pone en 0 al empezar
 * (visto corriendo el IPL3 del cartucho) y tarda unos 500 ms en frío
 * (n64brew.dev: IPL3), a 46,875 MHz. libultra mide el tiempo desde ahí:
 * osContInit duerme hasta que osGetTime() llega a 0,5 s, y en la consola
 * ese plazo ya pasó. Con 0 dormía, y como su osSetTimer pierde lo que
 * llevaba el temporizador cada vez que otro más corto se pone delante
 * (Conker arma uno de 20 000 cuentas cada dos campos para el audio), la
 * espera de 0,4 s se volvía de 29 s de pantalla negra. */
#define FLAT_IPL3_COUNT  23437500u
#define ELF_SEARCH_LIMIT (64u << 20)  /* el IPL3 de libdragon busca en 64 MB */
#define ELF_MAGIC        0x7F454C46u
#define PT_LOAD          1u
#define PT_N64_DECOMP    0x64E36341u
#define PF_N64_COMPRESSED 0x1000u

/* CIC 6103: copia de su imagen en DMEM a la RDRAM baja; DMEM e IMEM en
 * 0xFF salvo el número del CIC en IMEM 0x000. */
#define X103_COPY_SRC    0x4C0u      /* DMEM, = ROM */
#define X103_COPY_END    0x764u
#define X103_COPY_DST    0x000u
#define X103_IMEM_CIC    6103u

/* CIC 6105: copia de su imagen en DMEM a la RDRAM y DMA con paso del RSP. */
#define X105_COPY_SRC    0x554u      /* DMEM, = ROM (el IPL2 copia 0x40–0xFFF) */
#define X105_COPY_END    0x888u
#define X105_COPY_DST    0x004u
#define X105_RSP_SRC     0x1E8u      /* RDRAM → IMEM 0x120, 0x1F0 bytes */
#define X105_RSP_IMEM    0x120u
#define X105_RSP_LEN     0x1F0u
#define X105_RSP_DST     0x2FB1F0u   /* SP_WR_LEN = 0xFE817000 */
#define X105_RSP_ROWS    24u
#define X105_RSP_ROW     8u
#define X105_RSP_STRIDE  0xFF0u
#define X105_MEMSIZE     0x3F0u
#define X105_SPMEM_FILL  0xA4002000u /* DMEM e IMEM enteras al terminar */

enum { REG_S3 = 19, REG_S4, REG_S5, REG_S6, REG_S7, REG_SP = 29 };

static bool fail(struct salsa64_boot_info *info, const char *msg)
{
	info->error = msg;
	return false;
}

/* RDRAM física de una dirección de KSEG0/KSEG1, si [va, va + len) cabe. */
static bool rdram_range(const struct salsa64_bus *bus, uint32_t va, uint32_t len, uint32_t *pa)
{
	if ((va >> 30) != 2)  /* 0x80000000–0xBFFFFFFF */
		return false;
	uint32_t p = va & 0x1FFFFFFF;
	if (p > bus->rdram_size || len > bus->rdram_size - p)
		return false;
	*pa = p;
	return true;
}

/* Estado que no depende del IPL3: el que dejan IPL1 e IPL2. */
static void boot_common(struct salsa64_n64 *n64, const uint8_t *rom)
{
	struct salsa64_cpu *cpu = &n64->cpu;
	struct salsa64_bus *bus = &n64->bus;

	salsa64_cpu_cop0_write(cpu, SALSA64_CP0_STATUS, STATUS_BOOT);
	salsa64_cpu_cop0_write(cpu, SALSA64_CP0_COUNT, 0);
	salsa64_cpu_cop0_write(cpu, SALSA64_CP0_COMPARE, 0);

	/* Tiempos del dominio 1 que el IPL2 toma de los bytes 1–3 del encabezado. */
	bus->pi[SALSA64_PI_BSD_DOM1_LAT] = rom[3];
	bus->pi[SALSA64_PI_BSD_DOM1_PWD] = rom[2];
	bus->pi[SALSA64_PI_BSD_DOM1_PGS] = rom[1] & 0x0F;
	bus->pi[SALSA64_PI_BSD_DOM1_RLS] = (rom[1] >> 4) & 0x03;
	bus->ri[3] = RI_SELECT_BOOT;
	/* Registros de la RDRAM tras su inicialización (los que lee nemu64-test
	 * en la consola, con y sin Expansion Pak): DeviceType, Delay, RasInterval. */
	bus->rdram_regs[0] = 0xB4190010u;
	bus->rdram_regs[2] = 0x2B3B1A0Bu;
	bus->rdram_regs[6] = 0x101C0A04u;
	bus->vi.regs[SALSA64_VI_V_INTR] = VI_V_INTR_BOOT;
	/* SP detenido con PC = 0, MI sin interrupciones ni máscara, DMEM e IMEM
	 * en cero: ya lo deja así el reset. */
}

static uint32_t elf_find(const struct salsa64_bus *bus)
{
	uint32_t end = bus->rom_size < ELF_SEARCH_LIMIT ? bus->rom_size : ELF_SEARCH_LIMIT;
	for (uint32_t off = SALSA64_ROM_IPL3_END; off + 0x40 <= end; off += 0x100)
		if (salsa64_rd32(bus->rom + off) == ELF_MAGIC)
			return off;
	return 0;
}

static bool boot_elf(struct salsa64_n64 *n64, uint32_t elf, struct salsa64_boot_info *info)
{
	struct salsa64_bus *bus = &n64->bus;
	const uint8_t *e = bus->rom + elf;
	uint32_t room = bus->rom_size - elf;

	if (e[5] != 2)
		return fail(info, "ELF little-endian (el IPL3 de libdragon tampoco lo acepta)");
	bool elf64 = e[4] == 2;
	uint32_t phsize = elf64 ? 0x38 : 0x20;
	uint32_t entry = salsa64_rd32(e + (elf64 ? 0x1C : 0x18));
	uint32_t phoff = salsa64_rd32(e + (elf64 ? 0x24 : 0x1C));
	uint32_t phnum = salsa64_rd16(e + (elf64 ? 0x38 : 0x2C));
	if (phoff > room || phnum * phsize > room - phoff)
		return fail(info, "tabla de program headers fuera de la ROM");

	/* Igual que libdragon: solo los 32 bits bajos de cada campo de 64. */
	info->loaded = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = e + phoff + i * phsize;
		uint32_t type = salsa64_rd32(ph);
		uint32_t flags = salsa64_rd32(ph + (elf64 ? 0x04 : 0x18));
		uint32_t offset = salsa64_rd32(ph + (elf64 ? 0x0C : 0x04));
		uint32_t vaddr = salsa64_rd32(ph + (elf64 ? 0x14 : 0x08));
		uint32_t filesz = salsa64_rd32(ph + (elf64 ? 0x24 : 0x10));
		uint32_t memsz = salsa64_rd32(ph + (elf64 ? 0x2C : 0x14));
		uint32_t pa;

		if (type == PT_N64_DECOMP || (type == PT_LOAD && (flags & PF_N64_COMPRESSED)))
			return fail(info, "ELF comprimido (n64elfcompress): no soportado todavía");
		if (type != PT_LOAD)
			continue;
		if (memsz < filesz)
			memsz = filesz;
		if (!rdram_range(bus, vaddr, memsz, &pa))
			return fail(info, "segmento del ELF fuera de la RDRAM");
		if (offset > room || filesz > room - offset)
			return fail(info, "segmento del ELF fuera de la ROM");
		memcpy(bus->rdram + pa, e + offset, filesz);
		memset(bus->rdram + pa + filesz, 0, memsz - filesz);
		info->loaded += filesz;
	}

	/* Boot flags en DMEM (libdragon, boot/README.md). La entropía queda en 0
	 * para que cada corrida sea idéntica. */
	uint8_t *dmem = bus->sp_mem;
	salsa64_wr32(dmem + 0, bus->rdram_size);
	salsa64_wr32(dmem + 4, 0);
	salsa64_wr32(dmem + 8, (uint32_t)info->tv << 16);  /* ROM de cartucho, arranque en frío, N64 */
	salsa64_wr32(dmem + 12, (SALSA64_CART_ROM_ADDR + elf) << 8);  /* páginas de 256 bytes en 12–13 */

	n64->cpu.gpr[REG_SP] = salsa64_sx32(0x80000000u + bus->rdram_size - 0x10);
	info->kind = SALSA64_BOOT_ELF;
	info->elf_offset = elf;
	info->entry = entry;
	return true;
}

/*
 * Lo que el IPL3 del 6105/7105 deja además en la RDRAM, y que los juegos
 * comprueban (Perfect Dark se queda en un `b .` si 0x2E8 no vale 0xC86E2000;
 * Zelda OoT lo revisa en un actor; Donkey Kong 64 lee 0x2FE1C0):
 *   - copia su propia imagen de DMEM 0x554–0x888 a RDRAM 0x004–0x338;
 *   - corre en el RSP un microcódigo que lee 0x1F0 bytes de RDRAM 0x1E8 a
 *     IMEM 0x120 y los escribe de vuelta con un DMA con paso: 24 filas de 8
 *     bytes desde RDRAM 0x2FB1F0, una cada 0xFF0;
 *   - deja el tamaño de la RDRAM en 0x3F0.
 * Las variables de libultra (0x300–0x318) se escriben después y pisan la
 * copia; de 0x31C a 0x337 queda lo copiado.
 */
static void boot_x105(struct salsa64_bus *bus)
{
	uint8_t *ram = bus->rdram, *imem = bus->sp_mem + 0x1000;

	memcpy(ram + X105_COPY_DST, bus->rom + X105_COPY_SRC, X105_COPY_END - X105_COPY_SRC);
	memcpy(imem + X105_RSP_IMEM, ram + X105_RSP_SRC, X105_RSP_LEN);
	for (uint32_t i = 0; i < X105_RSP_ROWS; i++)
		memcpy(ram + X105_RSP_DST + i * X105_RSP_STRIDE, imem + X105_RSP_IMEM + i * X105_RSP_ROW,
		       X105_RSP_ROW);
	salsa64_wr32(ram + X105_MEMSIZE, bus->rdram_size);
	for (uint32_t i = 0; i < SALSA64_SP_MEM_SIZE; i += 4)
		salsa64_wr32(bus->sp_mem + i, X105_SPMEM_FILL);
}

/*
 * Lo que el IPL3 del 6103 deja al saltar al juego, y que Banjo-Kazooie
 * comprueba:
 *   - su imagen de DMEM 0x4C0–0x764 (la parte que corre desde ahí al final)
 *     en la RDRAM 0x000–0x2A4. Antes de cada proyección el juego lee 0x1D8
 *     y, si no está, cambia los planos cercano y lejano (30 y 4000) por 750
 *     y 1250: falta lo cercano (personajes, el cielo), lo lejano (el fondo)
 *     y la profundidad tiembla. Los vectores de excepción que pone después
 *     el juego pisan parte de la copia, como en la consola;
 *   - la DMEM y la IMEM en 0xFF salvo 6103 en IMEM 0x000. Si no, el juego no
 *     trae de la ROM el arranque del RSP (con la HLE no se nota; el
 *     microcódigo real no arranca).
 */
static void boot_x103(struct salsa64_bus *bus)
{
	memcpy(bus->rdram + X103_COPY_DST, bus->rom + X103_COPY_SRC, X103_COPY_END - X103_COPY_SRC);
	memset(bus->sp_mem, 0xFF, SALSA64_SP_MEM_SIZE);
	salsa64_wr32(bus->sp_mem + 0x1000, X103_IMEM_CIC);
}

/* CIC 6106 (F-Zero X): como el 6103 en DMEM e IMEM (0xFF salvo el número en
 * IMEM 0x000), con su número 6104 (0x17D8), medido corriendo el IPL3 del
 * cartucho. Su copia de la imagen a la RDRAM baja no se reproduce (no está
 * en la ROM tal cual) y ningún juego conocido la comprueba. */
#define X106_CIC_ID      0x17D8u

static bool boot_flat(struct salsa64_n64 *n64, struct salsa64_boot_info *info)
{
	struct salsa64_bus *bus = &n64->bus;
	struct salsa64_cpu *cpu = &n64->cpu;
	uint32_t entry = salsa64_rd32(bus->rom + 8);

	switch (info->cic) {
	case SALSA64_CIC_6103: entry -= 0x100000; break;
	case SALSA64_CIC_6106: entry -= 0x200000; break;
	case SALSA64_CIC_7102: entry = 0x80000480; break;
	default: break;
	}

	uint32_t len = bus->rom_size - SALSA64_ROM_IPL3_END, pa;
	if (len > FLAT_LOAD_SIZE)
		len = FLAT_LOAD_SIZE;
	if (!rdram_range(bus, entry, len, &pa))
		return fail(info, "la dirección de arranque del encabezado no cae en la RDRAM");
	memcpy(bus->rdram + pa, bus->rom + SALSA64_ROM_IPL3_END, len);
	if (info->cic == SALSA64_CIC_6103)
		boot_x103(bus);
	else if (info->cic == SALSA64_CIC_6105)
		boot_x105(bus);
	else if (info->cic == SALSA64_CIC_6106) {
		memset(bus->sp_mem, 0xFF, SALSA64_SP_MEM_SIZE);
		salsa64_wr32(bus->sp_mem + 0x1000, X106_CIC_ID);
	}
	if (info->cic != SALSA64_CIC_UNKNOWN)
		salsa64_cpu_cop0_write(cpu, SALSA64_CP0_COUNT, FLAT_IPL3_COUNT);

	/* Variables de arranque de libultra (osTvType, osRomType, osRomBase,
	 * osResetType, osVersion, osMemSize), como las deja el IPL3. osVersion y
	 * s7 valen 1 con todos los IPL3 (medido en ares con 6102, 6103, 6105 y
	 * 6106, ejecutando el IPL2 y el IPL3 reales). */
	salsa64_wr32(bus->rdram + 0x300, info->tv);
	salsa64_wr32(bus->rdram + 0x304, 0);
	salsa64_wr32(bus->rdram + 0x308, 0xB0000000u);
	salsa64_wr32(bus->rdram + 0x30C, 0);
	/* osCicId: los IPL3 del 6103 y del 6105 dejan su número, y los juegos de
	 * Rare lo comprueban (6103: Banjo-Kazooie, Diddy Kong Racing; 6105:
	 * Conker no arranca su hilo principal sin él, Perfect Dark, DK64). De
	 * las demás variantes no hay dato: queda como estaba. */
	if (info->cic == SALSA64_CIC_6103 || info->cic == SALSA64_CIC_6105 || info->cic == SALSA64_CIC_6106)
		salsa64_wr32(bus->rdram + 0x310, info->cic == SALSA64_CIC_6103 ? 6103 :
					       info->cic == SALSA64_CIC_6105 ? 6105 : X106_CIC_ID);
	salsa64_wr32(bus->rdram + 0x314, 1);
	salsa64_wr32(bus->rdram + 0x318, bus->rdram_size);

	/* Lo que el IPL2 pasa al IPL3: tipo de ROM, TV, reset, semilla, versión. */
	cpu->gpr[REG_S3] = 0;
	cpu->gpr[REG_S4] = info->tv;
	cpu->gpr[REG_S5] = 0;
	cpu->gpr[REG_S6] = salsa64_cic_seed(info->cic);
	cpu->gpr[REG_S7] = 1;
	cpu->gpr[REG_SP] = FLAT_SP;

	bus->pif.challenge6105 = info->cic == SALSA64_CIC_6105;
	info->kind = SALSA64_BOOT_FLAT;
	info->entry = entry;
	info->loaded = len;
	return true;
}

bool salsa64_boot_hle(struct salsa64_n64 *n64, int tv, struct salsa64_boot_info *info)
{
	const struct salsa64_bus *bus = &n64->bus;
	enum salsa64_cic cic = SALSA64_CIC_UNKNOWN;

	if (bus->rom && bus->rom_size >= SALSA64_ROM_IPL3_END)
		cic = salsa64_rom_cic(bus->rom);
	return salsa64_boot_hle_as(n64, tv, cic, info);
}

bool salsa64_boot_hle_as(struct salsa64_n64 *n64, int tv, enum salsa64_cic cic,
			 struct salsa64_boot_info *info)
{
	struct salsa64_boot_info scratch;
	struct salsa64_bus *bus = &n64->bus;
	const uint8_t *rom = bus->rom;

	if (!info)
		info = &scratch;
	memset(info, 0, sizeof *info);
	if (!rom || bus->rom_size < SALSA64_ROM_IPL3_END)
		return fail(info, "ROM ausente o de menos de 4 KB");

	salsa64_n64_reset(n64);
	info->cic = cic;
	info->tv = tv == SALSA64_BOOT_TV_AUTO ? salsa64_rom_tv_type(rom) : (enum salsa64_tv_type)tv;
	boot_common(n64, rom);
	salsa64_vi_set_tv(bus, info->tv);
	salsa64_vi_start(bus);

	/* Un IPL3 de Nintendo reconocido siempre copia 1 MB plano; si no, se
	 * busca un ELF como hace el de libdragon. */
	uint32_t elf = info->cic == SALSA64_CIC_UNKNOWN ? elf_find(bus) : 0;
	bool ok = elf ? boot_elf(n64, elf, info) : boot_flat(n64, info);
	if (ok) {
		salsa64_bus_rdram_written(bus, 0, bus->rdram_size);
		salsa64_cpu_set_pc(&n64->cpu, salsa64_sx32(info->entry));
	}
	return ok;
}
