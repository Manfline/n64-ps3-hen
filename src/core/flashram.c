/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — FlashRAM de 1 Mbit (128 KB) del cartucho, en el dominio 2 del
 * PI (0x08000000). Ver save.h.
 *
 * Fuentes (docs/THIRD_PARTY.md), consultadas el 2026-10-11. El código es
 * propio, no se copió de ninguna:
 *   [n64brew] n64brew.dev, página «Flash»: mapa, comandos, modos, registro
 *             de estado, identificador, chips por palabra o por byte y la
 *             vuelta de las ráfagas en bloques de 32 KB.
 *   [ares]    ares, ares/n64/cartridge/flash.cpp (ISC, commit 4cb8d92):
 *             cotejo de lo anterior; su base de juegos (mia) da el
 *             MX29L1100 a Majora's Mask, Pokémon Snap y Pokémon Puzzle
 *             League.
 *   [m64p]    mupen64plus-core, src/device/cart/flashram.c y main.c
 *             (GPLv2+, commit ba95bab): un solo modelo, el MX29L1100, para
 *             todos los juegos, con borrado y programación instantáneos.
 *
 * Comandos: una escritura de 32 bits en 0x08010000 [n64brew]. Los bits
 * 23–10 se ignoran.
 *   3C000000  prepara el borrado del chip
 *   4B000ppp  prepara el borrado del sector de la página ppp (16 KB:
 *             sector = ppp >> 7)
 *   78000000  borra lo preparado (todo a 0xFF) y pasa a modo estado; sin
 *             preparación no hace nada [n64brew, ares]
 *   A5000ppp  programa la página ppp (128 bytes) con el búfer de página y
 *             pasa a modo estado; programar solo baja bits (AND) y el búfer
 *             vuelve a 0xFF [n64brew, ares]
 *   B4000000  modo búfer de página
 *   D2000000  modo estado
 *   E1000000  modo identificador
 *   F0000000  modo lectura (el de encendido)
 * Un comando desconocido no cambia nada; se cuenta (flash.unknown).
 *
 * Lo que se ve en el dominio según el modo:
 *   lectura          los datos. El MX29L1100 cuenta en palabras de 16 bits:
 *                    la dirección X (desde 0x08000000) lee desde el byte 2X,
 *                    y 0x08000000–0x0800FFFF cubren los 128 KB [n64brew,
 *                    ares; en m64p, «old flash»]. Una ráfaga de DMA no sale
 *                    de su bloque de 32 KB: vuelve al principio [n64brew,
 *                    ares]. libultra parte sus lecturas en esos bloques.
 *   estado           «00 SS» repetido, SS el registro [n64brew]
 *   identificador    11118001 00C2001E, repetido en un DMA más largo; una
 *                    lectura de 32 bits del CPU da solo la primera palabra
 *                    [n64brew]
 *   búfer de página  sus 128 bytes (dirección & 0x7F) [n64brew, ares]
 * Escrituras (del CPU o por DMA hacia el cartucho): en modo búfer de
 * página, los bytes tal cual (el MX29L1100 sube y baja bits, el de
 * Matsushita solo baja) [n64brew]; en modo estado, un 0 en la dirección 0
 * limpia el estado, que en el MX29L1100 deja ERASE_OK y PROGRAM_OK en 1, su
 * valor de reposo [n64brew, ares]; lo demás se ignora.
 *
 * Registro de estado en reposo: WSM_READY | ERASE_OK | PROGRAM_OK = 0x8C
 * [n64brew, ares].
 *
 * Simplificaciones:
 *   - Borrar y programar son instantáneos (en el chip, ~85 ms y ~3,5 ms):
 *     los bits BUSY nunca se ven y el estado se queda en 0x8C. libultra
 *     sondea BUSY con un temporizador y después mira ERASE_OK o
 *     PROGRAM_OK: le basta, como en m64p.
 *   - Rarezas del MX29L1100 que no se emulan [n64brew, ares]: que a veces
 *     haga falta mandar dos veces el comando de estado, que la mitad alta
 *     de la primera lectura de estado traiga el modo anterior, y el «bus
 *     abierto» al leer el estado en 0x08010000–0x0801FFFF.
 *   - Un solo modelo. libultra lee el identificador y elige direcciones por
 *     palabra o por byte, así que el juego se adapta al que se le dé; el
 *     MX29L1100 es el de los juegos de la lista de ares y el que m64p da a
 *     todos. El MX29L1101 (por byte) y el MN63F8MPN de Matsushita quedan
 *     fuera.
 *   - La dirección se toma módulo 128 KB en todo el dominio 2, como m64p (el
 *     SDK pondría un segundo chip en +0x20000; ningún juego lo usa
 *     [n64brew]). Un DMA hacia 0x08010000 toma sus 4 primeros bytes como
 *     comando.
 *
 * El archivo de guardado son los 128 KB en el orden de la N64, como la
 * SRAM.
 */
#include "core/save.h"

#include <string.h>

#include "core/endian.h"

#ifdef SALSA64_DEBUG_FLASH
/* Rastro para salsa64-run (run.c): 'C' comando, 'R'/'W' E/S del CPU, 'I'/'O'
 * DMA hacia la RDRAM o hacia el chip; `mode` antes del acceso. */
void salsa64_debug_flash(char kind, unsigned mode, uint32_t off, uint32_t value);
#define TRACE(k, m, o, v) salsa64_debug_flash(k, m, o, v)
#else
#define TRACE(k, m, o, v) ((void)0)
#endif

#define CMD_CHIP_ERASE   0x3C
#define CMD_SECTOR_ERASE 0x4B
#define CMD_ERASE        0x78
#define CMD_PROGRAM      0xA5
#define CMD_LOAD_PAGE    0xB4
#define CMD_STATUS       0xD2
#define CMD_SILICON_ID   0xE1
#define CMD_READ_ARRAY   0xF0

#define ERASE_NONE   0
#define ERASE_SECTOR 1
#define ERASE_CHIP   2

#define SECTOR      0x4000u  /* 128 páginas */
#define BURST       0x8000u  /* bloque del que no sale una ráfaga */
#define PAGE_MASK   (SALSA64_FLASH_PAGE - 1)
#define STATUS_IDLE (SALSA64_FLASH_ST_READY | SALSA64_FLASH_ST_ERASE_OK | SALSA64_FLASH_ST_PROGRAM_OK)

void salsa64_flash_reset(struct salsa64_flash *f)
{
	memset(f, 0, sizeof *f);
	f->mode = SALSA64_FLASH_READ_ARRAY;
	f->status = STATUS_IDLE;
	memset(f->page, 0xFF, sizeof f->page);
}

static uint8_t silicon_id(unsigned i)
{
	uint32_t w = (i & 4) ? SALSA64_FLASH_MX29L1100 : SALSA64_FLASH_TYPE_ID;
	return (uint8_t)(w >> (24 - 8 * (i & 3)));
}

/* Datos en modo lectura: `len` bytes desde la dirección `off`, que cuenta
 * palabras de 16 bits, sin salir del bloque de 32 KB del primer byte. */
static void read_array(const struct salsa64_save *s, uint32_t off, uint8_t *dst, uint32_t len)
{
	uint32_t start = (off & (SALSA64_FLASH_SIZE - 1)) << 1;
	uint32_t block = start & ~(BURST - 1);
	for (uint32_t done = 0; done < len;) {
		uint32_t pos = (start + done) & (BURST - 1);
		uint32_t n = len - done < BURST - pos ? len - done : BURST - pos;
		if (block < SALSA64_FLASH_SIZE)
			memcpy(dst + done, s->data + block + pos, n);
		else
			memset(dst + done, 0, n);  /* 0x08010000–0x0801FFFF: fuera del chip */
		done += n;
	}
}

static void erase(struct salsa64_save *s, uint32_t base, uint32_t len)
{
	uint8_t ones[SALSA64_FLASH_PAGE];
	memset(ones, 0xFF, sizeof ones);
	for (uint32_t k = 0; k < len; k += sizeof ones)
		salsa64_save_store(s, base + k, ones, sizeof ones);
}

static void command(struct salsa64_save *s, uint32_t cmd)
{
	struct salsa64_flash *f = &s->flash;
	TRACE('C', f->mode, SALSA64_FLASH_CMD, cmd);
	f->commands++;
	switch (cmd >> 24) {
	case CMD_CHIP_ERASE:
		f->erase = ERASE_CHIP;
		return;
	case CMD_SECTOR_ERASE:
		f->erase = ERASE_SECTOR;
		f->erase_sector = (uint8_t)((cmd & 0x3FF) >> 7);
		return;
	case CMD_ERASE:
		if (f->erase == ERASE_NONE)
			return;
		if (f->erase == ERASE_CHIP)
			erase(s, 0, SALSA64_FLASH_SIZE);
		else
			erase(s, (uint32_t)f->erase_sector * SECTOR, SECTOR);
		f->erase = ERASE_NONE;
		f->mode = SALSA64_FLASH_STATUS;
		return;
	case CMD_PROGRAM: {
		uint32_t base = (cmd & 0x3FF) * SALSA64_FLASH_PAGE;
		uint8_t page[SALSA64_FLASH_PAGE];
		for (unsigned i = 0; i < SALSA64_FLASH_PAGE; i++)
			page[i] = s->data[base + i] & f->page[i];
		salsa64_save_store(s, base, page, sizeof page);
		memset(f->page, 0xFF, sizeof f->page);
		f->mode = SALSA64_FLASH_STATUS;
		return;
	}
	case CMD_LOAD_PAGE:
		f->mode = SALSA64_FLASH_LOAD_PAGE;
		return;
	case CMD_STATUS:
		f->mode = SALSA64_FLASH_STATUS;
		return;
	case CMD_SILICON_ID:
		f->mode = SALSA64_FLASH_SILICON_ID;
		return;
	case CMD_READ_ARRAY:
		f->mode = SALSA64_FLASH_READ_ARRAY;
		return;
	default:
		f->unknown++;
		return;
	}
}

/* Modo estado: un 0 en la dirección 0 limpia el registro (en el MX29L1100,
 * los bits OK vuelven a 1). `half` es la primera media palabra escrita. */
static void status_write(struct salsa64_flash *f, uint32_t off, uint32_t half)
{
	if (off == 0 && half == 0)
		f->status |= SALSA64_FLASH_ST_ERASE_OK | SALSA64_FLASH_ST_PROGRAM_OK;
}

uint32_t salsa64_flash_read32(const struct salsa64_save *s, uint32_t off)
{
	const struct salsa64_flash *f = &s->flash;
	off &= SALSA64_FLASH_SIZE - 1;
	TRACE('R', f->mode, off, 0);
	switch (f->mode) {
	case SALSA64_FLASH_STATUS:
		return (uint32_t)f->status << 16 | f->status;
	case SALSA64_FLASH_SILICON_ID:
		return SALSA64_FLASH_TYPE_ID;
	case SALSA64_FLASH_LOAD_PAGE:
		return salsa64_rd32(f->page + (off & PAGE_MASK & ~3u));
	default: {
		uint8_t b[4];
		read_array(s, off, b, sizeof b);
		return salsa64_rd32(b);
	}
	}
}

void salsa64_flash_write32(struct salsa64_save *s, uint32_t off, uint32_t value)
{
	struct salsa64_flash *f = &s->flash;
	off &= SALSA64_FLASH_SIZE - 1;
	if (off == SALSA64_FLASH_CMD) {
		command(s, value);
		return;
	}
	TRACE('W', f->mode, off, value);
	if (f->mode == SALSA64_FLASH_STATUS)
		status_write(f, off, value >> 16);
	else if (f->mode == SALSA64_FLASH_LOAD_PAGE)
		salsa64_wr32(f->page + (off & PAGE_MASK & ~3u), value);
}

void salsa64_flash_dma(struct salsa64_save *s, uint32_t off, uint8_t *rdram, uint32_t len, bool to_rdram)
{
	struct salsa64_flash *f = &s->flash;
	off &= SALSA64_FLASH_SIZE - 1;
	TRACE(to_rdram ? 'I' : 'O', f->mode, off, len);
	if (!to_rdram) {
		if (off == SALSA64_FLASH_CMD) {
			if (len >= 4)
				command(s, salsa64_rd32(rdram));
		} else if (f->mode == SALSA64_FLASH_STATUS) {
			if (len >= 2)
				status_write(f, off, salsa64_rd16(rdram));
		} else if (f->mode == SALSA64_FLASH_LOAD_PAGE) {
			for (uint32_t i = 0; i < len; i++)
				f->page[(off + i) & PAGE_MASK] = rdram[i];
		}
		return;
	}
	switch (f->mode) {
	case SALSA64_FLASH_STATUS:
		for (uint32_t i = 0; i < len; i++)
			rdram[i] = (i & 1) ? f->status : 0;
		return;
	case SALSA64_FLASH_SILICON_ID:
		for (uint32_t i = 0; i < len; i++)
			rdram[i] = silicon_id(i);
		return;
	case SALSA64_FLASH_LOAD_PAGE:
		for (uint32_t i = 0; i < len; i++)
			rdram[i] = f->page[(off + i) & PAGE_MASK];
		return;
	default:
		read_array(s, off, rdram, len);
		return;
	}
}
