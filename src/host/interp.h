/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — prototipo de interpolación de cuadros en el host. Ver interp.c
 * y docs/interpolacion.md.
 */
#ifndef SALSA64_HOST_INTERP_H
#define SALSA64_HOST_INTERP_H

#include <stdbool.h>
#include <stdint.h>

struct gfx_state;
struct gfx_job;
struct gfx_result;

struct interp_opts {
	const char *dir;     /* carpeta de salida (NULL: apagado) */
	unsigned k;          /* campos por cuadro del juego: intermedios en j/k, j = 1…k−1 */
	bool decimate;       /* tomar 1 de cada k tareas como cuadro del juego y comparar
	                        los intermedios con las tareas de en medio (la verdad) */
	float max_disp;      /* píxeles (de la N64) por cuadro del juego: más es un corte */
	uint32_t from_field; /* desde este campo del VI */
	unsigned block;      /* lado del bloque de la búsqueda por imagen (8) */
	unsigned range;      /* rango de la búsqueda completa en el nivel más grueso (±8) */
};

void interp_init(const struct interp_opts *o);
/* Hilo del CPU, antes de cada tarea gráfica (acquire): prepara el
 * emparejamiento de la HLE. */
void interp_task_begin(struct gfx_state *g, uint32_t field);
/* Alrededor del rasterizador en gfx_done: la tarea ya dibujada es un
 * cuadro del juego; si toca, se inventan los intermedios. */
void interp_before_raster(void);
void interp_after_raster(const struct gfx_job *job, const struct gfx_result *res, uint32_t field);
/* Resumen al terminar (stderr y DIR/resumen.txt). */
void interp_report(void);

#endif
