/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — armado de un volcado de la salida de la HLE gráfica
 * (gfx.h: gfx_dump_header) en memoria. Lo usan salsa64_rdp_dump (el archivo
 * gfxdump.bin) y la caja negra (blackbox.h), y salsa64-run --raster-dump lo
 * dibuja en el host.
 *
 * El volcado es autosuficiente: las texturas de la caché que usan los
 * dibujos se copian detrás del arena de la tarea (una vez cada una) y los
 * dibujos pasan a apuntar ahí; las que se leían de un destino
 * (GFX_TEX_TARGET) se decodifican de la RDRAM de ese momento, como las
 * habría decodificado el consumidor. Portable (compila con gcc nativo, con
 * pruebas en tests/unit/test_gfxdump.c).
 */
#ifndef SALSA64_RDP_GFXDUMP_H
#define SALSA64_RDP_GFXDUMP_H

#include <stdint.h>

#include "rsp/gfx/gfx.h"

/* Lo que sale de la HLE en una tarea y lo que necesita el volcado. */
struct gfx_dump_src {
	const uint8_t *cmd;
	uint32_t cmd_bytes;
	const uint8_t *vtx;
	uint32_t vtx_bytes;
	const uint8_t *arena;   /* arena de texturas de la tarea */
	uint32_t arena_bytes;   /* tex_bytes de gfx_result */
	const uint8_t *cache;   /* región de la caché de texturas (NULL: sin caché) */
	uint32_t cache_bytes;
	const uint8_t *rdram;   /* para decodificar las texturas de GFX_TEX_TARGET (puede ser NULL: ceros) */
	uint32_t rdram_size;
};

/* Cada dibujo usa a lo más 2 texturas distintas. */
#define GFX_DUMP_MAX_TEX(cmd_bytes) (2u * ((cmd_bytes) / (uint32_t)sizeof(struct gfx_draw)) + 1u)

/* Espacio de trabajo: GFX_DUMP_MAX_TEX(cmd_bytes) entradas de cada arreglo
 * (con cmd_bytes ≤ el mayor búfer de comandos de la HLE). Lo pone quien
 * llama; nada se asigna aquí. */
struct gfx_dump_work {
	uint32_t *src;                /* offset en la caché de cada textura distinta */
	uint32_t *len;                /* sus bytes (pitch × alto) */
	const struct gfx_tex **tgt;   /* la que se decodifica de la RDRAM (GFX_TEX_TARGET), o NULL */
	uint32_t cap;                 /* entradas */
};

/* Bytes que ocupará el volcado completo (cabecera incluida), o 0 si no se
 * puede armar (comandos mal formados, una textura fuera de la caché, el
 * espacio de trabajo no alcanza). */
uint32_t gfx_dump_measure(const struct gfx_dump_src *s, const struct gfx_dump_work *w);

/* Arma el volcado en `out` (capacidad `cap`, ≥ gfx_dump_measure) y devuelve
 * sus bytes, o 0 si no cabe o no se puede. */
uint32_t gfx_dump_build(const struct gfx_dump_src *s, const struct gfx_dump_work *w, uint8_t *out, uint32_t cap);

#endif
