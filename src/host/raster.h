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
/* La RDRAM [pa, pa + len) la escribió un DMA; `task` es la tarea gráfica en
 * vuelo (o la última terminada): sus copias y las anteriores no la pisan. */
void raster_rdram_written(uint32_t pa, uint32_t len, uint32_t task);
/* Imagen (0xRRGGBB, RASTER_W de paso) que contiene la dirección `addr`, o
 * NULL si ninguna tarea dibujó ahí. */
const uint32_t *raster_image(uint32_t addr, uint32_t *width);
/* Qué hay en la ranura `slot` de la tabla de destinos (para la vigilancia
 * de accesos del CPU); false si está vacía. */
bool raster_target_info(int slot, uint32_t *addr, uint32_t *width, uint32_t *bpp, uint32_t *lh);

/* Ajustes (los pone run.c): trazar a stderr los dibujos que leen un destino
 * (--trace-fb) con el campo en curso; el nivel de efectos de framebuffer
 * como salsa64_rdp_fbfx (0 apagados, 1 render a textura, 2 también copias
 * a la RDRAM). */
extern bool raster_trace_fb;
extern uint32_t raster_field;
extern int raster_fbfx;
/* Cada campo con imagen: el origen y ancho del VI (como salsa64_rdp_vi). */
struct salsa64_vi_frame;
void raster_vi(const struct salsa64_vi_frame *f);
/* Vuelve al estado del arranque: tabla de destinos, imágenes y cuentas
 * (pruebas unitarias; salsa64-run no lo necesita). */
void raster_reset(void);
/* Totales: copias a la RDRAM, destinos cebados, texturas desde un destino
 * (de su imagen) y desde la RDRAM. */
void raster_fb_stats(uint32_t *readbacks, uint32_t *primes, uint32_t *tex_rsx, uint32_t *tex_rdram);

#endif
