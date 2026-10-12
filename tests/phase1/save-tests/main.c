/*
 * Salsa64 — Fase 1: guardado del cartucho en la PS3.
 *
 * Corre sin pantalla las cuatro ROMs savetest (tests/roms/src/savetest:
 * EEPROM de 4 y 16 Kbit, SRAM de 256 Kbit y FlashRAM de 1 Mbit) con el
 * núcleo y
 * frontend/savefile.c, lo mismo que usa el frontend con un juego: tipo por
 * la cabecera, un archivo por ROM en SALSA64_DATA_DIR/saves, volcado
 * diferido con el reloj del sistema (EEPROM de 4 Kbit, SRAM y FlashRAM) y
 * escritura
 * al cerrar con cambios pendientes (EEPROM de 16 Kbit: se cierra sin
 * esperar el diferido, como quien sale del juego justo después de guardar).
 *
 * Cada ROM verifica lo que dejó su arranque anterior y escribe y relee el
 * suyo. Abrir el programa dos veces prueba que el guardado sobrevive a
 * cerrar y volver a abrir: la segunda vez las cuatro dicen "persistió".
 * Imprime "RESULTADO save-tests: OK/FALLO (…)".
 */
#include <malloc.h>
#include <ppu-types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "core/boot.h"
#include "frontend/savefile.h"
#include "platform/ps3/log.h"
#include "savetest_eeprom16k_bin.h"
#include "savetest_eeprom4k_bin.h"
#include "savetest_flashram_bin.h"
#include "savetest_sram_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define SAVES_DIR     SALSA64_DATA_DIR "/saves"
#define MAX_FIELDS    300        /* cada ROM termina en uno o dos campos */
#define FLUSH_WAIT_US 10000000u  /* el volcado diferido llega a los ~2 s */

static struct salsa64_n64 n64;
static struct salsa64_save save;
static struct savefile save_file;
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));

/* Última línea "savetest: …" que imprimió la ROM. */
static char result[256];

static void isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	static char line[256];
	static uint32_t n;
	(void)ctx;
	for (uint32_t i = 0; i < len; i++) {
		if (text[i] == '\n' || n == sizeof line - 1) {
			line[n] = '\0';
			salsa64_log("rom: %s", line);
			if (strncmp(line, "savetest: ", 10) == 0)
				snprintf(result, sizeof result, "%s", line);
			n = 0;
		}
		if (text[i] != '\n')
			line[n++] = (char)text[i];
	}
}

/* Corre una ROM hasta su resultado y espera el volcado diferido, o cierra
 * enseguida si `at_exit`. Devuelve el arranque que informó la ROM, o 0 si
 * algo falló. */
static uint32_t run_rom(const char *type_name, const uint8_t *bin, uint32_t size, uint8_t *rom, bool at_exit)
{
	memcpy(rom, bin, size);
	salsa64_bus_set_rom(&n64.bus, rom, size);
	struct salsa64_boot_info info;
	if (!salsa64_boot_hle(&n64, SALSA64_BOOT_TV_AUTO, &info)) {
		salsa64_log("%s: FALLO al arrancar: %s", type_name, info.error);
		return 0;
	}
	enum salsa64_save_type type = savefile_open(&save_file, &save, &n64.bus, rom, SAVES_DIR, NULL,
						    SALSA64_SAVE_AUTO, salsa64_log);
	if (strcmp(salsa64_save_name(type), type_name) != 0) {
		salsa64_log("%s: FALLO: el tipo detectado es %s", type_name, salsa64_save_name(type));
		savefile_close(&save_file);
		return 0;
	}

	result[0] = '\0';
	uint64_t t0 = sysGetSystemTime();
	uint32_t fields = 0;
	while (!result[0] && fields < MAX_FIELDS) {
		salsa64_n64_run_field(&n64);
		fields++;
	}
	uint64_t t1 = sysGetSystemTime(), now = t1;
	bool written = salsa64_save_dirty(&save);

	/* Como el frontend: un campo y una consulta por vuelta, hasta que
	 * salsa64_save_due pida volcar. */
	while (!at_exit && salsa64_save_dirty(&save) && now - t1 < FLUSH_WAIT_US) {
		salsa64_n64_run_field(&n64);
		now = sysGetSystemTime();
		savefile_poll(&save_file, now);
	}
	bool flushed;
	if (at_exit) {
		savefile_close(&save_file);  /* debe escribir: hay cambios pendientes */
		flushed = written && !salsa64_save_dirty(&save);
		salsa64_log("%s: resultado en %u campos (%.0f ms); volcado al cerrar %s", type_name, fields,
			    (t1 - t0) / 1000.0, flushed ? "hecho" : "NO hecho");
	} else {
		flushed = written && !salsa64_save_dirty(&save);
		salsa64_log("%s: resultado en %u campos (%.0f ms); volcado diferido %s a los %.1f s", type_name,
			    fields, (t1 - t0) / 1000.0, flushed ? "hecho" : "NO llegó", (now - t1) / 1e6);
		savefile_close(&save_file);
	}

	const char *at = strstr(result, " arranque ");
	if (strncmp(result, "savetest: OK ", 13) != 0 || !at || !flushed)
		return 0;
	return (uint32_t)strtoul(at + 10, NULL, 10);
}

int main(void)
{
	static const struct {
		const char *type;
		const uint8_t *bin;
		const u32 *size;
		bool at_exit;
	} roms[] = {
		{ "eeprom4k", savetest_eeprom4k_bin, &savetest_eeprom4k_bin_size, false },
		{ "eeprom16k", savetest_eeprom16k_bin, &savetest_eeprom16k_bin_size, true },
		{ "sram256k", savetest_sram_bin, &savetest_sram_bin_size, false },
		{ "flashram", savetest_flashram_bin, &savetest_flashram_bin_size, false },
	};
	enum { NROMS = sizeof roms / sizeof roms[0] };
	salsa64_log_init("save-tests");

	uint32_t rom_max = 0;
	for (unsigned k = 0; k < NROMS; k++)
		if (*roms[k].size > rom_max)
			rom_max = *roms[k].size;
	uint8_t *rom = memalign(128, rom_max);
	uint8_t *rdram = memalign(128, SALSA64_RDRAM_MAX);
	if (!rom || !rdram) {
		salsa64_log("RESULTADO save-tests: FALLO sin memoria");
		salsa64_log_shutdown();
		return 1;
	}
	salsa64_n64_init(&n64, rdram, SALSA64_RDRAM_MAX);
	salsa64_bus_set_isviewer(&n64.bus, isv_buf, isv_out, NULL);

	uint32_t boots[NROMS];
	bool ok = true;
	for (unsigned k = 0; k < NROMS; k++) {
		boots[k] = run_rom(roms[k].type, roms[k].bin, *roms[k].size, rom, roms[k].at_exit);
		ok = ok && boots[k] != 0 && boots[k] == boots[0];
	}
	if (ok)
		salsa64_log("RESULTADO save-tests: OK (arranque %u de eeprom4k, eeprom16k, sram256k y flashram; %s)",
			    boots[0], boots[0] > 1 ? "persistió el anterior" : "abrir otra vez para ver la persistencia");
	else
		salsa64_log("RESULTADO save-tests: FALLO (arranques %u, %u, %u y %u; ver el log)", boots[0], boots[1],
			    boots[2], boots[3]);
	salsa64_log_shutdown();
	return 0;
}
