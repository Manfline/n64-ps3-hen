/*
 * Salsa64 — pruebas de la FlashRAM (src/core/flashram.c): las secuencias de
 * comandos con que libultra identifica el chip, lee su estado, lee datos,
 * borra sectores y programa páginas, por el bus y el DMA del PI como lo
 * haría el juego.
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "core/save.h"

#define PI_DRAM_ADDR  0x04600000u
#define PI_CART_ADDR  0x04600004u
#define PI_RD_LEN     0x04600008u
#define PI_WR_LEN     0x0460000Cu
#define PI_STATUS     0x04600010u
#define MI_INTR       0x04300008u
#define FLASH         0x08000000u
#define FLASH_CMD     0x08010000u
#define BUF           0x30000u     /* zona de la RDRAM para los DMA */
#define IDLE          0x8Cu        /* WSM_READY | ERASE_OK | PROGRAM_OK */

static struct salsa64_save save;

static struct salsa64_bus *bus(void)
{
	return &t_n64.bus;
}

static uint8_t *rdram(uint32_t pa)
{
	return t_n64.bus.rdram + pa;
}

static void plug(void)
{
	salsa64_save_init(&save, SALSA64_SAVE_FLASHRAM);
	salsa64_save_attach(bus(), &save);
}

static void unplug(void)
{
	salsa64_save_attach(bus(), NULL);
}

static void wr(uint32_t paddr, uint32_t v)
{
	salsa64_bus_write32(bus(), paddr, v, 0xFFFFFFFFu);
}

static uint32_t rd(uint32_t paddr)
{
	return salsa64_bus_read32(bus(), paddr);
}

static void cmd(uint32_t c)
{
	wr(FLASH_CMD, c);
}

/* DMA del PI cartucho → RDRAM (PI_WR_LEN), con la interrupción atendida. */
static void dma_in(uint32_t cart, uint32_t dram, uint32_t len)
{
	wr(PI_DRAM_ADDR, dram);
	wr(PI_CART_ADDR, cart);
	wr(PI_WR_LEN, len - 1);
	CHECK((rd(MI_INTR) >> SALSA64_IRQ_PI) & 1);
	wr(PI_STATUS, 0x2);
}

/* DMA del PI RDRAM → cartucho (PI_RD_LEN). */
static void dma_out(uint32_t cart, uint32_t dram, uint32_t len)
{
	wr(PI_DRAM_ADDR, dram);
	wr(PI_CART_ADDR, cart);
	wr(PI_RD_LEN, len - 1);
	CHECK((rd(MI_INTR) >> SALSA64_IRQ_PI) & 1);
	wr(PI_STATUS, 0x2);
}

/* Datos conocidos en todo el chip, sin contar como escrituras del juego. */
static void fill_pattern(void)
{
	for (uint32_t i = 0; i < SALSA64_FLASH_SIZE; i++)
		save.data[i] = (uint8_t)(i * 7 + (i >> 8));
	save.writes = save.flushed = save.seen = 0;
}

static bool all(const uint8_t *p, uint8_t v, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++)
		if (p[i] != v)
			return false;
	return true;
}

/* Lo que hace osFlashReadStatus: lectura de E/S, modo estado y otra
 * lectura. */
static uint32_t read_status(void)
{
	(void)rd(FLASH);
	cmd(0xD2000000);
	return rd(FLASH) & 0xFF;
}

/* ---- Identificador y estado ------------------------------------------------ */

static void flash_type(void)
{
	plug();
	CHECK_EQ(save.type, SALSA64_SAVE_FLASHRAM);
	CHECK_EQ(save.size, 128u << 10);
	CHECK(salsa64_save_emulated(SALSA64_SAVE_FLASHRAM));
	CHECK(all(save.data, 0xFF, SALSA64_FLASH_SIZE));  /* chip nuevo: borrado */
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_READ_ARRAY);
	CHECK_EQ(save.flash.status, IDLE);
	unplug();
}

/* osFlashReadId: lectura de E/S, comando E1 y DMA de 8 bytes. */
static void flash_silicon_id(void)
{
	static const uint8_t id[8] = { 0x11, 0x11, 0x80, 0x01, 0x00, 0xC2, 0x00, 0x1E };
	plug();
	(void)rd(FLASH);
	cmd(0xE1000000);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_SILICON_ID);
	memset(rdram(BUF), 0x55, 32);
	dma_in(FLASH, BUF, 8);
	CHECK(memcmp(rdram(BUF), id, 8) == 0);
	CHECK_EQ(rdram(BUF)[8], 0x55);
	CHECK_EQ(rd(PI_DRAM_ADDR), BUF + 8);
	CHECK_EQ(rd(PI_CART_ADDR), FLASH + 8);

	/* Un DMA más largo lo repite; la dirección no importa. */
	dma_in(FLASH + 0x1230, BUF, 16);
	CHECK(memcmp(rdram(BUF), id, 8) == 0);
	CHECK(memcmp(rdram(BUF + 8), id, 8) == 0);

	/* Por E/S solo se ve la primera palabra. */
	CHECK_EQ(rd(FLASH), 0x11118001u);
	CHECK_EQ(rd(FLASH + 4), 0x11118001u);
	CHECK_EQ(save.writes, 0);
	unplug();
}

static void flash_status(void)
{
	plug();
	CHECK_EQ(read_status(), IDLE);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_STATUS);
	CHECK_EQ(rd(FLASH), 0x008C008Cu);  /* «00 SS» repetido */
	CHECK_EQ(rd(FLASH + 0x40), 0x008C008Cu);
	memset(rdram(BUF), 0x55, 8);
	dma_in(FLASH, BUF, 6);
	CHECK_EQ(salsa64_rd32(rdram(BUF)), 0x008C008Cu);
	CHECK_EQ(salsa64_rd16(rdram(BUF + 4)), 0x008C);
	CHECK_EQ(rdram(BUF)[6], 0x55);

	/* osFlashClearStatus: modo estado y un 0 en la dirección 0. En el
	 * MX29L1100 los bits OK quedan en 1. */
	save.flash.status = 0x80;
	cmd(0xD2000000);
	wr(FLASH, 0);
	CHECK_EQ(save.flash.status, IDLE);
	save.flash.status = 0x80;
	wr(FLASH + 4, 0);  /* otra dirección: nada */
	CHECK_EQ(save.flash.status, 0x80);
	salsa64_wr32(rdram(BUF), 0);
	dma_out(FLASH, BUF, 4);  /* también por DMA */
	CHECK_EQ(save.flash.status, IDLE);
	CHECK_EQ(save.writes, 0);
	unplug();
}

/* ---- Lectura ------------------------------------------------------------------- */

/* osFlashReadArray en un chip por palabras: modo lectura, lectura de E/S y
 * DMA desde página × 64. */
static void flash_read_array(void)
{
	plug();
	fill_pattern();
	/* Al encender ya está en modo lectura. */
	CHECK_EQ(rd(FLASH), salsa64_rd32(save.data));
	cmd(0xD2000000);
	cmd(0xF0000000);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_READ_ARRAY);
	(void)rd(FLASH);

	/* Páginas 3–5: dirección 3 × 64, 384 bytes. */
	dma_in(FLASH + 3 * 64, BUF, 3 * 128);
	CHECK(memcmp(rdram(BUF), save.data + 3 * 128, 3 * 128) == 0);
	CHECK_EQ(rd(PI_CART_ADDR), FLASH + 3 * 64 + 3 * 128);

	/* La última página y lecturas de E/S por palabra. */
	dma_in(FLASH + 1023 * 64, BUF, 128);
	CHECK(memcmp(rdram(BUF), save.data + 1023 * 128, 128) == 0);
	CHECK_EQ(rd(FLASH + 0x1000), salsa64_rd32(save.data + 0x2000));
	CHECK_EQ(rd(FLASH + 0xFFFC), salsa64_rd32(save.data + 0x1FFF8));

	/* Una ráfaga no sale de su bloque de 32 KB: vuelve al principio. */
	dma_in(FLASH + 0x3FF8, BUF, 32);
	CHECK(memcmp(rdram(BUF), save.data + 0x7FF0, 16) == 0);
	CHECK(memcmp(rdram(BUF + 16), save.data, 16) == 0);
	dma_in(FLASH + 0xBFFC, BUF, 16);
	CHECK(memcmp(rdram(BUF), save.data + 0x17FF8, 8) == 0);
	CHECK(memcmp(rdram(BUF + 8), save.data + 0x10000, 8) == 0);

	/* Espejo cada 128 KB en el dominio 2; 0x08010000–0x0801FFFF está fuera
	 * del chip (se lee 0). */
	CHECK_EQ(rd(FLASH + 0x20000 + 0x40), salsa64_rd32(save.data + 0x80));
	CHECK_EQ(rd(FLASH + 0x10040), 0u);
	dma_in(FLASH + 0x60000 + 64, BUF, 128);
	CHECK(memcmp(rdram(BUF), save.data + 128, 128) == 0);
	CHECK_EQ(save.writes, 0);

	/* Escribir en modo lectura no cambia nada. */
	wr(FLASH + 0x100, 0x12345678);
	dma_out(FLASH, BUF, 128);
	CHECK_EQ(save.writes, 0);
	unplug();
}

/* ---- Borrado ------------------------------------------------------------------- */

/* osFlashSectorErase: 4B con una página del sector, 78 y sondeo de
 * ERASE_BUSY; después osFlashReadStatus y la prueba de ERASE_OK. */
static void flash_sector_erase(void)
{
	plug();
	memset(save.data, 0, SALSA64_FLASH_SIZE);
	cmd(0x4B000000 | 0x123);  /* página 0x123: sector 2 */
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_READ_ARRAY);
	CHECK(all(save.data + 0x8000, 0, 0x4000));
	cmd(0x78000000);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_STATUS);
	CHECK_EQ(rd(FLASH) & 0x02, 0);  /* sin ERASE_BUSY */
	CHECK(all(save.data, 0, 0x8000));
	CHECK(all(save.data + 0x8000, 0xFF, 0x4000));
	CHECK(all(save.data + 0xC000, 0, SALSA64_FLASH_SIZE - 0xC000));
	CHECK_EQ(save.writes, 128);  /* una por página */
	CHECK_EQ(read_status() & 0x08, 0x08);

	/* Los bits 23–10 se ignoran: 0x4B00FC80 es la página 0x080, sector 1. */
	cmd(0x4B00FC80);
	cmd(0x78000000);
	CHECK(all(save.data + 0x4000, 0xFF, 0x4000));
	CHECK(all(save.data, 0, 0x4000));

	/* Borrar sin preparar no hace nada (ni cambia de modo); borrar lo ya
	 * borrado no cuenta como escritura. */
	uint32_t writes = save.writes;
	cmd(0xF0000000);
	cmd(0x78000000);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_READ_ARRAY);
	cmd(0x4B000080);
	cmd(0x78000000);
	CHECK_EQ(save.writes, writes);
	CHECK(all(save.data, 0, 0x4000));
	unplug();
}

static void flash_chip_erase(void)
{
	plug();
	fill_pattern();
	cmd(0x3C000000);
	cmd(0x78000000);
	CHECK(all(save.data, 0xFF, SALSA64_FLASH_SIZE));
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_STATUS);
	CHECK_EQ(save.flash.erase, 0);
	CHECK_EQ(rd(FLASH), 0x008C008Cu);
	unplug();
}

/* ---- Programación -------------------------------------------------------------- */

/* osFlashWriteBuffer (B4 y DMA de 128 bytes hacia el cartucho) y
 * osFlashWriteArray (A5 con la página y sondeo de PROGRAM_BUSY), sobre un
 * sector borrado. */
static void flash_program_page(void)
{
	plug();
	fill_pattern();
	cmd(0x4B000000 | 0x200);
	cmd(0x78000000);
	CHECK(all(save.data + 0x10000, 0xFF, 0x4000));

	for (uint32_t i = 0; i < 128; i++)
		rdram(BUF)[i] = (uint8_t)(0xA0 ^ i);
	cmd(0xB4000000);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_LOAD_PAGE);
	dma_out(FLASH, BUF, 128);
	CHECK(memcmp(save.flash.page, rdram(BUF), 128) == 0);
	CHECK_EQ(rd(PI_CART_ADDR), FLASH + 128);
	uint32_t writes = save.writes;
	cmd(0xA5000000 | 0x203);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_STATUS);
	CHECK_EQ(rd(FLASH) & 0x01, 0);  /* sin PROGRAM_BUSY */
	CHECK_EQ(rd(FLASH) & 0x04, 0x04);
	CHECK(memcmp(save.data + 0x203 * 128, rdram(BUF), 128) == 0);
	CHECK(all(save.data + 0x202 * 128, 0xFF, 128));
	CHECK(all(save.data + 0x204 * 128, 0xFF, 128));
	CHECK_EQ(save.writes, writes + 1);
	CHECK(all(save.flash.page, 0xFF, 128));  /* el búfer vuelve a 0xFF */

	/* Releer como el juego: modo lectura y DMA desde 0x203 × 64. */
	cmd(0xF0000000);
	dma_in(FLASH + 0x203 * 64, BUF + 0x100, 128);
	CHECK(memcmp(rdram(BUF + 0x100), rdram(BUF), 128) == 0);

	/* Programar solo baja bits; un búfer a medias deja 0xFF en el resto. */
	cmd(0xB4000000);
	memset(rdram(BUF), 0x0F, 16);
	dma_out(FLASH, BUF, 16);
	cmd(0xA5000000 | 0x203);
	for (uint32_t i = 0; i < 16; i++)
		CHECK_EQ(save.data[0x203 * 128 + i], (uint8_t)((0xA0 ^ i) & 0x0F));
	CHECK_EQ(save.data[0x203 * 128 + 16], 0xA0 ^ 16);

	/* Bits 23–10 ignorados también en la página. */
	cmd(0xB4000000);
	memset(rdram(BUF), 0, 128);
	dma_out(FLASH, BUF, 128);
	cmd(0xA5FFFC00 | 0x204);
	CHECK(all(save.data + 0x204 * 128, 0, 128));
	unplug();
}

/* El búfer de página por E/S de 32 bits, su lectura y el comando B4 repetido
 * (libultra lo vuelve a mandar con chips por byte): el búfer se conserva
 * hasta programar. */
static void flash_page_buffer(void)
{
	plug();
	cmd(0xB4000000);
	wr(FLASH + 0x10, 0x11223344);
	wr(FLASH + 0x80 + 0x14, 0x55667788);  /* la dirección va & 0x7F */
	CHECK_EQ(rd(FLASH + 0x10), 0x11223344u);
	CHECK_EQ(rd(FLASH + 0x14), 0x55667788u);
	CHECK_EQ(rd(FLASH + 0x18), 0xFFFFFFFFu);
	cmd(0xB4000000);
	dma_in(FLASH, BUF, 128);
	CHECK_EQ(salsa64_rd32(rdram(BUF + 0x10)), 0x11223344u);
	cmd(0xA5000000);
	CHECK_EQ(salsa64_rd32(save.data + 0x10), 0x11223344u);
	CHECK_EQ(salsa64_rd32(save.data + 0x14), 0x55667788u);
	CHECK(all(save.data + 0x18, 0xFF, 128 - 0x18));
	unplug();
}

/* ---- Casos raros ---------------------------------------------------------------- */

static void flash_odd_commands(void)
{
	plug();
	cmd(0xE1000000);
	cmd(0x12345678);  /* desconocido: se cuenta y no cambia el modo */
	CHECK_EQ(save.flash.unknown, 1);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_SILICON_ID);
	CHECK_EQ(save.flash.commands, 2);

	/* Comando por DMA hacia 0x08010000 (4 primeros bytes). */
	salsa64_wr32(rdram(BUF), 0xF0000000);
	dma_out(FLASH_CMD, BUF, 8);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_READ_ARRAY);

	/* El comando se reconoce en cualquier espejo del dominio 2. */
	wr(FLASH + 0x40000 + 0x10000, 0xD2000000);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_STATUS);

	/* Un DMA que se pasa de la RDRAM se corta sin tocar nada más. */
	cmd(0xF0000000);
	uint32_t end = t_n64.bus.rdram_size;
	dma_in(FLASH, end - 64, 128);
	CHECK(all(rdram(end - 64), 0xFF, 64));

	/* Sin FlashRAM (otro tipo), el dominio 2 no contesta como flash. */
	salsa64_save_init(&save, SALSA64_SAVE_EEPROM_4K);
	cmd(0xE1000000);
	CHECK_EQ(rd(FLASH), 0u);
	unplug();
}

/* El modo y el búfer de página sobreviven al reset de la consola (el chip no
 * se apaga); salsa64_save_init, al abrir la ROM, los deja como al encender. */
static void flash_reset(void)
{
	plug();
	cmd(0xB4000000);
	wr(FLASH, 0x01020304);
	salsa64_n64_reset(&t_n64);
	CHECK(t_n64.bus.save == &save);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_LOAD_PAGE);
	CHECK_EQ(rd(FLASH), 0x01020304u);
	salsa64_save_init(&save, SALSA64_SAVE_FLASHRAM);
	CHECK_EQ(save.flash.mode, SALSA64_FLASH_READ_ARRAY);
	CHECK(all(save.flash.page, 0xFF, 128));
	unplug();
}

void t_suite_flashram(void)
{
	RUN(flash_type);
	RUN(flash_silicon_id);
	RUN(flash_status);
	RUN(flash_read_array);
	RUN(flash_sector_erase);
	RUN(flash_chip_erase);
	RUN(flash_program_page);
	RUN(flash_page_buffer);
	RUN(flash_odd_commands);
	RUN(flash_reset);
}
