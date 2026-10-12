/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — archivos de guardado del cartucho (EEPROM, SRAM, FlashRAM) en el
 * disco.
 *
 * Un archivo por juego y tipo en el directorio que se indique (en la PS3,
 * SALSA64_DATA_DIR/saves; en salsa64-run, --save-dir), con el nombre de
 * salsa64_save_filename ("<título>-<CRC1>.eep|sra|fla"). Los datos viven en el
 * struct salsa64_save que pone quien llama (preasignado); aquí solo se leen
 * al abrir y se escriben cuando salsa64_save_due lo pide (unos segundos sin
 * escrituras del juego) y al cerrar. Nada se escribe en cada escritura del
 * juego ni se asigna memoria.
 *
 * Escritura segura: primero <archivo>.tmp completo; después se borra el
 * anterior y se renombra. Si algo corta en medio, al abrir se usa el .tmp.
 *
 * Solo usa stdio y mkdir: compila igual para la PS3 y para el host.
 *
 * Dos hilos (docs/hilos-ppe.md): el de emulación llama a savefile_stage
 * una vez por campo y, cuando toca volcar, copia los datos a la zona de
 * paso `stage` y marca `pending`; el de E/S llama a savefile_write_staged
 * y escribe esa copia al disco. Mientras está pendiente no se vuelve a
 * copiar; el que escribe nunca toca los datos vivos del cartucho. Con un
 * solo hilo, savefile_poll hace las dos cosas.
 */
#ifndef SALSA64_FRONTEND_SAVEFILE_H
#define SALSA64_FRONTEND_SAVEFILE_H

#include "core/bus.h"
#include "core/save.h"

#define SAVEFILE_PATH_MAX 256
#define SAVEFILE_RETRY_US 10000000u  /* tras un error de escritura */

typedef void (*savefile_log_fn)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct savefile {
	struct salsa64_save *save;
	struct salsa64_bus *bus;
	savefile_log_fn log;
	uint64_t retry_us;              /* no reintentar antes de esto */
	bool failed;
	char path[SAVEFILE_PATH_MAX];   /* vacío: la ROM no tiene guardado emulado */
	char tmp[SAVEFILE_PATH_MAX + 4];
	/* Zona de paso entre hilos: la llena savefile_stage, la escribe
	 * savefile_write_staged. */
	volatile uint32_t pending;      /* hay una copia sin escribir */
	uint32_t stage_len;
	uint32_t stage_writes;          /* escrituras del juego que lleva */
	uint8_t stage[SALSA64_SAVE_MAX] __attribute__((aligned(16)));
};

/*
 * Elige el tipo de guardado de la ROM y lo conecta al cartucho: `manual` (un
 * enum salsa64_save_type o SALSA64_SAVE_AUTO), si no la línea que aplique de
 * `cfg_path` (guardado.cfg; puede ser NULL o no existir), la cabecera de
 * homebrew o la tabla. Lee el archivo si existe y crea `dir` si falta (con
 * `dir` NULL el guardado queda solo en memoria). Va antes de
 * salsa64_boot_hle o después, da igual: el guardado sobrevive al reset.
 * Devuelve el tipo emulado (NONE si la ROM queda sin guardado).
 */
enum salsa64_save_type savefile_open(struct savefile *f, struct salsa64_save *save, struct salsa64_bus *bus,
				     const uint8_t *rom, const char *dir, const char *cfg_path, int manual,
				     savefile_log_fn log);
/* Una vez por campo, con un reloj en µs: escribe si toca (un solo hilo). */
void savefile_poll(struct savefile *f, uint64_t now_us);
/* Hilo de emulación, una vez por campo: si toca volcar y la zona de paso
 * está libre, copia los datos ahí y los da por volcados. */
void savefile_stage(struct savefile *f, uint64_t now_us);
/* Hilo de E/S: escribe la copia pendiente, si la hay (con reintentos). */
void savefile_write_staged(struct savefile *f, uint64_t now_us);
/* Escribe ya si hay cambios. false si no se pudo. */
bool savefile_flush(struct savefile *f);
/* Al terminar con la ROM: escribe lo pendiente y desconecta el guardado. */
void savefile_close(struct savefile *f);

#endif
