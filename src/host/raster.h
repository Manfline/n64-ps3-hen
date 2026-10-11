/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — rasterizador de referencia de la salida de la HLE gráfica
 * (solo host). Ver raster.c.
 */
#ifndef SALSA64_HOST_RASTER_H
#define SALSA64_HOST_RASTER_H

#include <stdbool.h>
#include <stdint.h>

#include "rsp/gfx/gfx.h"

/* Tamaño de las imágenes; para pantallas de alta resolución (la intro de
 * Perfect Dark, 576×474) se puede compilar con -DRASTER_W=640 -DRASTER_H=480. */
#ifndef RASTER_W
#define RASTER_W 320
#endif
#ifndef RASTER_H
#define RASTER_H 240
#endif

/* Dibuja la salida de una tarea en las imágenes de cada color image. */
void raster_task(const struct gfx_job *job, const struct gfx_result *res);
/* Imagen (0xRRGGBB, RASTER_W de paso) que contiene la dirección `addr`, o
 * NULL si ninguna tarea dibujó ahí. */
const uint32_t *raster_image(uint32_t addr, uint32_t *width);

#endif
