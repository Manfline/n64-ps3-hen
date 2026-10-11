/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — memoria de guardado del cartucho. Ver save.h.
 *
 * Escrito desde n64brew.dev (Joybus Protocol, Peripheral Interface, Memory
 * map y ROM Header, consultadas el 2026-10-10); no deriva de código de
 * otros emuladores.
 */
#include "core/save.h"

#include <stdio.h>
#include <string.h>

#include "core/bus.h"
#include "core/endian.h"

#define EEP_INFO     0x00
#define EEP_READ     0x04
#define EEP_WRITE    0x05
#define EEP_RESET    0xFF
#define EEP_BLOCK    8
#define EEP_ID_4K    0x80  /* identificador 0x0080 */
#define EEP_ID_16K   0xC0  /* identificador 0x00C0 */

/* Encabezado de la ROM (n64brew.dev: ROM Header). */
#define HDR_CRC1     0x10
#define HDR_TITLE    0x20
#define HDR_TITLE_LEN 20
#define HDR_CODE     0x3B  /* categoría, código único (2) y destino */
#define HDR_VERSION  0x3F

static const struct {
	const char *name;
	const char *ext;
	uint32_t size;  /* 0: sin emular */
} types[SALSA64_SAVE_NTYPES] = {
	[SALSA64_SAVE_NONE]       = { "ninguno", NULL, 0 },
	[SALSA64_SAVE_EEPROM_4K]  = { "eeprom4k", "eep", 512 },
	[SALSA64_SAVE_EEPROM_16K] = { "eeprom16k", "eep", 2048 },
	[SALSA64_SAVE_SRAM_256K]  = { "sram256k", "sra", 32768 },
	[SALSA64_SAVE_SRAM_768K]  = { "sram768k", "sra", 0 },
	[SALSA64_SAVE_FLASHRAM]   = { "flashram", "fla", 0 },
	[SALSA64_SAVE_SRAM_1M]    = { "sram1m", "sra", 0 },
};

bool salsa64_save_emulated(enum salsa64_save_type type)
{
	return (unsigned)type < SALSA64_SAVE_NTYPES && types[type].size != 0;
}

const char *salsa64_save_name(enum salsa64_save_type type)
{
	return (unsigned)type < SALSA64_SAVE_NTYPES ? types[type].name : "?";
}

const char *salsa64_save_source_name(enum salsa64_save_source src)
{
	switch (src) {
	case SALSA64_SAVE_FROM_CONFIG: return "ajuste manual";
	case SALSA64_SAVE_FROM_HEADER: return "cabecera de homebrew";
	case SALSA64_SAVE_FROM_TABLE:  return "tabla por código de juego";
	default:                       return "sin indicación";
	}
}

void salsa64_save_init(struct salsa64_save *s, enum salsa64_save_type type)
{
	s->type = salsa64_save_emulated(type) ? type : SALSA64_SAVE_NONE;
	s->size = types[s->type].size;
	s->writes = s->flushed = s->seen = 0;
	s->seen_us = s->dirty_us = 0;
	memset(s->data, 0xFF, sizeof s->data);
}

bool salsa64_save_load(struct salsa64_save *s, const uint8_t *data, uint32_t len)
{
	memcpy(s->data, data, len < s->size ? len : s->size);
	return len == s->size;
}

void salsa64_save_attach(struct salsa64_bus *bus, struct salsa64_save *s)
{
	bus->save = s;
}

/* Escritura del juego: solo cuenta si cambia algo. */
static void store(struct salsa64_save *s, uint32_t off, const uint8_t *src, uint32_t len)
{
	if (memcmp(s->data + off, src, len) == 0)
		return;
	memcpy(s->data + off, src, len);
	s->writes++;
}

/* ---- EEPROM (canal 4 del PIF) ------------------------------------------- */

static bool is_eeprom(const struct salsa64_save *s)
{
	return s->type == SALSA64_SAVE_EEPROM_4K || s->type == SALSA64_SAVE_EEPROM_16K;
}

int salsa64_save_joybus(struct salsa64_save *s, const uint8_t *tx, unsigned txlen, uint8_t *reply)
{
	if (!is_eeprom(s) || txlen == 0)
		return -1;
	switch (tx[0]) {
	case EEP_INFO:
	case EEP_RESET:
		reply[0] = 0x00;
		reply[1] = s->type == SALSA64_SAVE_EEPROM_4K ? EEP_ID_4K : EEP_ID_16K;
		reply[2] = 0x00;  /* sin escritura en curso */
		return 3;
	case EEP_READ:
		if (txlen < 2)
			return -1;
		/* El tamaño es potencia de 2: la de 4 Kbit repite los bloques. */
		memcpy(reply, s->data + ((tx[1] * EEP_BLOCK) & (s->size - 1)), EEP_BLOCK);
		return EEP_BLOCK;
	case EEP_WRITE:
		if (txlen < 2 + EEP_BLOCK)
			return -1;
		store(s, (tx[1] * EEP_BLOCK) & (s->size - 1), tx + 2, EEP_BLOCK);
		reply[0] = 0x00;  /* no ocupada */
		return 1;
	default:
		return -1;
	}
}

/* ---- SRAM (dominio 2 del PI) --------------------------------------------- */

static struct salsa64_save *sram(const struct salsa64_bus *bus)
{
	struct salsa64_save *s = bus->save;
	return s && s->type == SALSA64_SAVE_SRAM_256K ? s : NULL;
}

bool salsa64_save_pi_dma(struct salsa64_bus *bus, bool to_rdram)
{
	struct salsa64_save *s = sram(bus);
	uint32_t cart = bus->pi[SALSA64_PI_CART_ADDR] & 0xFFFFFFFE;
	if (!s || cart - SALSA64_SRAM_ADDR >= SALSA64_SRAM_SPAN)
		return false;
	uint32_t dram = bus->pi[SALSA64_PI_DRAM_ADDR] & 0x00FFFFFE;
	uint32_t len = (bus->pi[to_rdram ? SALSA64_PI_WR_LEN : SALSA64_PI_RD_LEN] & 0x00FFFFFF) + 1;

	/* Lo que caiga fuera de la RDRAM se descarta. */
	for (uint32_t done = 0; done < len;) {
		uint32_t d = dram + done;
		if (d >= bus->rdram_size)
			break;
		uint32_t off = (cart - SALSA64_SRAM_ADDR + done) & (s->size - 1);
		uint32_t chunk = len - done;
		if (chunk > bus->rdram_size - d)
			chunk = bus->rdram_size - d;
		if (chunk > s->size - off)
			chunk = s->size - off;
		if (to_rdram)
			memcpy(bus->rdram + d, s->data + off, chunk);
		else
			store(s, off, bus->rdram + d, chunk);
		done += chunk;
	}
	if (to_rdram && dram < bus->rdram_size)
		salsa64_bus_rdram_written(bus, dram, len);  /* código recompilado ahí: descartarlo */

	/* Igual que el DMA de la ROM (pi.c). */
	bus->pi[SALSA64_PI_DRAM_ADDR] = (dram + len + 7) & 0x00FFFFF8;
	bus->pi[SALSA64_PI_CART_ADDR] = (cart + len + 1) & 0xFFFFFFFE;
	salsa64_mi_raise(bus, SALSA64_IRQ_PI);
	return true;
}

uint32_t salsa64_save_sram_read32(const struct salsa64_bus *bus, uint32_t paddr)
{
	const struct salsa64_save *s = sram(bus);
	if (!s)
		return 0;
	return salsa64_rd32(s->data + ((paddr - SALSA64_SRAM_ADDR) & (s->size - 4)));
}

void salsa64_save_sram_write32(struct salsa64_bus *bus, uint32_t paddr, uint32_t value)
{
	struct salsa64_save *s = sram(bus);
	if (!s)
		return;
	uint8_t w[4];
	salsa64_wr32(w, value);
	store(s, (paddr - SALSA64_SRAM_ADDR) & (s->size - 4), w, sizeof w);
}

/* ---- Volcado diferido ------------------------------------------------------ */

bool salsa64_save_dirty(const struct salsa64_save *s)
{
	return s->writes != s->flushed;
}

bool salsa64_save_due(struct salsa64_save *s, uint64_t now_us)
{
	if (s->writes != s->seen) {
		if (s->seen == s->flushed)
			s->dirty_us = now_us;
		s->seen = s->writes;
		s->seen_us = now_us;
	}
	if (!salsa64_save_dirty(s))
		return false;
	return now_us - s->seen_us >= SALSA64_SAVE_QUIET_US ||
	       now_us - s->dirty_us >= SALSA64_SAVE_MAX_DELAY_US;
}

void salsa64_save_flushed(struct salsa64_save *s)
{
	s->flushed = s->seen = s->writes;
}

/* ---- Tipo de cada ROM ------------------------------------------------------ */

bool salsa64_save_from_header(const uint8_t *rom, enum salsa64_save_type *type)
{
	if (rom[0x3C] != 'E' || rom[0x3D] != 'D')
		return false;
	/* Bits 4–7 de 0x3F (n64brew.dev: ROM Header, "Advanced Homebrew ROM
	 * Header"); los bits 0–1 (RTC, sin región) no importan aquí. */
	static const enum salsa64_save_type nibble[] = {
		SALSA64_SAVE_NONE, SALSA64_SAVE_EEPROM_4K, SALSA64_SAVE_EEPROM_16K,
		SALSA64_SAVE_SRAM_256K, SALSA64_SAVE_SRAM_768K, SALSA64_SAVE_FLASHRAM,
		SALSA64_SAVE_SRAM_1M,
	};
	unsigned n = rom[0x3F] >> 4;
	*type = n < sizeof nibble / sizeof nibble[0] ? nibble[n] : SALSA64_SAVE_NONE;
	return true;
}

enum salsa64_save_type salsa64_save_detect(const uint8_t *rom, int manual, enum salsa64_save_source *src)
{
	enum salsa64_save_type type;
	if (manual >= 0 && manual < SALSA64_SAVE_NTYPES) {
		*src = SALSA64_SAVE_FROM_CONFIG;
		return (enum salsa64_save_type)manual;
	}
	if (salsa64_save_from_header(rom, &type)) {
		*src = SALSA64_SAVE_FROM_HEADER;
		return type;
	}
	if (salsa64_save_from_table(rom, &type)) {
		*src = SALSA64_SAVE_FROM_TABLE;
		return type;
	}
	*src = SALSA64_SAVE_FROM_NONE;
	return SALSA64_SAVE_NONE;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

static bool same(const char *a, size_t n, const char *b)
{
	size_t k = 0;
	for (; k < n && b[k]; k++)
		if (lower((unsigned char)a[k]) != lower((unsigned char)b[k]))
			return false;
	return k == n && !b[k];
}

static void trim(const char **s, size_t *n)
{
	while (*n && (**s == ' ' || **s == '\t' || **s == '\r')) {
		(*s)++;
		(*n)--;
	}
	while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' || (*s)[*n - 1] == '\r'))
		(*n)--;
}

static int hexval(int c)
{
	c = lower(c);
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Especificidad con que la clave describe a la ROM: 0 no aplica, -1 clave
 * inválida; si no, 1 (*), 2 (3 caracteres), 3 (4) o 4 (CRC1). */
static int key_match(const char *key, size_t n, const uint8_t *rom)
{
	if (n == 1 && key[0] == '*')
		return 1;
	if (n == 3 || n == 4) {
		for (size_t k = 0; k < n; k++)
			if (lower((unsigned char)key[k]) != lower(rom[HDR_CODE + k]))
				return 0;
		return (int)n - 1;
	}
	if (n == 8) {
		uint32_t crc = 0;
		for (size_t k = 0; k < n; k++) {
			int h = hexval((unsigned char)key[k]);
			if (h < 0)
				return -1;
			crc = crc << 4 | (uint32_t)h;
		}
		return crc == salsa64_rd32(rom + HDR_CRC1) ? 4 : 0;
	}
	return -1;
}

int salsa64_save_parse_config(const char *text, size_t len, const uint8_t *rom, int *bad,
			      char *err, size_t err_size)
{
	int best = 0, result = SALSA64_SAVE_AUTO;
	unsigned lineno = 0;
	size_t i = 0;
	*bad = 0;
	if (err && err_size)
		err[0] = '\0';
	while (i < len) {
		const char *line = text + i;
		const char *nl = memchr(line, '\n', len - i);
		size_t n = nl ? (size_t)(nl - line) : len - i;
		i += n + 1;
		lineno++;
		const char *hash = memchr(line, '#', n);
		if (hash)
			n = (size_t)(hash - line);
		trim(&line, &n);
		if (n == 0)
			continue;

		const char *eq = memchr(line, '=', n);
		int match = -1, type = -1;
		if (eq) {
			const char *key = line, *val = eq + 1;
			size_t kn = (size_t)(eq - line), vn = n - kn - 1;
			trim(&key, &kn);
			trim(&val, &vn);
			match = key_match(key, kn, rom);
			for (int t = 0; t < SALSA64_SAVE_NTYPES; t++)
				if (same(val, vn, types[t].name))
					type = t;
			if (same(val, vn, "sram"))
				type = SALSA64_SAVE_SRAM_256K;
		}
		if (match < 0 || type < 0) {
			if (!*bad && err && err_size)
				snprintf(err, err_size, "línea %u: \"%.*s\"", lineno, (int)(n > 40 ? 40 : n), line);
			(*bad)++;
			continue;
		}
		if (match > best) {
			best = match;
			result = type;
		}
	}
	return result;
}

void salsa64_save_filename(const uint8_t *rom, enum salsa64_save_type type, char *buf, size_t size)
{
	char title[HDR_TITLE_LEN + 1];
	size_t n = 0;
	for (unsigned k = 0; k < HDR_TITLE_LEN; k++) {
		int c = rom[HDR_TITLE + k];
		bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
		if (alnum)
			title[n++] = (char)c;
		else if (n && title[n - 1] != '_')
			title[n++] = '_';
	}
	while (n && title[n - 1] == '_')
		n--;
	title[n] = '\0';
	const char *ext = (unsigned)type < SALSA64_SAVE_NTYPES && types[type].ext ? types[type].ext : "sav";
	snprintf(buf, size, "%s-%08X.%s", n ? title : "rom", (unsigned)salsa64_rd32(rom + HDR_CRC1), ext);
}
