/*
 * Retro64 — pruebas del arranque HLE, de la imagen del cartucho, del
 * ISViewer y de las reglas del bus fuera de la RDRAM.
 *
 * Las ROMs son sintéticas (encabezado, ceros en el IPL3 y unas pocas
 * instrucciones): no hay código de Nintendo.
 */
#include "test.h"

#include <string.h>

#include "core/boot.h"
#include "core/endian.h"

#define KSEG1(pa) (0xFFFFFFFFA0000000ull | (pa))

static uint8_t rom[0x2000];

static void rom_header(uint32_t entry, char destination)
{
	memset(rom, 0, sizeof rom);
	retro64_wr32(rom, 0x80371240);
	retro64_wr32(rom + 8, entry);
	rom[0x3E] = (uint8_t)destination;
}

static bool boot(int tv, struct retro64_boot_info *info)
{
	retro64_bus_set_rom(&t_n64.bus, rom, sizeof rom);
	return retro64_boot_hle(&t_n64, tv, info);
}

static void done(void)
{
	retro64_bus_set_rom(&t_n64.bus, NULL, 0);
}

static void rom_byte_orders(void)
{
	static const uint8_t z64[8] = { 0x80, 0x37, 0x12, 0x40, 0x00, 0x00, 0x00, 0x0F };
	static const uint8_t v64[8] = { 0x37, 0x80, 0x40, 0x12, 0x00, 0x00, 0x0F, 0x00 };
	static const uint8_t n64[8] = { 0x40, 0x12, 0x37, 0x80, 0x0F, 0x00, 0x00, 0x00 };
	uint8_t b[8];

	memcpy(b, v64, 8);
	CHECK_EQ(retro64_rom_normalize(b, 8), RETRO64_ROM_V64);
	CHECK(memcmp(b, z64, 8) == 0);
	memcpy(b, n64, 8);
	CHECK_EQ(retro64_rom_normalize(b, 8), RETRO64_ROM_N64);
	CHECK(memcmp(b, z64, 8) == 0);
	CHECK_EQ(retro64_rom_normalize(b, 8), RETRO64_ROM_Z64);
	CHECK(memcmp(b, z64, 8) == 0);
	/* Cualquier otra firma se toma como .z64 y no se toca. */
	b[0] = 0x81;
	CHECK_EQ(retro64_rom_normalize(b, 8), RETRO64_ROM_Z64);
	CHECK_EQ(b[0], 0x81);
}

static void crc_cic_and_tv(void)
{
	CHECK_EQ(retro64_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
	rom_header(0x80000400, 'E');
	CHECK_EQ(retro64_rom_cic(rom), RETRO64_CIC_UNKNOWN);  /* IPL3 en ceros */
	CHECK_EQ(retro64_rom_tv_type(rom), RETRO64_TV_NTSC);
	rom[0x3E] = 'P';
	CHECK_EQ(retro64_rom_tv_type(rom), RETRO64_TV_PAL);
	rom[0x3E] = 'B';
	CHECK_EQ(retro64_rom_tv_type(rom), RETRO64_TV_MPAL);
	CHECK_EQ(retro64_cic_seed(RETRO64_CIC_6105), 0x91);
	CHECK_EQ(retro64_cic_seed(RETRO64_CIC_6102), 0x3F);
}

/* ELF32 big-endian con un PT_LOAD de 8 bytes y BSS, en 0x1100 (la búsqueda
 * va de 256 en 256 desde 0x1000). */
static void build_elf(uint32_t elf, uint32_t vaddr, uint32_t memsz)
{
	uint8_t *e = rom + elf, *ph = e + 0x34;
	retro64_wr32(e, 0x7F454C46);
	e[4] = 1;  /* 32 bits */
	e[5] = 2;  /* big-endian */
	e[6] = 1;
	retro64_wr32(e + 0x18, vaddr);   /* e_entry */
	retro64_wr32(e + 0x1C, 0x34);    /* e_phoff */
	retro64_wr16(e + 0x2A, 0x20);    /* e_phentsize */
	retro64_wr16(e + 0x2C, 1);       /* e_phnum */
	retro64_wr32(ph + 0x00, 1);      /* PT_LOAD */
	retro64_wr32(ph + 0x04, 0x100);  /* p_offset */
	retro64_wr32(ph + 0x08, vaddr);
	retro64_wr32(ph + 0x0C, vaddr);
	retro64_wr32(ph + 0x10, 8);      /* p_filesz */
	retro64_wr32(ph + 0x14, memsz);
	retro64_wr32(ph + 0x18, 5);
	retro64_wr32(e + 0x100, ADDIU(T0, ZERO, 0x55));
	retro64_wr32(e + 0x104, ADDU(T1, SP, ZERO));
}

static void boot_elf_like_libdragon(void)
{
	struct retro64_boot_info info;
	rom_header(0x80000400, 'E');
	build_elf(0x1100, 0x80001000, 0x20);

	CHECK(boot(RETRO64_BOOT_TV_AUTO, &info));
	CHECK_EQ(info.kind, RETRO64_BOOT_ELF);
	CHECK_EQ(info.elf_offset, 0x1100);
	CHECK_EQ(info.entry, 0x80001000);
	CHECK_EQ(info.loaded, 8);
	CHECK_EQ(CPU->pc, 0xFFFFFFFF80001000ull);
	CHECK_EQ(CPU->status, 0x34000000);
	CHECK_EQ(R[SP], 0xFFFFFFFF807FFFF0ull);  /* fin de los 8 MB - 0x10 */
	CHECK_EQ(t_rd32(0x1004), ADDU(T1, SP, ZERO));
	CHECK_EQ(t_rd32(0x1008), 0);

	/* Boot flags en DMEM: memoria, entropía, TV y ELF en páginas de 256. */
	const uint8_t *dmem = t_n64.bus.sp_mem;
	CHECK_EQ(retro64_rd32(dmem + 0), 0x800000);
	CHECK_EQ(retro64_rd32(dmem + 8), 0x00010000);
	CHECK_EQ(retro64_rd32(dmem + 12) >> 16 << 8, 0x1100);
	CHECK_EQ(t_n64.bus.sp.regs[RETRO64_SP_STATUS], RETRO64_SP_STATUS_HALT);

	t_step(2);
	CHECK_EQ(R[T0], 0x55);
	CHECK_EQ(R[T1], 0xFFFFFFFF807FFFF0ull);
	done();
}

static void boot_elf_errors(void)
{
	struct retro64_boot_info info;
	rom_header(0x80000400, 'E');
	build_elf(0x1000, 0x80001000, 0x20);
	rom[0x1005] = 1;  /* little-endian */
	CHECK(!boot(RETRO64_BOOT_TV_AUTO, &info));
	CHECK(info.error != NULL);

	build_elf(0x1000, 0x80001000, 0x01000000);  /* 16 MB de BSS: no cabe */
	CHECK(!boot(RETRO64_BOOT_TV_AUTO, &info));
	CHECK(info.error != NULL);
	done();
}

static void boot_flat_like_libultra(void)
{
	struct retro64_boot_info info;
	rom_header(0x80000400, 'P');
	retro64_wr32(rom + 0x1000, ADDIU(T0, ZERO, 0x77));

	CHECK(boot(RETRO64_BOOT_TV_AUTO, &info));
	CHECK_EQ(info.kind, RETRO64_BOOT_FLAT);
	CHECK_EQ(info.tv, RETRO64_TV_PAL);
	CHECK_EQ(info.loaded, sizeof rom - 0x1000);
	CHECK_EQ(CPU->pc, 0xFFFFFFFF80000400ull);
	CHECK_EQ(t_rd32(0x400), ADDIU(T0, ZERO, 0x77));
	CHECK_EQ(t_rd32(0x300), RETRO64_TV_PAL);       /* osTvType */
	CHECK_EQ(t_rd32(0x308), 0xB0000000);           /* osRomBase */
	CHECK_EQ(t_rd32(0x318), 0x800000);             /* osMemSize */
	CHECK_EQ(R[S4], RETRO64_TV_PAL);
	CHECK_EQ(R[S6], 0x3F);
	CHECK_EQ(R[SP], 0xFFFFFFFFA4001FF0ull);
	t_step(1);
	CHECK_EQ(R[T0], 0x77);

	CHECK(boot(RETRO64_TV_MPAL, &info));  /* tipo de TV forzado */
	CHECK_EQ(t_rd32(0x300), RETRO64_TV_MPAL);
	done();
}

static char isv_text[64];
static uint32_t isv_len;

static void isv_capture(void *ctx, const uint8_t *text, uint32_t len)
{
	(void)ctx;
	isv_len = len < sizeof isv_text ? len : sizeof isv_text;
	memcpy(isv_text, text, isv_len);
}

static void isviewer_prints(void)
{
	static uint8_t isv[RETRO64_ISV_SIZE];
	retro64_bus_set_isviewer(&t_n64.bus, isv, isv_capture, NULL);
	isv_len = 0;

	R[T0] = 0xFFFFFFFFB3FF0000ull;
	R[T1] = 0x686F6C61;  /* "hola" */
	R[T2] = 0x21;        /* "!" */
	R[T3] = 5;
	T_LOAD(SW(T1, 0x20, T0), SB(T2, 0x24, T0), LW(T4, 0x20, T0), SW(T3, 0x14, T0));
	t_step(4);
	CHECK_EQ(R[T4], 0x686F6C61);  /* se puede leer lo escrito (así lo detectan) */
	CHECK_EQ(isv_len, 5);
	CHECK(memcmp(isv_text, "hola!", 5) == 0);

	retro64_bus_set_isviewer(&t_n64.bus, NULL, NULL, NULL);
}

/* Bus PI de 16 bits: lecturas de 8/16 bits desde una dirección par traen la
 * palabra que empieza ahí (n64brew.dev: Memory map). */
static void pi_bus_subword_reads(void)
{
	static const uint8_t data[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
	retro64_bus_set_rom(&t_n64.bus, data, sizeof data);
	R[T0] = KSEG1(0x10000000);
	T_LOAD(LBU(T1, 1, T0), LBU(T2, 2, T0), LBU(T3, 3, T0), LHU(T4, 2, T0), LW(T5, 4, T0));
	t_step(5);
	CHECK_EQ(R[T1], 0x23);
	CHECK_EQ(R[T2], 0x89);
	CHECK_EQ(R[T3], 0xAB);
	CHECK_EQ(R[T4], 0x89AB);
	CHECK_EQ(R[T5], 0xFFFFFFFF89ABCDEFull);
	done();
}

/* SB/SH a la PIF RAM escriben la palabra completa (nemu64-test, pif_memory). */
static void pif_ram_subword_writes(void)
{
	R[T0] = KSEG1(RETRO64_PIF_RAM_ADDR);
	R[T1] = 0x12345678;
	T_LOAD(SW(T1, 0, T0), SB(T1, 1, T0), LW(T2, 0, T0), SH(T1, 6, T0), LW(T3, 4, T0));
	t_step(5);
	CHECK_EQ(R[T2], 0x56780000);
	CHECK_EQ(R[T3], 0x12345678);
}

static void rdram_regs_after_boot(void)
{
	rom_header(0x80000400, 'E');
	CHECK(boot(RETRO64_BOOT_TV_AUTO, NULL));
	CHECK_EQ(retro64_bus_read32(&t_n64.bus, 0x03F00000), 0xB4190010);
	CHECK_EQ(retro64_bus_read32(&t_n64.bus, 0x03F00008), 0x2B3B1A0B);
	CHECK_EQ(retro64_bus_read32(&t_n64.bus, 0x03F00018), 0x101C0A04);
	CHECK_EQ(retro64_bus_read32(&t_n64.bus, 0x03F00040), 0xB4190010);  /* espejo cada 0x40 */
	CHECK_EQ(retro64_bus_read32(&t_n64.bus, 0x0470000C), 0x14);        /* RI_SELECT */
	done();
}

void t_suite_boot(void)
{
	RUN(rom_byte_orders);
	RUN(crc_cic_and_tv);
	RUN(boot_elf_like_libdragon);
	RUN(boot_elf_errors);
	RUN(boot_flat_like_libultra);
	RUN(isviewer_prints);
	RUN(pi_bus_subword_reads);
	RUN(pif_ram_subword_writes);
	RUN(rdram_regs_after_boot);
}
