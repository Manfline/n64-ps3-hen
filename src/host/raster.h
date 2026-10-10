/*
 * Salsa64 — rasterizador de referencia de la salida de la HLE gráfica
 * (solo host). Ver raster.c.
 */
#ifndef SALSA64_HOST_RASTER_H
#define SALSA64_HOST_RASTER_H

#include <stdbool.h>
#include <stdint.h>

#include "rsp/gfx/gfx.h"

#define RASTER_W 320
#define RASTER_H 240

/* Dibuja la salida de una tarea en las imágenes de cada color image. */
void raster_task(const struct gfx_job *job, const struct gfx_result *res);
/* Imagen (0xRRGGBB, RASTER_W de paso) que contiene la dirección `addr`, o
 * NULL si ninguna tarea dibujó ahí. */
const uint32_t *raster_image(uint32_t addr, uint32_t *width);

#endif
