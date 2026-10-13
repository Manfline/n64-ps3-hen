/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — trucos (códigos GameShark) por juego.
 *
 * Archivos: SALSA64_DATA_DIR/cheats/<nombre de la ROM sin extensión>.cht, en
 * el formato de RetroArch/libretro:
 *
 *     cheats = 2
 *     cheat0_desc = "Vidas infinitas"
 *     cheat0_code = "8107A6C4 0063+8107A6C6 0063"
 *     cheat0_enable = false
 *
 * Varias líneas de código van separadas por «+». Cuáles están activos se
 * recuerda en <nombre>.on (una descripción por línea), junto al .cht, sin
 * tocar el .cht del usuario.
 *
 * Códigos que se aplican (la dirección son los 24 bits bajos; 8 MB como
 * máximo):
 *   80, A0, F0 aaaaaa 00vv   escribe un byte
 *   81, A1, F1 aaaaaa vvvv   escribe 16 bits (dirección par)
 *   D0 / D1    aaaaaa vv(vv) la línea siguiente solo si el byte / los 16 bits son iguales
 *   D2 / D3    aaaaaa vv(vv) la línea siguiente solo si son distintos
 *   50 00nnss  vvvv          repite la línea siguiente nn veces, ss bytes más
 *                            allá cada vez y vvvv más en el valor
 * Los demás tipos (botones del GameShark, etc.) se cuentan en `unsupported`
 * y no se aplican. Sin dependencias de PSL1GHT: compila en cualquier host.
 */
#ifndef SALSA64_FRONTEND_CHEATS_H
#define SALSA64_FRONTEND_CHEATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHEATS_MAX       4096   /* trucos por juego (los que sobren se ignoran) */
#define CHEATS_LINES_MAX 65536  /* líneas de código en total */
#define CHEAT_DESC_MAX   64
#define CHEATS_PATH_MAX  256

struct salsa64_bus;

struct cheat_line {
	uint32_t addr;  /* palabra de dirección: tipo en el byte alto */
	uint32_t val;
};

struct cheat {
	char desc[CHEAT_DESC_MAX];
	uint16_t first, count;  /* sus líneas en cheat_set.l */
	bool on;
};

struct cheat_set {
	unsigned n;
	struct cheat c[CHEATS_MAX];
	unsigned nlines;
	struct cheat_line l[CHEATS_LINES_MAX];
	unsigned unsupported;  /* líneas que no se pueden aplicar */
	unsigned dropped;      /* trucos o líneas que no cupieron */
	bool crc_mismatch;     /* .cht de Project64: ninguna sección era del CRC de la ROM (se usó la primera) */
	char cht_path[CHEATS_PATH_MAX];  /* el .cht que se leyó (o el que se buscó) */
	char on_path[CHEATS_PATH_MAX];   /* dónde se guardan los activos */
};

/* Lee el texto de un .cht de Project64 (Config/Cheats): secciones
 * «[CRC1-CRC2-C:45]», cada truco «$Nombre\Grupo» con líneas «AAAAAAAA VVVV» y,
 * si el valor lleva «??», una línea «VV Opción» por opción (cada opción pasa a
 * ser un truco). Elige la sección del CRC de la ROM (crc1 y crc2 de la
 * cabecera; 0 si no se sabe) o, si no hay, la primera, y lo anota en
 * crc_mismatch. */
void cheats_parse_pj64(struct cheat_set *cs, const char *text, size_t len, uint32_t crc1, uint32_t crc2);

/* Lee el texto de un .cht (reemplaza lo que hubiera). Un truco sin código
 * válido no se cuenta. */
void cheats_parse(struct cheat_set *cs, const char *text, size_t len);

/* Los activos: una descripción por línea; deja encendidos los que coincidan
 * (por texto) y apaga los demás. */
void cheats_sel_parse(struct cheat_set *cs, const char *text, size_t len);
int cheats_sel_format(const struct cheat_set *cs, char *buf, size_t size);

/* Busca <dir>/<rom sin extensión>.cht (también sin distinguir mayúsculas),
 * lo lee y carga los activos. Devuelve cuántos trucos hay; 0 si no hay
 * archivo (cht_path queda con el nombre esperado). */
/* El .cht puede ser de RetroArch o de Project64 (se distingue por el texto).
 * crc1 y crc2 son los de la cabecera de la ROM (0 si no se sabe). */
unsigned cheats_load(struct cheat_set *cs, const char *dir, const char *rom_name, const char *title, const char *code,
		     uint32_t crc1, uint32_t crc2);
/* Guarda los activos en on_path. */
bool cheats_save(const struct cheat_set *cs);

unsigned cheats_active(const struct cheat_set *cs);

/* Aplica los activos a la RDRAM (una vez por campo). Si cambia memoria,
 * avisa al bus para que el recompilador descarte código que dependa de ella. */
void cheats_apply(const struct cheat_set *cs, struct salsa64_bus *bus);

#endif
