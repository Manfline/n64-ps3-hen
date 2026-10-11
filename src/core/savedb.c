/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tipo de guardado por código de juego. Ver save.h.
 *
 * Solo datos de compatibilidad: el código del encabezado (0x3B–0x3D:
 * categoría y código único; con región en 0x3E y versión en 0x3F para las
 * excepciones) y el tipo de memoria de guardado del cartucho. Nada de
 * material de Nintendo.
 *
 * Origen (docs/THIRD_PARTY.md): la lista se cotejó el 2026-10-10 entre dos
 * fuentes independientes, la heurística de mia en ares
 * (mia/medium/nintendo-64.cpp, ISC) y la base de N64FlashcartMenu
 * (src/menu/rom_info.c, AGPL-3.0); solo entran los códigos en que las dos
 * dicen lo mismo. No se copió código de ninguna. Quedan fuera los
 * desacuerdos (NK4 fuera de las versiones japonesas 1.0 y 1.1, NO2, NDD):
 * esos juegos se configuran a mano en guardado.cfg.
 */
#include "core/save.h"

#include <string.h>

/* Excepciones por región y versión: se consultan antes que las listas. Los
 * mismos juegos en otras regiones guardan en el Controller Pak (o no se
 * sabe) y no aparecen en las listas. */
static const struct {
	char code[4];         /* 0x3B–0x3E */
	uint8_t max_version;  /* versión más alta a la que aplica */
	uint8_t type;
} special[] = {
	{ { 'N', 'K', '4', 'J' }, 1, SALSA64_SAVE_SRAM_256K },     /* versiones 1.0 y 1.1 */
	{ { 'N', '3', 'H', 'J' }, 0xFF, SALSA64_SAVE_SRAM_256K },
	{ { 'N', 'D', '3', 'J' }, 0xFF, SALSA64_SAVE_EEPROM_16K },
	{ { 'N', 'D', '4', 'J' }, 0xFF, SALSA64_SAVE_EEPROM_16K },
	{ { 'N', 'D', 'K', 'J' }, 0xFF, SALSA64_SAVE_EEPROM_4K },
	{ { 'N', 'W', 'T', 'J' }, 0xFF, SALSA64_SAVE_EEPROM_4K },
};

/* Códigos de 3 caracteres (todas las regiones), en orden alfabético. */
static const char eeprom4k[] =
	"CLB" "NAB" "NAD" "NAG" "NB6" "NBC" "NBD" "NBH" "NBK" "NBM" "NBN" "NBV"
	"NCG" "NCH" "NCR" "NCT" "NCU" "NCX" "NDQ" "NDR" "NDU" "NDY" "NEA" "NER"
	"NF2" "NFG" "NFH" "NFW" "NFX" "NFY" "NGE" "NGL" "NGU" "NGV" "NHA" "NHF"
	"NHP" "NIC" "NIJ" "NIR" "NJK" "NJM" "NK2" "NKA" "NKI" "NKT" "NLB" "NLL"
	"NLR" "NMG" "NMI" "NML" "NMO" "NMR" "NMS" "NMU" "NMW" "NMZ" "NN6" "NNA"
	"NOS" "NP2" "NPG" "NPT" "NPW" "NPY" "NRA" "NRC" "NRS" "NS3" "NS6" "NSA"
	"NSC" "NSM" "NSN" "NSS" "NSU" "NSV" "NSW" "NT6" "NTB" "NTC" "NTJ" "NTM"
	"NTN" "NTP" "NTR" "NTW" "NTX" "NVL" "NVY" "NWC" "NWQ" "NWR" "NWU" "NXO"
	"NYK";

static const char eeprom16k[] =
	"N3D" "NB7" "NCW" "NCZ" "ND2" "ND6" "NDO" "NEP" "NEV" "NFU" "NGC" "NGT"
	"NIM" "NM8" "NMV" "NMX" "NNB" "NPD" "NPP" "NR7" "NRZ" "NUB" "NYS";

static const char sram256k[] =
	"CFZ" "CPS" "CZL" "NA2" "NAL" "NB5" "NFZ" "NG6" "NGP" "NHY" "NIB" "NJ5"
	"NJG" "NKG" "NMF" "NOB" "NP4" "NP6" "NPA" "NPE" "NPM" "NPS" "NRE" "NRI"
	"NS4" "NSI" "NT3" "NTE" "NUM" "NUT" "NVB" "NVP" "NW2" "NWL" "NWX" "NYW"
	"NZL";

static const char sram768k[] = "CDZ";

static const char flashram[] =
	"CP2" "NAF" "NCC" "NCK" "NCV" "NDA" "NDP" "NJF" "NKJ" "NM6" "NMQ" "NP3"
	"NPF" "NPN" "NPO" "NRH" "NSQ" "NT9" "NW4" "NZS";

static bool listed(const char *list, size_t len, const uint8_t *code)
{
	for (size_t i = 0; i + 3 <= len; i += 3)
		if (memcmp(list + i, code, 3) == 0)
			return true;
	return false;
}

bool salsa64_save_from_table(const uint8_t *rom, enum salsa64_save_type *type)
{
	static const struct {
		const char *list;
		size_t len;
		enum salsa64_save_type type;
	} lists[] = {
		{ eeprom4k, sizeof eeprom4k - 1, SALSA64_SAVE_EEPROM_4K },
		{ eeprom16k, sizeof eeprom16k - 1, SALSA64_SAVE_EEPROM_16K },
		{ sram256k, sizeof sram256k - 1, SALSA64_SAVE_SRAM_256K },
		{ sram768k, sizeof sram768k - 1, SALSA64_SAVE_SRAM_768K },
		{ flashram, sizeof flashram - 1, SALSA64_SAVE_FLASHRAM },
	};
	const uint8_t *code = rom + 0x3B;
	uint8_t version = rom[0x3F];

	for (size_t k = 0; k < sizeof special / sizeof special[0]; k++) {
		if (memcmp(special[k].code, code, 4) == 0 && version <= special[k].max_version) {
			*type = (enum salsa64_save_type)special[k].type;
			return true;
		}
	}
	for (size_t k = 0; k < sizeof lists / sizeof lists[0]; k++) {
		if (listed(lists[k].list, lists[k].len, code)) {
			*type = lists[k].type;
			return true;
		}
	}
	return false;
}
