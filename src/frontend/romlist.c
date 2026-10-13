/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lista de ROMs para el menú del frontend. Ver romlist.h.
 */
#include "romlist.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rom.h"
#include "core/save.h"

#define HDR_TITLE   0x20
#define HDR_CODE    0x3B
#define HDR_REGION  0x3E
#define HDR_VERSION 0x3F

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

static int casecmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int d = lower((unsigned char)*a) - lower((unsigned char)*b);
		if (d || !*a)
			return d;
	}
}

bool romlist_has_rom_ext(const char *name)
{
	size_t n = strlen(name);
	if (n < 5 || name[n - 4] != '.')
		return false;
	const char *x = name + n - 3;
	return (lower(x[0]) == 'z' || lower(x[0]) == 'v' || lower(x[0]) == 'n') && x[1] == '6' && x[2] == '4';
}

void romlist_parse_header(const uint8_t raw[0x40], const char *save_cfg, size_t save_cfg_len, struct rom_entry *e)
{
	uint8_t h[SALSA64_ROM_HEADER_SIZE];
	memcpy(h, raw, sizeof h);
	e->format = (uint8_t)salsa64_rom_normalize(h, sizeof h);

	/* Título: ASCII imprimible; los ceros cuentan como espacio y lo demás
	 * (katakana JIS X 0201 de los títulos japoneses) como '?'. Sin
	 * espacios a los lados. */
	char t[ROMLIST_TITLE_MAX + 1];
	for (int i = 0; i < ROMLIST_TITLE_MAX; i++) {
		uint8_t c = h[HDR_TITLE + i];
		t[i] = c == 0 ? ' ' : c >= 0x20 && c < 0x7F ? (char)c : '?';
	}
	int a = 0, b = ROMLIST_TITLE_MAX;
	while (a < b && t[a] == ' ')
		a++;
	while (b > a && t[b - 1] == ' ')
		b--;
	memcpy(e->title, t + a, (size_t)(b - a));
	e->title[b - a] = '\0';

	/* Código de juego: vacío si la cabecera no trae (homebrew con ceros). */
	bool any = false;
	for (int i = 0; i < 4; i++) {
		uint8_t c = h[HDR_CODE + i];
		e->code[i] = c > 0x20 && c < 0x7F ? (char)c : '?';
		any |= c != 0 && c != ' ';
	}
	e->code[any ? 4 : 0] = '\0';
	e->crc1 = (uint32_t)h[0x10] << 24 | (uint32_t)h[0x11] << 16 | (uint32_t)h[0x12] << 8 | h[0x13];
	e->crc2 = (uint32_t)h[0x14] << 24 | (uint32_t)h[0x15] << 16 | (uint32_t)h[0x16] << 8 | h[0x17];
	e->region = h[HDR_REGION];
	e->version = h[HDR_VERSION];
	e->tv = (uint8_t)salsa64_rom_tv_type(h);

	int manual = SALSA64_SAVE_AUTO;
	if (save_cfg && save_cfg_len) {
		int bad;
		manual = salsa64_save_parse_config(save_cfg, save_cfg_len, h, &bad, NULL, 0);
	}
	enum salsa64_save_source src;
	e->save = (uint8_t)salsa64_save_detect(h, manual, &src);
}

bool romlist_add_file(struct romlist *l, const char *path, const char *save_cfg, size_t save_cfg_len)
{
	size_t plen = strlen(path);
	if (l->count >= ROMLIST_MAX || plen >= ROMLIST_PATH_MAX) {
		l->skipped++;
		return false;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		l->skipped++;
		return false;
	}
	uint8_t raw[SALSA64_ROM_HEADER_SIZE];
	long len = -1;
	bool ok = fread(raw, 1, sizeof raw, f) == sizeof raw && fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 &&
		  (unsigned long)len >= ROMLIST_MIN_SIZE && (unsigned long)len <= 0xFFFFFFFFul;
	fclose(f);
	if (!ok) {
		l->skipped++;
		return false;
	}
	struct rom_entry *e = &l->e[l->count++];
	memset(e, 0, sizeof *e);
	memcpy(e->path, path, plen + 1);
	const char *slash = strrchr(path, '/');
	e->name = (uint16_t)(slash ? slash + 1 - path : 0);
	e->size = (uint32_t)len;
	romlist_parse_header(raw, save_cfg, save_cfg_len, e);
	return true;
}

int romlist_scan_dir(struct romlist *l, const char *dir, const char *save_cfg, size_t save_cfg_len)
{
	DIR *d = opendir(dir);
	if (!d)
		return -1;
	int added = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		/* Ocultos fuera: "._oot.z64" es lo que deja macOS en una memoria FAT. */
		if (name[0] == '.' || !romlist_has_rom_ext(name))
			continue;
		char path[ROMLIST_PATH_MAX];
		int n = snprintf(path, sizeof path, "%s/%s", dir, name);
		if (n < 0 || (size_t)n >= sizeof path) {
			l->skipped++;
			continue;
		}
		if (romlist_add_file(l, path, save_cfg, save_cfg_len))
			added++;
	}
	closedir(d);
	return added;
}

const char *romlist_display_title(const struct rom_entry *e)
{
	return e->title[0] ? e->title : romlist_name(e);
}

static int entry_cmp(const void *pa, const void *pb)
{
	const struct rom_entry *a = pa, *b = pb;
	int d = casecmp(romlist_display_title(a), romlist_display_title(b));
	if (!d)
		d = casecmp(romlist_name(a), romlist_name(b));
	return d ? d : strcmp(a->path, b->path);
}

void romlist_sort(struct romlist *l)
{
	qsort(l->e, l->count, sizeof l->e[0], entry_cmp);
}

int romlist_find(const struct romlist *l, const char *path)
{
	for (uint32_t i = 0; i < l->count; i++)
		if (!strcmp(l->e[i].path, path))
			return (int)i;
	return -1;
}

const char *romlist_save_label(uint8_t save)
{
	switch (save) {
	case SALSA64_SAVE_EEPROM_4K:  return "EEPROM 4K";
	case SALSA64_SAVE_EEPROM_16K: return "EEPROM 16K";
	case SALSA64_SAVE_SRAM_256K:  return "SRAM";
	case SALSA64_SAVE_SRAM_768K:  return "SRAM 768K";
	case SALSA64_SAVE_FLASHRAM:   return "FlashRAM";
	case SALSA64_SAVE_SRAM_1M:    return "SRAM 1M";
	default:                      return "—";
	}
}

const char *romlist_region_name(uint8_t region)
{
	/* n64brew.dev, ROM Header: código de destino. */
	switch (region) {
	case 'A': return "Asia";
	case 'B': return "Brasil";
	case 'C': return "China";
	case 'D': return "Alemania";
	case 'E': return "EE. UU.";
	case 'F': return "Francia";
	case 'G': return "Gateway";
	case 'H': return "Países Bajos";
	case 'I': return "Italia";
	case 'J': return "Japón";
	case 'K': return "Corea";
	case 'L': return "Gateway";
	case 'N': return "Canadá";
	case 'P': case 'X': case 'Y': case 'Z': return "Europa";
	case 'S': return "España";
	case 'U': return "Australia";
	case 'W': return "Escandinavia";
	default:  return "?";
	}
}

bool romlist_load_last(const char *file, char *path, size_t size)
{
	FILE *f = fopen(file, "r");
	if (!f)
		return false;
	bool ok = fgets(path, (int)size, f) != NULL;
	fclose(f);
	if (!ok)
		return false;
	path[strcspn(path, "\r\n")] = '\0';
	return path[0] != '\0';
}

bool romlist_save_last(const char *file, const char *path)
{
	FILE *f = fopen(file, "w");
	if (!f)
		return false;
	bool ok = fprintf(f, "%s\n", path) > 0;
	return fclose(f) == 0 && ok;
}
