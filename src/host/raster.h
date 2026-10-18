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
/* Dibujos con triángulos dibujados, cuántos son para la pasada de aristas
 * (opacos con AA_EN; la llevan si raster_aa_edges) y sus triángulos. */
void raster_edge_stats(uint32_t *draws, uint32_t *with_edges, uint32_t *tris);
/* Dibujos en color images de 8 bits y texturas I8 leídas de ellas: de su
 * imagen (render a textura) o, si no se pudo, de la RDRAM. */
void raster_8bit_stats(uint32_t *draws, uint32_t *tex_rsx, uint32_t *tex_rdram);
/* Borrados enteros de las z images que el RDP no borra enteras (zauto de rdp.c). */
uint32_t raster_zauto_count(void);
const uint32_t *raster_image(uint32_t addr, uint32_t *width);
/* Qué hay en la ranura `slot` de la tabla de destinos (para la vigilancia
 * de accesos del CPU); false si está vacía. */
/* La imagen de la ranura `slot` (RASTER_W de paso), o NULL si está vacía. */
const uint32_t *raster_slot_image(int slot);
bool raster_target_info(int slot, uint32_t *addr, uint32_t *width, uint32_t *bpp, uint32_t *lh);

/* Ajustes (los pone run.c): trazar a stderr los dibujos que leen un destino
 * (--trace-fb) con el campo en curso; el nivel de efectos de framebuffer
 * como salsa64_rdp_fbfx (0 apagados, 1 render a textura, 2 también copias
 * a la RDRAM). */
extern bool raster_trace_fb;
extern uint32_t raster_field;
extern int raster_fbfx;
/* En las imágenes de 16 bits, el color con la precisión de la N64 (5 bits
 * por canal, truncado como la copia a la RDRAM); false: a 8 bits, como lo
 * muestra el RSX (salsa64-run --ares, para comparar con el tramado del RDP). */
extern bool raster_q5;
/* Como la PS3 (salsa64_rdp_aa_edges): en los dibujos opacos con AA_EN, las
 * aristas como líneas antes de los triángulos (experimental, apagadas por
 * omisión; SALSA64_AA_EDGES=1 las pone). */
extern bool raster_aa_edges;
extern float raster_edge_width;  /* ancho de esas líneas, en píxeles */
extern float raster_edge_factor, raster_edge_units;  /* su desplazamiento hacia atrás */
/* Cada campo con imagen: el origen y ancho del VI (como salsa64_rdp_vi). */
struct salsa64_vi_frame;
void raster_vi(const struct salsa64_vi_frame *f);
/* Vuelve al estado del arranque: tabla de destinos, imágenes y cuentas
 * (pruebas unitarias; salsa64-run no lo necesita). */
void raster_reset(void);
/* Totales: copias a la RDRAM, destinos cebados, texturas desde un destino
 * (de su imagen) y desde la RDRAM, y las que la HLE dejó para leer de un
 * destino (GFX_TEX_TARGET) y hubo que decodificar aquí. */
void raster_fb_stats(uint32_t *readbacks, uint32_t *primes, uint32_t *tex_rsx, uint32_t *tex_rdram,
		     uint32_t *tex_decoded);

/* Todo el estado (destinos, imágenes, profundidades, cuentas), para dibujar
 * un cuadro inventado y volver (interp.c). */
#include <stddef.h>
size_t raster_state_size(void);
void raster_state_save(void *buf);
void raster_state_restore(const void *buf);

#endif
