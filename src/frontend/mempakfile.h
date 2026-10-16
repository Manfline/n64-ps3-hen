/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — archivo del Controller Pak en el disco.
 *
 * Un archivo por juego, "<título>-<CRC1>.mpk" (el nombre de
 * salsa64_save_basename), junto a los guardados del cartucho: 4 × 32 KB, el
 * control 1 en 0x0000, el 2 en 0x8000, el 3 en 0x10000 y el 4 en 0x18000
 * (el mismo orden que el .mpk de mupen64plus, para poder importar
 * partidas). Los datos viven en el struct salsa64_mempak que pone quien
 * llama (preasignado). Se escribe unos segundos después de la última
 * escritura del juego y al cerrar; nada se escribe en cada comando ni se
 * asigna memoria. Escritura segura y reparto entre hilos como savefile.h:
 * .tmp completo y rename; el hilo de emulación copia a la zona de paso
 * (mempakfile_stage) y el de E/S escribe esa copia (mempakfile_write_staged).
 */
#ifndef SALSA64_FRONTEND_MEMPAKFILE_H
#define SALSA64_FRONTEND_MEMPAKFILE_H

#include "core/bus.h"
#include "core/mempak.h"
#include "savefile.h"

struct mempakfile {
	struct salsa64_mempak *mp;
	struct salsa64_bus *bus;
	savefile_log_fn log;
	uint64_t retry_us;
	bool failed;
	char path[SAVEFILE_PATH_MAX];  /* vacío: solo en memoria */
	char tmp[SAVEFILE_PATH_MAX + 4];
	volatile uint32_t pending;
	uint32_t stage_writes;
	/* Resumen del tráfico del accesorio en el log (mempakfile_stage): cuántos
	 * comandos llevaba la última vez, cuántos hay ahora y desde cuándo. */
	uint32_t logged_total, seen_total;
	uint64_t seen_us;
	uint8_t stage[SALSA64_MEMPAK_TOTAL] __attribute__((aligned(16)));
};

/*
 * Conecta un Controller Pak a cada control de `ports` (máscara de bits, el
 * bit 0 es el control 1) y lee el archivo si existe; con `ports` en 0 no
 * conecta nada y no toca el disco. Crea `dir` si falta (con `dir` NULL el pak
 * queda solo en memoria).
 */
void mempakfile_open(struct mempakfile *f, struct salsa64_mempak *mp, struct salsa64_bus *bus,
		     const uint8_t *rom, const char *dir, unsigned ports, savefile_log_fn log);
void mempakfile_poll(struct mempakfile *f, uint64_t now_us);
void mempakfile_stage(struct mempakfile *f, uint64_t now_us);
void mempakfile_write_staged(struct mempakfile *f, uint64_t now_us);
bool mempakfile_flush(struct mempakfile *f);
/* Escribe lo pendiente y desconecta el pak. */
void mempakfile_close(struct mempakfile *f);

#endif
