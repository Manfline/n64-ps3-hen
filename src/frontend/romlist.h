/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Lista compacta de las entradas de una carpeta visible en el menú. */
#ifndef SALSA64_FRONTEND_ROMLIST_H
#define SALSA64_FRONTEND_ROMLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROMLIST_MAX 4096u
#define ROMLIST_SOURCES_MAX 9u
#define ROMLIST_PATH_MAX 256u
#define ROMLIST_NAME_MAX 255u
#define ROMLIST_STRING_POOL_MAX (ROMLIST_MAX * (ROMLIST_NAME_MAX + 1u))
#define ROMLIST_TITLE_MAX 20u
#define ROMLIST_MIN_SIZE 0x1000u
#define ROMLIST_LIST_BYTES (sizeof(struct romlist) + ROMLIST_MAX * sizeof(struct rom_entry) + ROMLIST_STRING_POOL_MAX)

struct rom_entry {
	const char *name;
	uint32_t size, crc1, crc2;
	char title[ROMLIST_TITLE_MAX + 1];
	char code[5];
	uint8_t kind, title_supported, source, format, region, version, tv, save;
};

enum romlist_kind { ROMLIST_ROM, ROMLIST_DIRECTORY, ROMLIST_PARENT };
enum romlist_order { ROMLIST_ORDER_TITLE, ROMLIST_ORDER_FILENAME };

struct romlist {
	uint32_t count, rom_count, dir_count, skipped;
	char directory[ROMLIST_PATH_MAX]; /* ruta relativa común a todas las unidades */
	uint32_t strings_used;
	struct rom_entry e[];
};

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(struct rom_entry) == 52, "tamaño previsto de cada fila compacta");
_Static_assert(sizeof(struct romlist) == 276, "tamaño previsto de la cabecera de lista");
#endif
_Static_assert(ROMLIST_LIST_BYTES <= 1572864u, "la lista debe caber en 1,5 MiB");

struct romlist *romlist_create(void);
void romlist_free(struct romlist *l);
size_t romlist_memory_bytes(void);
const char *romlist_name(const struct rom_entry *e);
const char *romlist_display_title(const struct rom_entry *e);

bool romlist_has_rom_ext(const char *name);
void romlist_parse_header(const uint8_t raw[0x40], const char *save_cfg, size_t save_cfg_len, struct rom_entry *e);
bool romlist_add_file(struct romlist *l, const char *path, unsigned source, const char *save_cfg, size_t save_cfg_len);
/* Caché persistente de cabeceras (ver romlist.c). `path` NULL la apaga. */
void romlist_cache_file(const char *path);
void romlist_cache_release(void); /* suelta la memoria de la caché (se relee del archivo al volver) */
#define ROMLIST_SCAN_REUSE 1u /* una carpeta sin cambios de fecha sale de la caché sin leerla */
int romlist_scan_sources_ex(struct romlist *l, const char *const *roots, unsigned root_count, const char *relative,
		 const char *save_cfg, size_t save_cfg_len, unsigned flags);
/* De la última lectura: filas de la caché, cabeceras leídas del disco y carpetas resueltas sin leerlas. */
void romlist_scan_stats(uint32_t *hits, uint32_t *reads, uint32_t *fast_dirs);
int romlist_scan_dir(struct romlist *l, const char *dir, const char *root, const char *save_cfg, size_t save_cfg_len);
int romlist_scan_sources(struct romlist *l, const char *const *roots, unsigned root_count, const char *relative,
		 const char *save_cfg, size_t save_cfg_len);
bool romlist_build_path(const struct romlist *l, const struct rom_entry *e, const char *const *roots,
		unsigned root_count, char *path, size_t path_size);
void romlist_sort(struct romlist *l, enum romlist_order order);
int romlist_find(const struct romlist *l, const char *path, const char *const *roots, unsigned root_count);
const char *romlist_save_label(uint8_t save);
const char *romlist_region_name(uint8_t region);
bool romlist_load_last(const char *file, char *path, size_t size);
bool romlist_save_last(const char *file, const char *path);

#endif
