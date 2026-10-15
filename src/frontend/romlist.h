/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lista de ROMs para el menú del frontend.
 *
 * Busca archivos .z64, .v64 y .n64 (cualquier mayúscula; los tres órdenes de
 * bytes) en uno o más directorios, lee solo su cabecera (los primeros 0x40
 * bytes; nunca la ROM entera) y guarda lo que el menú muestra: título
 * interno del cartucho (0x20, 20 bytes), código de juego, región, tipo de
 * guardado detectado (core/save.h, con guardado.cfg si se da) y tamaño. La
 * lista se ordena por título o nombre de archivo (sin distinguir
 * mayúsculas); las carpetas van antes de las ROMs.
 *
 * Recuerda la última ROM abierta en un archivo de texto (su ruta completa).
 *
 * Solo usa stdio y dirent: compila igual para la PS3 (newlib de PSL1GHT) y
 * para el host (pruebas en tests/unit/test_menu.c). Sin memoria dinámica:
 * la lista es de tamaño fijo y la pone quien llama.
 */
#ifndef SALSA64_FRONTEND_ROMLIST_H
#define SALSA64_FRONTEND_ROMLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROMLIST_MAX       4096
#define ROMLIST_SOURCES_MAX 9
#define ROMLIST_TOTAL_MAX (ROMLIST_MAX * ROMLIST_SOURCES_MAX)
#define ROMLIST_DIR_MAX   256
#define ROMLIST_DIR_TOTAL_MAX (ROMLIST_DIR_MAX * ROMLIST_SOURCES_MAX)
#define ROMLIST_ENTRIES_MAX (ROMLIST_TOTAL_MAX + ROMLIST_DIR_TOTAL_MAX + 1)
#define ROMLIST_PATH_MAX  256
#define ROMLIST_TITLE_MAX 20
#define ROMLIST_MIN_SIZE  0x1000u  /* lo mismo que exige el frontend al cargar */

struct rom_entry {
	char path[ROMLIST_PATH_MAX];       /* ruta completa */
	uint16_t name;                     /* índice del nombre de archivo en path */
	char title[ROMLIST_TITLE_MAX + 1]; /* título limpio; vacío si la cabecera no trae */
	char code[5];                      /* código de juego (0x3B–0x3E); vacío si no trae */
	uint8_t format;                    /* enum salsa64_rom_format */
	uint8_t region;                    /* byte 0x3E (código de destino) */
	uint8_t version;                   /* byte 0x3F */
	uint8_t tv;                        /* enum salsa64_tv_type */
	uint8_t save;                      /* enum salsa64_save_type */
	uint32_t size;                     /* bytes del archivo */
	uint32_t crc1, crc2;               /* sumas de la cabecera (0x10 y 0x14), como las usan los .cht de Project64 */
	uint8_t kind;                      /* ROM, carpeta o subir un nivel */
	uint8_t title_supported;           /* false si hay bytes que no representa la fuente */
};

enum romlist_kind { ROMLIST_ROM, ROMLIST_DIRECTORY, ROMLIST_PARENT };
enum romlist_order { ROMLIST_ORDER_TITLE, ROMLIST_ORDER_FILENAME };

struct romlist {
	uint32_t count;
	uint32_t rom_count;
	uint32_t directory_rom_count;
	uint32_t dir_count;
	uint32_t directory_dir_count;
	uint32_t skipped;   /* con extensión de ROM pero ilegibles, chicas o sin lugar en la lista */
	char directory[ROMLIST_PATH_MAX];
	char root[ROMLIST_PATH_MAX];
	struct rom_entry e[ROMLIST_ENTRIES_MAX];
};

static inline const char *romlist_name(const struct rom_entry *e) { return e->path + e->name; }

/* ¿Termina en .z64, .v64 o .n64 (sin distinguir mayúsculas)? */
bool romlist_has_rom_ext(const char *name);

/* Llena `e` (salvo path, name y size) con los primeros 0x40 bytes de la
 * ROM tal como están en el archivo, en cualquiera de los tres órdenes.
 * `save_cfg` es el texto de guardado.cfg (puede ser NULL). */
void romlist_parse_header(const uint8_t raw[0x40], const char *save_cfg, size_t save_cfg_len, struct rom_entry *e);

/* Agrega un archivo (lee su cabecera). false si no se pudo leer, mide menos
 * de ROMLIST_MIN_SIZE o la lista está llena (cuenta en skipped). */
bool romlist_add_file(struct romlist *l, const char *path, const char *save_cfg, size_t save_cfg_len);

/* Agrega carpetas y ROMs de un directorio, sin entrar en ellas. `root`
 * define dónde deja de ofrecer la fila `..`. Devuelve cuántas ROMs agregó,
 * o -1 si el directorio no se pudo abrir. */
int romlist_scan_dir(struct romlist *l, const char *dir, const char *root, const char *save_cfg, size_t save_cfg_len);

/* Ordena las carpetas primero y las ROMs según `order`. */
void romlist_sort(struct romlist *l, enum romlist_order order);

/* Índice de la entrada con esa ruta, o -1. */
int romlist_find(const struct romlist *l, const char *path);

/* Título para mostrar: el de la cabecera si la fuente puede representarlo;
 * si no, el nombre de archivo. */
const char *romlist_display_title(const struct rom_entry *e);

/* Nombre corto del tipo de guardado ("EEPROM 4K", "SRAM", "FlashRAM"…;
 * "—" si no tiene). */
const char *romlist_save_label(uint8_t save);

/* Región por el código de destino (0x3E): "EE. UU.", "Japón", "Europa"… */
const char *romlist_region_name(uint8_t region);

/* Última ROM abierta: la ruta guardada en `file`. false si no hay. */
bool romlist_load_last(const char *file, char *path, size_t size);
/* Guarda `path` como la última ROM abierta. false si no se pudo escribir. */
bool romlist_save_last(const char *file, const char *path);

#endif
