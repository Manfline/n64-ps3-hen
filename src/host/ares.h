/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — comparar la HLE gráfica con un volcado de la RDRAM de ares
 * (salsa64-run --ares). Ver ares.c.
 */
#ifndef SALSA64_HOST_ARES_H
#define SALSA64_HOST_ARES_H

#include <stdint.h>

/* argv[0] es "--ares"; devuelve el código de salida de salsa64-run. */
int ares_main(int argc, char **argv);
/* Un volcado de la HLE (gfx_dump_header) del orden del host al de la PS3,
 * en su lugar: el que leen la PS3, RPCS3 (--replay) y --raster-dump. */
void ares_dump_to_be(uint8_t *dump, uint32_t size);

#endif
