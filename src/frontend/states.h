/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — estados guardados en la PS3: los archivos de las ranuras y la
 * miniatura (el formato y lo que entra en él: core/state.h).
 *
 * Archivos: SALSA64_DATA_DIR/states/<título>-<CRC1>.st0 … .st9, el mismo
 * nombre base que las partidas de saves/. Se escriben en un .tmp y se
 * renombran (un corte a mitad no pisa el estado anterior). La E/S va con
 * las llamadas al kernel (sysLv2Fs*, <sys/file.h>): la RDRAM (8 MB) sale y
 * entra de su búfer sin copias. No con sysFs* de libsysfs: esos son puentes
 * al módulo cellFs del sistema, que hay que cargar antes; en RPCS3 andan
 * igual, pero en la PS3 real colgaban la consola entera al primer mkdir.
 *
 * Todo con la emulación quieta (main.c: el hilo de emulación espera en el
 * límite del campo con la tarea del RSP recogida) y desde el hilo de
 * presentación.
 */
#ifndef SALSA64_FRONTEND_STATES_H
#define SALSA64_FRONTEND_STATES_H

#include <rsxkit/rsxkit.h>

#include "core/state.h"
#include "platform/ps3/paths.h"

#define STATES_DIR SALSA64_DATA_DIR "/states"

/* Tiempos de la última operación (µs) y su tamaño. */
struct states_timing {
	uint64_t us_total;  /* todo, desde la orden hasta el archivo cerrado */
	uint64_t us_io;     /* de eso, leyendo o escribiendo el disco */
	uint32_t bytes;
};

/* ¿Hay un estado en la ranura para la ROM del bus? */
bool states_slot_exists(const struct salsa64_bus *bus, unsigned slot);

/* Guarda la máquina en la ranura, con la miniatura capturada si la hay. */
bool states_save(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size);

/* Carga la ranura sobre la máquina en marcha. *broken: falló a mitad de la
 * RDRAM y la máquina quedó inservible. */
bool states_load(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size,
		 bool *broken);

/* Miniatura de la imagen recién dibujada (después de rsxkit_finish): una
 * muestra del búfer del RSX por píxel de 160 × 120, dentro de `r` (donde
 * quedó la imagen de la N64). La guarda el próximo states_save. */
void states_thumb_capture(const rsxkit_rect *r);
void states_thumb_clear(void);
/* Lo que tardó la última captura (µs; 0 si no hubo). */
uint64_t states_thumb_us(void);

#endif
