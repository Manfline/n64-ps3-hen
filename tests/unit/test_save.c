/*
 * Salsa64 — pruebas del guardado del cartucho (src/core/save.c y savedb.c):
 * EEPROM por el PIF, SRAM por DMA del PI y acceso directo, tipo de cada ROM,
 * guardado.cfg, nombre del archivo y volcado diferido. La FlashRAM tiene
 * las suyas en test_flashram.c.
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "core/save.h"

#define SI_DRAM_ADDR  0x04800000u
#define SI_PIF_RD64B  0x04800004u
#define SI_PIF_WR64B  0x04800010u
#define PI_DRAM_ADDR  0x04600000u
#define PI_CART_ADDR  0x04600004u
#define PI_RD_LEN     0x04600008u
#define PI_WR_LEN     0x0460000Cu
#define PI_STATUS     0x04600010u
#define MI_INTR       0x04300008u
#define PIF_RAM       0x1FC007C0u
#define SRAM          0x08000000u
#define CMD           63

static struct salsa64_save save;
static uint8_t rom[0x40];

static struct salsa64_bus *bus(void)
{
	return &t_n64.bus;
}

static uint8_t *ram(void)
{
	return t_n64.bus.pif_ram;
}

/* Guardado nuevo del tipo dado, conectado al cartucho. Cada prueba lo quita
 * al final con unplug() (la máquina de pruebas es compartida). */
static void plug(enum salsa64_save_type type)
{
	salsa64_save_init(&save, type);
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

/* Un intercambio en el canal 4 con el formato de libultra: un 0x00 por cada
 * puerto de control, el comando y 0xFE. Devuelve la posición del byte RX. */
static unsigned eep_exchange(const uint8_t *tx, unsigned txlen, unsigned rxlen)
{
	unsigned at = 4;
	memset(ram(), 0, SALSA64_PIF_RAM_SIZE);
	ram()[at] = (uint8_t)txlen;
	ram()[at + 1] = (uint8_t)rxlen;
	memcpy(ram() + at + 2, tx, txlen);
	memset(ram() + at + 2 + txlen, 0xFF, rxlen);
	ram()[at + 2 + txlen + rxlen] = 0xFE;
	ram()[CMD] = 0x01;
	salsa64_pif_written(bus());
	salsa64_pif_execute(bus());
	return at + 1;
}

static void eep_read(uint8_t block, uint8_t out[8])
{
	const uint8_t tx[] = { 0x04, block };
	unsigned rx = eep_exchange(tx, sizeof tx, 8);
	CHECK_EQ(ram()[rx], 8);  /* sin bits de error */
	memcpy(out, ram() + rx + 1 + sizeof tx, 8);
}

static uint8_t eep_write(uint8_t block, const uint8_t data[8])
{
	uint8_t tx[10] = { 0x05, block };
	memcpy(tx + 2, data, 8);
	unsigned rx = eep_exchange(tx, sizeof tx, 1);
	CHECK_EQ(ram()[rx], 1);
	return ram()[rx + 1 + sizeof tx];
}

/* ---- EEPROM ----------------------------------------------------------------- */

static void eeprom_info(void)
{
	static const uint8_t info[] = { 0x00 }, reset[] = { 0xFF };
	plug(SALSA64_SAVE_EEPROM_4K);
	unsigned rx = eep_exchange(info, 1, 3);
	CHECK_EQ(ram()[rx], 3);
	CHECK_EQ(ram()[rx + 2], 0x00);  /* identificador 0x0080 */
	CHECK_EQ(ram()[rx + 3], 0x80);
	CHECK_EQ(ram()[rx + 4], 0x00);  /* no está escribiendo */

	plug(SALSA64_SAVE_EEPROM_16K);
	rx = eep_exchange(reset, 1, 3);
	CHECK_EQ(ram()[rx], 3);
	CHECK_EQ(ram()[rx + 2], 0x00);  /* identificador 0x00C0 */
	CHECK_EQ(ram()[rx + 3], 0xC0);
	CHECK_EQ(ram()[rx + 4], 0x00);
	unplug();
}

static void eeprom_read_write(void)
{
	static const uint8_t a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, b[8] = { 0xA5, 0, 0xFF, 9, 9, 9, 0x5A, 1 };
	uint8_t got[8];
	plug(SALSA64_SAVE_EEPROM_4K);
	eep_read(5, got);
	for (int k = 0; k < 8; k++)
		CHECK_EQ(got[k], 0xFF);  /* en blanco */

	CHECK_EQ(eep_write(5, a), 0x00);
	CHECK_EQ(save.writes, 1);
	CHECK(memcmp(save.data + 40, a, 8) == 0);
	eep_read(5, got);
	CHECK(memcmp(got, a, 8) == 0);

	/* 4 Kbit: los dos bits altos del bloque no cuentan. */
	eep_read(64 + 5, got);
	CHECK(memcmp(got, a, 8) == 0);
	CHECK_EQ(eep_write(192 + 63, b), 0x00);
	CHECK(memcmp(save.data + 504, b, 8) == 0);

	/* Escribir lo mismo no ensucia. */
	CHECK_EQ(save.writes, 2);
	eep_write(5, a);
	CHECK_EQ(save.writes, 2);

	/* 16 Kbit: 256 bloques distintos. */
	plug(SALSA64_SAVE_EEPROM_16K);
	eep_write(200, b);
	eep_read(8, got);
	CHECK_EQ(got[0], 0xFF);
	eep_read(200, got);
	CHECK(memcmp(got, b, 8) == 0);
	CHECK(memcmp(save.data + 1600, b, 8) == 0);
	unplug();
}

/* Sin EEPROM (sin guardado o con SRAM) el canal 4 no tiene dispositivo; con
 * EEPROM, un comando incompleto o desconocido tampoco contesta. */
static void eeprom_absent(void)
{
	static const uint8_t info[] = { 0x00 }, read_short[] = { 0x04 }, rtc[] = { 0x06 };
	unsigned rx = eep_exchange(info, 1, 3);
	CHECK_EQ(ram()[rx], 0x83);

	plug(SALSA64_SAVE_SRAM_256K);
	rx = eep_exchange(info, 1, 3);
	CHECK_EQ(ram()[rx], 0x83);

	plug(SALSA64_SAVE_EEPROM_4K);
	rx = eep_exchange(read_short, 1, 8);
	CHECK_EQ(ram()[rx], 0x88);
	rx = eep_exchange(rtc, 1, 3);
	CHECK_EQ(ram()[rx], 0x83);
	unplug();
}

/* Todo el camino de libultra: RDRAM → PIF por DMA del SI y de vuelta. */
static void eeprom_through_si(void)
{
	static const uint8_t cmds[] = {
		0x00, 0x00, 0x00, 0x00,                       /* canales 0–3: saltar */
		0x0A, 0x01, 0x05, 0x03, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xFF,
		0xFE,
	};
	plug(SALSA64_SAVE_EEPROM_4K);
	for (unsigned k = 0; k < 64; k += 4) {
		uint32_t w = 0;
		for (unsigned b = 0; b < 4; b++)
			w = w << 8 | (k + b < sizeof cmds ? cmds[k + b] : 0);
		t_wr32(0x3000 + k, k == 60 ? 0x00000001u : w);
	}
	wr(SI_DRAM_ADDR, 0x3000);
	wr(SI_PIF_WR64B, PIF_RAM);
	wr(SI_DRAM_ADDR, 0x3100);
	wr(SI_PIF_RD64B, PIF_RAM);
	CHECK_EQ(t_rd32(0x3104), 0x0A010503u);
	CHECK_EQ(t_rd32(0x3110) >> 16, 0x00FEu);  /* respuesta 0x00 y fin */
	CHECK_EQ(salsa64_rd32(save.data + 24), 0x11223344u);
	CHECK_EQ(salsa64_rd32(save.data + 28), 0x55667788u);
	unplug();
}

/* ---- SRAM ------------------------------------------------------------------- */

static void sram_dma(void)
{
	plug(SALSA64_SAVE_SRAM_256K);
	for (uint32_t k = 0; k < 0x8000; k += 4)
		t_wr32(0x10000 + k, k * 0x01010101u ^ 0xC0FFEE00u);

	/* RDRAM → SRAM (PI_RD_LEN). */
	wr(PI_DRAM_ADDR, 0x10000);
	wr(PI_CART_ADDR, SRAM);
	wr(PI_RD_LEN, 0x7FFF);
	CHECK(memcmp(save.data, t_n64.bus.rdram + 0x10000, 0x8000) == 0);
	CHECK_EQ(save.writes, 1);
	CHECK(rd(PI_STATUS) & SALSA64_PI_STATUS_INTR);
	CHECK((rd(MI_INTR) >> SALSA64_IRQ_PI) & 1);
	CHECK_EQ(rd(PI_DRAM_ADDR), 0x18000);
	CHECK_EQ(rd(PI_CART_ADDR), SRAM + 0x8000);
	wr(PI_STATUS, 0x2);
	CHECK_EQ(rd(PI_STATUS) & SALSA64_PI_STATUS_INTR, 0);

	/* Los mismos datos otra vez: no cuenta como escritura. */
	wr(PI_DRAM_ADDR, 0x10000);
	wr(PI_CART_ADDR, SRAM);
	wr(PI_RD_LEN, 0x7FFF);
	CHECK_EQ(save.writes, 1);

	/* SRAM → RDRAM (PI_WR_LEN), desde un espejo y cruzando el final. */
	wr(PI_DRAM_ADDR, 0x20000);
	wr(PI_CART_ADDR, SRAM + 0x18000 + 0x7FF0);
	wr(PI_WR_LEN, 0x1F);
	CHECK(memcmp(t_n64.bus.rdram + 0x20000, save.data + 0x7FF0, 16) == 0);
	CHECK(memcmp(t_n64.bus.rdram + 0x20010, save.data, 16) == 0);
	CHECK((rd(MI_INTR) >> SALSA64_IRQ_PI) & 1);
	CHECK_EQ(rd(PI_CART_ADDR), SRAM + 0x18000 + 0x8010);
	unplug();
}

/* Sin SRAM el dominio 2 se lee en ceros y el DMA hacia el cartucho solo
 * levanta la interrupción, como antes. */
static void sram_absent(void)
{
	plug(SALSA64_SAVE_EEPROM_4K);
	t_wr32(0x20000, 0x12345678);
	wr(PI_DRAM_ADDR, 0x20000);
	wr(PI_CART_ADDR, SRAM);
	wr(PI_WR_LEN, 0x7);
	CHECK_EQ(t_rd32(0x20000), 0);
	wr(PI_STATUS, 0x2);
	wr(PI_RD_LEN, 0x7);
	CHECK((rd(MI_INTR) >> SALSA64_IRQ_PI) & 1);
	CHECK_EQ(rd(SRAM + 0x10), 0);
	wr(SRAM + 0x10, 0xDEADBEEF);
	CHECK_EQ(save.writes, 0);
	unplug();
}

/* Lecturas y escrituras directas de 32 bits, también desde el CPU. */
static void sram_direct(void)
{
	plug(SALSA64_SAVE_SRAM_256K);
	wr(SRAM + 0x100, 0xCAFEF00D);
	CHECK_EQ(salsa64_rd32(save.data + 0x100), 0xCAFEF00Du);
	CHECK_EQ(rd(SRAM + 0x100), 0xCAFEF00Du);
	CHECK_EQ(rd(SRAM + 0x8100), 0xCAFEF00Du);  /* espejo */
	CHECK_EQ(save.writes, 1);

	/* lui t0, 0xA800; lw t1, 0x100(t0); sw t1, 0x7FFC(t0) */
	T_LOAD(LUI(T0, 0xA800), LW(T1, 0x100, T0), SW(T1, 0x7FFC, T0));
	t_step(3);
	CHECK_EQ(R[T1], 0xFFFFFFFFCAFEF00Dull);
	CHECK_EQ(salsa64_rd32(save.data + 0x7FFC), 0xCAFEF00Du);
	CHECK_EQ(save.writes, 2);
	unplug();
}

/* El guardado sobrevive al reset de la máquina (y al arranque, que lo hace). */
static void save_survives_reset(void)
{
	plug(SALSA64_SAVE_SRAM_256K);
	wr(SRAM, 0x01020304);
	salsa64_n64_reset(&t_n64);
	CHECK(t_n64.bus.save == &save);
	CHECK_EQ(rd(SRAM), 0x01020304u);
	unplug();
}

static void save_load(void)
{
	static uint8_t file[600];
	memset(file, 0x42, sizeof file);
	salsa64_save_init(&save, SALSA64_SAVE_EEPROM_4K);
	CHECK(salsa64_save_load(&save, file, 512));
	CHECK_EQ(save.data[511], 0x42);
	CHECK_EQ(save.writes, 0);

	salsa64_save_init(&save, SALSA64_SAVE_EEPROM_16K);
	CHECK(!salsa64_save_load(&save, file, 512));  /* archivo de 4 Kbit */
	CHECK_EQ(save.data[511], 0x42);
	CHECK_EQ(save.data[512], 0xFF);
	CHECK(!salsa64_save_load(&save, file, sizeof file));

	/* Los tipos sin emular quedan como ninguno. */
	salsa64_save_init(&save, SALSA64_SAVE_SRAM_768K);
	CHECK_EQ(save.type, SALSA64_SAVE_NONE);
	CHECK_EQ(save.size, 0);
}

/* ---- Tipo de cada ROM ---------------------------------------------------------- */

static void rom_code(const char *code, uint8_t version)
{
	memset(rom, 0, sizeof rom);
	memcpy(rom + 0x3B, code, 4);
	rom[0x3F] = version;
}

static enum salsa64_save_type detect(int manual, enum salsa64_save_source *src)
{
	return salsa64_save_detect(rom, manual, src);
}

static void detect_header(void)
{
	static const struct {
		uint8_t byte;
		enum salsa64_save_type type;
	} cases[] = {
		{ 0x00, SALSA64_SAVE_NONE }, { 0x10, SALSA64_SAVE_EEPROM_4K },
		{ 0x20, SALSA64_SAVE_EEPROM_16K }, { 0x31, SALSA64_SAVE_SRAM_256K },
		{ 0x40, SALSA64_SAVE_SRAM_768K }, { 0x52, SALSA64_SAVE_FLASHRAM },
		{ 0x60, SALSA64_SAVE_SRAM_1M }, { 0xF0, SALSA64_SAVE_NONE },
	};
	enum salsa64_save_source src;
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		rom_code("NED", cases[k].byte);
		rom[0x3B] = 0;  /* libdragon no pone categoría */
		CHECK_EQ(detect(SALSA64_SAVE_AUTO, &src), cases[k].type);
		CHECK_EQ(src, SALSA64_SAVE_FROM_HEADER);
	}
	/* El ajuste manual manda. */
	CHECK_EQ(detect(SALSA64_SAVE_EEPROM_4K, &src), SALSA64_SAVE_EEPROM_4K);
	CHECK_EQ(src, SALSA64_SAVE_FROM_CONFIG);
}

static void detect_table(void)
{
	static const struct {
		const char *code;
		uint8_t version;
		enum salsa64_save_type type;
	} cases[] = {
		/* Los juegos objetivo (docs/proyecto.md). */
		{ "NSME", 0, SALSA64_SAVE_EEPROM_4K }, { "NSMJ", 3, SALSA64_SAVE_EEPROM_4K },
		{ "NZLE", 2, SALSA64_SAVE_SRAM_256K }, { "NZLP", 0, SALSA64_SAVE_SRAM_256K },
		{ "CZLJ", 0, SALSA64_SAVE_SRAM_256K }, { "NKTE", 0, SALSA64_SAVE_EEPROM_4K },
		{ "NFXE", 0, SALSA64_SAVE_EEPROM_4K }, { "NGEE", 0, SALSA64_SAVE_EEPROM_4K },
		{ "NPDE", 1, SALSA64_SAVE_EEPROM_16K },
		/* Excepciones por región y versión. */
		{ "NK4J", 0, SALSA64_SAVE_SRAM_256K }, { "NK4J", 1, SALSA64_SAVE_SRAM_256K },
		{ "ND3J", 0, SALSA64_SAVE_EEPROM_16K }, { "NWTJ", 0, SALSA64_SAVE_EEPROM_4K },
		/* FlashRAM: Majora's Mask en las tres regiones, Paper Mario,
		 * Pokémon Snap, Pokémon Puzzle League y Doubutsu no Mori. */
		{ "NZSE", 0, SALSA64_SAVE_FLASHRAM }, { "NZSJ", 1, SALSA64_SAVE_FLASHRAM },
		{ "NZSP", 0, SALSA64_SAVE_FLASHRAM }, { "NMQE", 0, SALSA64_SAVE_FLASHRAM },
		{ "NMQP", 0, SALSA64_SAVE_FLASHRAM }, { "NPFE", 0, SALSA64_SAVE_FLASHRAM },
		{ "NPFJ", 0, SALSA64_SAVE_FLASHRAM }, { "NPNE", 0, SALSA64_SAVE_FLASHRAM },
		{ "NAFJ", 0, SALSA64_SAVE_FLASHRAM },
		/* Reconocido sin emular. */
		{ "CDZJ", 0, SALSA64_SAVE_SRAM_768K },
	};
	enum salsa64_save_source src;
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		rom_code(cases[k].code, cases[k].version);
		CHECK_EQ(detect(SALSA64_SAVE_AUTO, &src), cases[k].type);
		CHECK_EQ(src, SALSA64_SAVE_FROM_TABLE);
	}
	/* Sin dato (o con fuentes en desacuerdo): ninguno. Donald Duck (NDQ),
	 * Rush 2049 (NRU) y Hydro Thunder (NHT) guardan solo en el Controller
	 * Pak: sin tipo de cartucho. */
	static const char *const unknown[] = { "NK4E", "ND3E", "NWTP", "NXXE", "NDQE", "NDQP", "NRUE", "NHTE" };
	for (unsigned k = 0; k < sizeof unknown / sizeof unknown[0]; k++) {
		rom_code(unknown[k], 0);
		CHECK_EQ(detect(SALSA64_SAVE_AUTO, &src), SALSA64_SAVE_NONE);
		CHECK_EQ(src, SALSA64_SAVE_FROM_NONE);
	}
	rom_code("NK4J", 2);
	CHECK_EQ(detect(SALSA64_SAVE_AUTO, &src), SALSA64_SAVE_NONE);
}

static void config(void)
{
	static const char text[] =
		"# tipo de guardado por juego\n"
		"* = eeprom4k\n"
		"nzl = SRAM   # todas las regiones\n"
		"NZLP = eeprom16k\n"
		"\n"
		"EC7011B7 = ninguno\r\n"
		"NZLE sram256k\n"
		"NSM = cinta\n";
	int bad;
	char err[64];
	rom_code("NZLE", 0);
	salsa64_wr32(rom + 0x10, 0x12345678);
	CHECK_EQ(salsa64_save_parse_config(text, sizeof text - 1, rom, &bad, err, sizeof err),
		 SALSA64_SAVE_SRAM_256K);
	CHECK_EQ(bad, 2);
	CHECK(strcmp(err, "línea 7: \"NZLE sram256k\"") == 0);

	rom_code("NZLP", 0);
	CHECK_EQ(salsa64_save_parse_config(text, sizeof text - 1, rom, &bad, NULL, 0),
		 SALSA64_SAVE_EEPROM_16K);
	salsa64_wr32(rom + 0x10, 0xEC7011B7);
	CHECK_EQ(salsa64_save_parse_config(text, sizeof text - 1, rom, &bad, NULL, 0), SALSA64_SAVE_NONE);
	rom_code("NKTE", 0);
	CHECK_EQ(salsa64_save_parse_config(text, sizeof text - 1, rom, &bad, NULL, 0),
		 SALSA64_SAVE_EEPROM_4K);
	CHECK_EQ(salsa64_save_parse_config("NKT = sram1m\n", 13, rom, &bad, NULL, 0), SALSA64_SAVE_SRAM_1M);
	CHECK_EQ(salsa64_save_parse_config("NSM = eeprom4k", 14, rom, &bad, NULL, 0), SALSA64_SAVE_AUTO);
	CHECK_EQ(bad, 0);
}

static void filename(void)
{
	char name[64];
	memset(rom, 0, sizeof rom);
	memcpy(rom + 0x20, "THE LEGEND OF ZELDA ", 20);
	salsa64_wr32(rom + 0x10, 0xEC7011B7);
	salsa64_save_filename(rom, SALSA64_SAVE_SRAM_256K, name, sizeof name);
	CHECK(strcmp(name, "THE_LEGEND_OF_ZELDA-EC7011B7.sra") == 0);

	memcpy(rom + 0x20, "  Mario/Kart:64\xB0\xB1    ", 20);
	salsa64_save_filename(rom, SALSA64_SAVE_EEPROM_16K, name, sizeof name);
	CHECK(strcmp(name, "Mario_Kart_64-EC7011B7.eep") == 0);

	memcpy(rom + 0x20, "ZELDA MAJORA'S MASK ", 20);
	salsa64_wr32(rom + 0x10, 0x5354631C);
	salsa64_save_filename(rom, SALSA64_SAVE_FLASHRAM, name, sizeof name);
	CHECK(strcmp(name, "ZELDA_MAJORA_S_MASK-5354631C.fla") == 0);

	memset(rom + 0x20, ' ', 20);
	salsa64_wr32(rom + 0x10, 0x00ABCDEF);
	salsa64_save_filename(rom, SALSA64_SAVE_EEPROM_4K, name, sizeof name);
	CHECK(strcmp(name, "rom-00ABCDEF.eep") == 0);
}

/* ---- Volcado diferido ----------------------------------------------------------- */

static void flush_policy(void)
{
	const uint64_t s = 1000000;
	salsa64_save_init(&save, SALSA64_SAVE_SRAM_256K);
	CHECK(!salsa64_save_due(&save, 0));

	save.writes++;  /* una escritura del juego */
	CHECK(!salsa64_save_due(&save, 10 * s));
	CHECK(salsa64_save_dirty(&save));
	CHECK(!salsa64_save_due(&save, 11 * s));
	CHECK(salsa64_save_due(&save, 12 * s));  /* 2 s sin escrituras */
	salsa64_save_flushed(&save);
	CHECK(!salsa64_save_dirty(&save));
	CHECK(!salsa64_save_due(&save, 20 * s));

	/* Escrituras cada segundo: se vuelca a los 30 s del primer cambio. */
	uint64_t t = 100 * s;
	for (int k = 0; k < 30; k++, t += s) {
		save.writes++;
		CHECK(!salsa64_save_due(&save, t));
	}
	save.writes++;
	CHECK(salsa64_save_due(&save, t));
}

void t_suite_save(void)
{
	RUN(eeprom_info);
	RUN(eeprom_read_write);
	RUN(eeprom_absent);
	RUN(eeprom_through_si);
	RUN(sram_dma);
	RUN(sram_absent);
	RUN(sram_direct);
	RUN(save_survives_reset);
	RUN(save_load);
	RUN(detect_header);
	RUN(detect_table);
	RUN(config);
	RUN(filename);
	RUN(flush_policy);
}
