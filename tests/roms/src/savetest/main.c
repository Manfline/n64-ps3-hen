/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — savetest: ROM de prueba del guardado del cartucho (EEPROM de 4
 * y 16 Kbit, SRAM de 256 Kbit, FlashRAM de 1 Mbit). Código propio, sin
 * libultra ni libdragon; se empaqueta con el IPL3 libre de libdragon
 * (nust64).
 *
 * El tipo de guardado lo declara la ROM en su cabecera de homebrew ("ED" en
 * 0x3C–0x3D, tipo en los 4 bits altos de 0x3F): tools/build-savetest.sh
 * saca cuatro ROMs que solo difieren en el título y en esa cabecera. La ROM
 * lee su cabecera del cartucho y comprueba que el emulador le dé justo eso:
 * la EEPROM de ese tamaño en el canal 4 del PIF, o la SRAM o la FlashRAM en
 * el dominio 2 del PI y nada en el canal 4.
 *
 * En cada arranque:
 *   1. Lee todo el guardado. Si empieza con la marca "SLSA", verifica el
 *      patrón que dejó el arranque anterior (su número va en la marca): así
 *      se prueba que el guardado persistió. Si no persistió, lo informa y
 *      sigue, para que el arranque siguiente vuelva a probar.
 *   2. Escribe en todo el guardado el patrón del arranque N = anterior + 1
 *      (EEPROM: bloque a bloque con el comando 0x05; SRAM: DMA RDRAM →
 *      cartucho salvo el último KB, que va con escrituras directas del CPU;
 *      FlashRAM: borra el chip, comprueba que quede en 0xFF y programa las
 *      1024 páginas) y lo relee completo (EEPROM: comando 0x04; SRAM y
 *      FlashRAM: DMA cartucho → RDRAM y unas lecturas directas). En la
 *      EEPROM de 4 Kbit comprueba además que los bloques 64–255 repitan los
 *      0–63; en la FlashRAM, que borrar el último sector no toque el
 *      anterior, y vuelve a programarlo.
 *   3. Imprime por el ISViewer "savetest: OK <tipo> arranque N …" o
 *      "savetest: FALLO <tipo> …" y pinta la pantalla: azul en el primer
 *      arranque (falta ver la persistencia), verde si el anterior persistió,
 *      rojo si algo falló; arriba, N cuadros blancos (hasta 20).
 *
 * Comandos del PIF y registros del PI y del VI: n64brew.dev (Joybus
 * Protocol, Peripheral Interface, Video Interface). FlashRAM: n64brew.dev,
 * página «Flash» (comandos en 0x08010000, identificador por DMA de 8
 * bytes, chips que leen por palabra o por byte, ráfagas que no cruzan los
 * bloques de 32 KB). Modo de video: el estándar de 320×240 que usa vitest.
 */
#include <stdint.h>

#define REG(pa) (*(volatile uint32_t *)(0xA0000000u | (pa)))

#define VI_CONTROL   REG(0x04400000)
#define VI_ORIGIN    REG(0x04400004)
#define VI_WIDTH     REG(0x04400008)
#define VI_V_INTR    REG(0x0440000C)
#define VI_V_CURRENT REG(0x04400010)
#define VI_BURST     REG(0x04400014)
#define VI_V_SYNC    REG(0x04400018)
#define VI_H_SYNC    REG(0x0440001C)
#define VI_H_SYNC_LEAP REG(0x04400020)
#define VI_H_VIDEO   REG(0x04400024)
#define VI_V_VIDEO   REG(0x04400028)
#define VI_V_BURST   REG(0x0440002C)
#define VI_X_SCALE   REG(0x04400030)
#define VI_Y_SCALE   REG(0x04400034)
#define PI_DRAM_ADDR REG(0x04600000)
#define PI_CART_ADDR REG(0x04600004)
#define PI_RD_LEN    REG(0x04600008)  /* RDRAM → cartucho */
#define PI_WR_LEN    REG(0x0460000C)  /* cartucho → RDRAM */
#define PI_STATUS    REG(0x04600010)
#define PI_DOM1(k)   REG(0x04600014 + 4 * (k))
#define PI_DOM2(k)   REG(0x04600024 + 4 * (k))
#define SI_DRAM_ADDR REG(0x04800000)
#define SI_PIF_RD64B REG(0x04800004)
#define SI_PIF_WR64B REG(0x04800010)
#define SI_STATUS    REG(0x04800018)
#define PIF_RAM      0x1FC007C0u
#define SRAM_ADDR    0x08000000u
#define FLASH_CMD    REG(0x08010000)
#define FLASH_TYPE   0x11118001u
#define ROM_HEADER   0x1000003Cu      /* "ED", destino y tipo de guardado */
#define BOOT_FLAGS   REG(0x04000008)  /* DMEM: el IPL3 de libdragon deja el tipo de TV en [23:16] */

#define ISV_MAGIC    REG(0x13FF0000)
#define ISV_LEN      REG(0x13FF0014)
#define ISV_BUF      0x13FF0020u

#define W 320
#define H 240
#define CTRL_16 0x3202u  /* pixel advance 3, solo remuestreo, RGBA5551 */
#define SAVE_MAX 131072u
#define DIRECT 1024u     /* SRAM: el último KB va por accesos directos */
#define FLASH_PAGE 128u
#define FLASH_SECTOR 16384u
#define FLASH_BURST 32768u  /* una ráfaga de DMA no cruza estos bloques */
#define MAGIC 0x534C5341u  /* "SLSA" */

enum { BLUE = 0x0010u << 1 | 1, GREEN = 0x0018u << 6 | 1, RED = 0x001Fu << 11 | 1, WHITE = 0xFFFFu };

static const struct {
	const char *name;
	uint32_t size;
	uint32_t eeprom_id;  /* identificador en el canal 4; 0: nada */
} types[] = {
	[1] = { "eeprom4k", 512, 0x0080 },
	[2] = { "eeprom16k", 2048, 0x00C0 },
	[3] = { "sram256k", 32768, 0 },
	[5] = { "flashram", SAVE_MAX, 0 },
};

/* Fuera de .bss: solo se usan por KSEG1 (sin caché, por el DMA). */
static uint16_t fb[W * H] __attribute__((section(".fb"), aligned(64)));
static uint8_t data[SAVE_MAX] __attribute__((section(".fb"), aligned(64)));
static uint32_t pif_buf[16] __attribute__((section(".fb"), aligned(64)));
static uint32_t flash_id[4] __attribute__((section(".fb"), aligned(64)));

void *memset(void *d, int c, __SIZE_TYPE__ n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
	uint8_t *p = d;
	const uint8_t *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

static volatile void *uncached(const void *p)
{
	return (volatile void *)((uint32_t)p | 0x20000000u);
}

/* ---- ISViewer ------------------------------------------------------------ */

static int isv;
static char msg[200];
static unsigned msg_len;

static void put(const char *s)
{
	while (*s && msg_len < sizeof msg)
		msg[msg_len++] = *s++;
}

static void put_u(uint32_t v)
{
	char tmp[11];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n && msg_len < sizeof msg)
		msg[msg_len++] = tmp[--n];
}

static void put_x(uint32_t v, int digits)
{
	put("0x");
	while (digits--)
		if (msg_len < sizeof msg)
			msg[msg_len++] = "0123456789ABCDEF"[(v >> (4 * digits)) & 15];
}

static void isv_flush(void)
{
	if (isv) {
		for (unsigned i = 0; i < msg_len; i += 4) {
			uint32_t w = 0;
			for (unsigned k = 0; k < 4; k++)
				w = w << 8 | (i + k < msg_len ? (uint8_t)msg[i + k] : 0);
			REG(ISV_BUF + i) = w;
		}
		ISV_LEN = msg_len;
	}
	msg_len = 0;
}

/* ---- EEPROM (canal 4 del PIF) -------------------------------------------- */

static void si_wait(void)
{
	while (SI_STATUS & 3)  /* DMA o E/S en curso */
		;
}

/* Un comando en el canal 4 con el formato de libultra (un 0x00 por cada
 * puerto de control). Devuelve el byte RX con los bits de error del PIF y
 * deja la respuesta en `rx`. */
static uint8_t eep_command(const uint8_t *tx, unsigned txlen, uint8_t *rx, unsigned rxlen)
{
	volatile uint32_t *buf = uncached(pif_buf);
	uint8_t frame[64];
	unsigned at = 4;
	memset(frame, 0, sizeof frame);
	frame[at] = (uint8_t)txlen;
	frame[at + 1] = (uint8_t)rxlen;
	memcpy(frame + at + 2, tx, txlen);
	memset(frame + at + 2 + txlen, 0xFF, rxlen);
	frame[at + 2 + txlen + rxlen] = 0xFE;
	frame[63] = 0x01;  /* ejecutar joybus */
	for (unsigned i = 0; i < 64; i += 4)
		buf[i / 4] = (uint32_t)frame[i] << 24 | (uint32_t)frame[i + 1] << 16 |
			     (uint32_t)frame[i + 2] << 8 | frame[i + 3];
	si_wait();
	SI_DRAM_ADDR = (uint32_t)pif_buf & 0x1FFFFFFF;
	SI_PIF_WR64B = PIF_RAM;
	si_wait();
	SI_STATUS = 0;
	SI_DRAM_ADDR = (uint32_t)pif_buf & 0x1FFFFFFF;
	SI_PIF_RD64B = PIF_RAM;
	si_wait();
	SI_STATUS = 0;
	for (unsigned i = 0; i < 64; i += 4) {
		uint32_t w = buf[i / 4];
		for (unsigned k = 0; k < 4; k++)
			frame[i + k] = (uint8_t)(w >> (24 - 8 * k));
	}
	memcpy(rx, frame + at + 2 + txlen, rxlen);
	return frame[at + 1];
}

/* Identificador del dispositivo del canal 4, o 0 si no contesta. */
static uint32_t eep_probe(void)
{
	static const uint8_t tx[] = { 0x00 };
	uint8_t r[3];
	if (eep_command(tx, 1, r, 3) & 0xC0)
		return 0;
	return (uint32_t)r[0] << 8 | r[1];
}

static int eep_read(unsigned block, volatile uint8_t *out)
{
	uint8_t tx[2] = { 0x04, (uint8_t)block }, r[8];
	if (eep_command(tx, 2, r, 8) & 0xC0)
		return 0;
	for (int k = 0; k < 8; k++)
		out[k] = r[k];
	return 1;
}

/* Devuelve el byte de estado de la EEPROM (0x00: bien), o 0x100 si no
 * contestó. */
static uint32_t eep_write(unsigned block, const volatile uint8_t *in)
{
	uint8_t tx[10] = { 0x05, (uint8_t)block }, r[1];
	for (int k = 0; k < 8; k++)
		tx[2 + k] = in[k];
	if (eep_command(tx, 10, r, 1) & 0xC0)
		return 0x100;
	return r[0];
}

/* ---- SRAM (dominio 2 del PI) ---------------------------------------------- */

static void pi_wait(void)
{
	while (PI_STATUS & 3)  /* DMA o E/S en curso */
		;
}

static void sram_dma(uint32_t off, const void *rdram, uint32_t len, int to_sram)
{
	pi_wait();
	PI_DRAM_ADDR = (uint32_t)rdram & 0x1FFFFFFF;
	PI_CART_ADDR = SRAM_ADDR + off;
	if (to_sram)
		PI_RD_LEN = len - 1;
	else
		PI_WR_LEN = len - 1;
	pi_wait();
	PI_STATUS = 2;  /* baja la interrupción del PI */
}

/* ---- FlashRAM (dominio 2 del PI) ----------------------------------------- */

static int flash_words;  /* el chip lee por palabras de 16 bits */

static void flash_command(uint32_t c)
{
	pi_wait();
	FLASH_CMD = c;
	pi_wait();
}

/* Registro de estado: modo estado y una lectura de E/S. El comando va dos
 * veces: algunos MX29L1100 lo necesitan para cambiar de modo (n64brew). */
static uint32_t flash_status(void)
{
	flash_command(0xD2000000);
	flash_command(0xD2000000);
	pi_wait();
	return REG(SRAM_ADDR) & 0xFF;
}

/* Modo estado y un 0 en la dirección 0. */
static void flash_clear_status(void)
{
	flash_command(0xD2000000);
	pi_wait();
	REG(SRAM_ADDR) = 0;
	pi_wait();
}

/* Identificador (modo E1 y DMA de 8 bytes); según el fabricante y el
 * dispositivo, direcciones por palabra (MX29L0000, MX29L0001, MX29L1100) o
 * por byte (los demás). Devuelve la palabra del fabricante y el dispositivo,
 * o 0 si la primera no es 0x11118001. */
static uint32_t flash_identify(void)
{
	volatile uint32_t *id = uncached(flash_id);
	id[0] = id[1] = 0;
	flash_command(0xE1000000);
	flash_command(0xE1000000);
	sram_dma(0, flash_id, 8, 0);
	uint32_t model = id[1];
	flash_words = model == 0x00C20000u || model == 0x00C20001u || model == 0x00C2001Eu;
	return id[0] == FLASH_TYPE ? model : 0;
}

/* Modo lectura y DMA desde el byte `off`, partido en los bloques de 32 KB. */
static void flash_read(uint32_t off, void *rdram, uint32_t len)
{
	flash_command(0xF0000000);
	pi_wait();
	(void)REG(SRAM_ADDR);  /* lectura de E/S antes del DMA, como libultra */
	uint8_t *dst = rdram;
	while (len) {
		uint32_t n = FLASH_BURST - off % FLASH_BURST;
		if (n > len)
			n = len;
		sram_dma(flash_words ? off / 2 : off, dst, n, 0);
		off += n;
		dst += n;
		len -= n;
	}
}

/* Espera a que se apague `busy` en el estado y devuelve el estado final. */
static uint32_t flash_wait(uint32_t busy)
{
	uint32_t st = 0;
	for (int k = 0; k < 1000000; k++) {
		pi_wait();
		st = REG(SRAM_ADDR) & 0xFF;
		if (!(st & busy))
			break;
	}
	return st;
}

/* Borra el chip (sector < 0) o un sector. Devuelve el estado final. */
static uint32_t flash_erase(int sector)
{
	flash_command(sector < 0 ? 0x3C000000 : 0x4B000000 | (uint32_t)sector * 128);
	flash_command(0x78000000);
	uint32_t st = flash_wait(0x02);
	flash_clear_status();
	return st;
}

/* Programa una página: modo búfer de página, DMA de 128 bytes y comando
 * A5. Devuelve el estado final. */
static uint32_t flash_program(uint32_t page, const void *rdram)
{
	flash_command(0xB4000000);
	sram_dma(0, rdram, FLASH_PAGE, 1);
	flash_command(0xA5000000 | page);
	uint32_t st = flash_wait(0x01);
	flash_clear_status();
	return st;
}

/* ---- Patrón ------------------------------------------------------------- */

static uint8_t pattern(uint32_t boot, uint32_t i)
{
	uint32_t x = (i + 1) * 0x9E3779B1u ^ boot * 0x85EBCA77u;
	x ^= x >> 15;
	return (uint8_t)(x ^ x >> 8);
}

static uint32_t rd_be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Byte esperado en la posición i del guardado del arranque `boot`. */
static uint8_t expected(uint32_t boot, uint32_t size, uint32_t i)
{
	uint32_t hdr[4] = { MAGIC, boot, size, ~boot };
	if (i < 16)
		return (uint8_t)(hdr[i / 4] >> (24 - 8 * (i % 4)));
	return pattern(boot, i);
}

/* Primera posición que no coincide con el arranque `boot`, o -1. */
static int32_t mismatch(const volatile uint8_t *d, uint32_t boot, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++)
		if (d[i] != expected(boot, size, i))
			return (int32_t)i;
	return -1;
}

/* ---- Pantalla ------------------------------------------------------------- */

static void show(uint16_t color, uint32_t boot)
{
	volatile uint16_t *p = uncached(fb);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			int k = (x - 16) / 12;
			int square = y >= 16 && y < 24 && x >= 16 && (x - 16) % 12 < 8 && k < 20 && (uint32_t)k < boot;
			p[y * W + x] = square ? WHITE : color;
		}

	int pal = ((BOOT_FLAGS >> 16) & 0xFF) == 0;  /* 0 = PAL, 1 = NTSC, 2 = MPAL */
	VI_CONTROL = 0;
	VI_ORIGIN = (uint32_t)fb & 0x1FFFFFFF;
	VI_WIDTH = W;
	VI_V_INTR = 0x3FF;  /* sin interrupción */
	VI_V_CURRENT = 0;
	if (pal) {
		VI_BURST = 0x0404233A;
		VI_V_SYNC = 0x271;
		VI_H_SYNC = 0x00150C69;
		VI_H_SYNC_LEAP = 0x0C6F0C6E;
		VI_H_VIDEO = 0x00800300;
		VI_V_VIDEO = 0x005F0239;
		VI_V_BURST = 0x0009026B;
	} else {
		VI_BURST = 0x03E52239;
		VI_V_SYNC = 0x20D;
		VI_H_SYNC = 0x00000C15;
		VI_H_SYNC_LEAP = 0x0C150C15;
		VI_H_VIDEO = 0x006C02EC;
		VI_V_VIDEO = 0x002501FF;
		VI_V_BURST = 0x000E0204;
	}
	VI_X_SCALE = 0x200;  /* 320 / 640 en 2.10 */
	VI_Y_SCALE = 0x400;
	VI_CONTROL = CTRL_16;
}

/* ---- Programa ------------------------------------------------------------ */

extern const uint32_t exc_vector[2];

static void install_exc_vector(void)
{
	volatile uint32_t *vec = (volatile uint32_t *)0xA0000180u;
	vec[0] = exc_vector[0];
	vec[1] = exc_vector[1];
	__asm__ volatile(".set push\n\t.set mips3\n\tcache 0x10, 0(%0)\n\t.set pop" : : "r"(0x80000180u));
}

static const char *type_name = "?";

static void fail_begin(void)
{
	put("savetest: FALLO ");
	put(type_name);
	put(": ");
}

static void fail_byte(const char *what, int32_t at, const volatile uint8_t *d, uint32_t boot, uint32_t size)
{
	fail_begin();
	put(what);
	put(": byte ");
	put_x((uint32_t)at, 4);
	put(" = ");
	put_x(d[at], 2);
	put(", se esperaba ");
	put_x(expected(boot, size, (uint32_t)at), 2);
	put(" (arranque ");
	put_u(boot);
	put(")\n");
	isv_flush();
}

static int flash;  /* el guardado es FlashRAM */

/* Lee todo el guardado en `d`. */
static int read_all(volatile uint8_t *d, uint32_t size, int eeprom)
{
	if (flash) {
		flash_read(0, data, size);
		return 1;
	}
	if (!eeprom) {
		sram_dma(0, data, size, 0);
		return 1;
	}
	for (uint32_t b = 0; b < size / 8; b++)
		if (!eep_read(b, d + 8 * b))
			return 0;
	return 1;
}

int main(void)
{
	volatile uint8_t *d = uncached(data);
	uint32_t hdr = REG(ROM_HEADER), kind = (hdr & 0xFF) >> 4;
	int ok = 1;

	install_exc_vector();
	ISV_MAGIC = 0x56495354;  /* "VIST": la ROM del cartucho no lo guardaría */
	isv = ISV_MAGIC == 0x56495354;
	for (int k = 0; k < 4; k++)
		PI_DOM2(k) = PI_DOM1(k);  /* el emulador no mide tiempos del bus */

	if (hdr >> 16 != 0x4544 || kind >= sizeof types / sizeof types[0] || !types[kind].name) {
		put("savetest: FALLO: la cabecera no declara un tipo de guardado (\"ED\" en 0x3C): ");
		put_x(hdr, 8);
		put("\n");
		isv_flush();
		show(RED, 0);
		for (;;)
			;
	}
	type_name = types[kind].name;
	uint32_t size = types[kind].size, want = types[kind].eeprom_id;
	int eeprom = want != 0;
	flash = size == SAVE_MAX;

	/* 1. Lo que hay en el canal 4 y lo que dejó el arranque anterior. */
	uint32_t id = eep_probe();
	if (id != want) {
		fail_begin();
		put("el canal 4 contesta ");
		put_x(id, 4);
		put(" y se esperaba ");
		put_x(want, 4);
		put("\n");
		isv_flush();
		ok = 0;
	}
	if (ok && flash) {
		uint32_t model = flash_identify();
		if (!model) {
			fail_begin();
			put("el identificador no empieza con ");
			put_x(FLASH_TYPE, 8);
			put("\n");
			isv_flush();
			ok = 0;
		} else {
			put("savetest: FlashRAM ");
			put_x(model, 8);
			put(flash_words ? " (por palabras)\n" : " (por bytes)\n");
			isv_flush();
		}
		uint32_t st = flash_status();
		if (ok && !(st & 0x80)) {
			fail_begin();
			put("el estado no dice WSM_READY: ");
			put_x(st, 2);
			put("\n");
			isv_flush();
			ok = 0;
		}
	}
	uint32_t prev = 0;
	if (ok && !read_all(d, size, eeprom)) {
		fail_begin();
		put("la EEPROM no contestó a la lectura\n");
		isv_flush();
		ok = 0;
	}
	int persisted = ok;
	if (ok && rd_be32(d) == MAGIC) {
		prev = rd_be32(d + 4);
		int32_t at = mismatch(d, prev, size);
		if (at >= 0) {
			fail_byte("persistencia", at, d, prev, size);
			persisted = 0;  /* se sigue: el arranque siguiente vuelve a probar */
		}
	}
	uint32_t boot = prev + 1;

	/* 2. Patrón del arranque N: escribir y releer. */
	if (ok) {
		for (uint32_t i = 0; i < size; i++)
			d[i] = expected(boot, size, i);
		if (flash) {
			uint32_t st = flash_erase(-1);
			if (!(st & 0x08)) {
				fail_begin();
				put("borrado del chip: estado ");
				put_x(st, 2);
				put("\n");
				isv_flush();
				ok = 0;
			}
			if (ok) {
				for (uint32_t i = 0; i < size; i++)
					d[i] = 0;
				flash_read(0, data, size);
				for (uint32_t i = 0; i < size && ok; i++)
					if (d[i] != 0xFF) {
						fail_begin();
						put("después de borrar el chip, byte ");
						put_x(i, 5);
						put(" = ");
						put_x(d[i], 2);
						put("\n");
						isv_flush();
						ok = 0;
					}
				for (uint32_t i = 0; i < size; i++)
					d[i] = expected(boot, size, i);
			}
			for (uint32_t p = 0; p < size / FLASH_PAGE && ok; p++) {
				uint32_t st = flash_program(p, data + p * FLASH_PAGE);
				if (!(st & 0x04)) {
					fail_begin();
					put("programación de la página ");
					put_u(p);
					put(": estado ");
					put_x(st, 2);
					put("\n");
					isv_flush();
					ok = 0;
				}
			}
		} else if (eeprom) {
			for (uint32_t b = 0; b < size / 8 && ok; b++) {
				uint32_t st = eep_write(b, d + 8 * b);
				if (st != 0) {
					fail_begin();
					put("escritura del bloque ");
					put_u(b);
					put(": estado ");
					put_x(st, 2);
					put("\n");
					isv_flush();
					ok = 0;
				}
			}
		} else {
			sram_dma(0, data, size - DIRECT, 1);
			for (uint32_t i = size - DIRECT; i < size; i += 4) {
				pi_wait();
				REG(SRAM_ADDR + i) = rd_be32(d + i);
			}
			pi_wait();
		}
	}
	if (ok) {
		for (uint32_t i = 0; i < size; i++)
			d[i] = 0x55;
		int32_t at = read_all(d, size, eeprom) ? mismatch(d, boot, size) : 0;
		if (at >= 0) {
			fail_byte("relectura", at, d, boot, size);
			ok = 0;
		}
	}
	if (ok && eeprom && size == 512) {
		/* 4 Kbit: los bloques 64–255 repiten los 0–63. */
		static const uint8_t blocks[] = { 64, 64 + 7, 128 + 33, 255 };
		uint8_t r[8];
		for (unsigned k = 0; k < sizeof blocks && ok; k++) {
			unsigned b = blocks[k];
			uint32_t base = 8 * (b & 63);
			volatile uint8_t *rv = r;
			int same = eep_read(b, rv);
			for (int j = 0; j < 8 && same; j++)
				same = r[j] == expected(boot, size, base + (uint32_t)j);
			if (!same) {
				fail_begin();
				put("el bloque ");
				put_u(b);
				put(" no repite el ");
				put_u(b & 63);
				put("\n");
				isv_flush();
				ok = 0;
			}
		}
	}
	if (ok && flash) {
		/* Borrar el último sector no toca el anterior; después se vuelve a
		 * programar. */
		uint32_t last = size - FLASH_SECTOR;
		flash_erase(7);
		for (uint32_t i = last - 1024; i < size; i++)
			d[i] = 0x55;
		flash_read(last - 1024, data + last - 1024, 1024 + FLASH_SECTOR);
		for (uint32_t i = last - 1024; i < size && ok; i++) {
			uint8_t want_b = i < last ? expected(boot, size, i) : 0xFF;
			if (d[i] != want_b) {
				fail_begin();
				put("borrado del sector 7: byte ");
				put_x(i, 5);
				put(" = ");
				put_x(d[i], 2);
				put(", se esperaba ");
				put_x(want_b, 2);
				put("\n");
				isv_flush();
				ok = 0;
			}
		}
		for (uint32_t i = last; i < size; i++)
			d[i] = expected(boot, size, i);
		for (uint32_t p = last / FLASH_PAGE; p < size / FLASH_PAGE && ok; p++)
			if (!(flash_program(p, data + p * FLASH_PAGE) & 0x04))
				ok = 0;
		if (ok) {
			for (uint32_t i = 0; i < size; i++)
				d[i] = 0x55;
			int32_t at = read_all(d, size, eeprom) ? mismatch(d, boot, size) : 0;
			if (at >= 0) {
				fail_byte("relectura tras el sector 7", at, d, boot, size);
				ok = 0;
			}
		}
		flash_command(0xF0000000);
	}
	if (ok && !eeprom) {
		/* Lecturas directas del CPU, una por KB (la FlashRAM por palabras
		 * cuenta la dirección en medias palabras). */
		for (uint32_t i = 0x40; i < size && ok; i += 1024) {
			uint32_t at = flash && flash_words ? i / 2 : i;
			pi_wait();
			uint32_t got = REG(SRAM_ADDR + at), want_w = rd_be32(d + i);
			if (got != want_w) {
				fail_begin();
				put("lectura directa en ");
				put_x(SRAM_ADDR + at, 8);
				put(" = ");
				put_x(got, 8);
				put(", se esperaba ");
				put_x(want_w, 8);
				put("\n");
				isv_flush();
				ok = 0;
			}
		}
	}

	/* 3. Resultado. */
	ok = ok && persisted;
	if (ok) {
		put("savetest: OK ");
		put(type_name);
		put(" arranque ");
		put_u(boot);
		put(" (");
		if (prev) {
			put("persistió el arranque ");
			put_u(prev);
			put("; ");
		}
		put_u(size);
		put(" bytes escritos y releídos");
		if (!prev)
			put("; falta un segundo arranque para ver la persistencia");
		put(")\n");
		isv_flush();
	}
	show(!ok ? RED : prev ? GREEN : BLUE, boot);
	for (;;)
		;
}
