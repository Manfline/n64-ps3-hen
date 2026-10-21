/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tareas gráficas de prueba (selftest.c): una RDRAM de mentira con
 * una display list en la dirección 0 que pasa por lo común de la HLE
 * (viewport, matrices, vértices, triángulos, cargas a la TMEM con LOADBLOCK
 * y LOADTLUT, texturas RGBA16 y CI4, combiners de uno y dos ciclos, dos
 * texturas, rectángulos, relleno) con la codificación de la familia del
 * microcódigo pedido; en la segunda generación, un tramo S2DEX2 con un
 * G_BG_COPY, y en las tareas S2DEX sueltas 1.x y 2.x, su propia copia de
 * fondo y terminación de lista. F3DZEX2 pasa por G_BRANCH_W y F3DEX2 cambia
 * también a F3DFLX y sube su tabla de alfa. Nada viene de un juego ni de un
 * microcódigo: vértices, texturas y textos son inventados (el texto
 * "RSP Gfx ucode S2DEX…" es lo que reconoce gfx_ucode_kind).
 *
 * Al cargar una imagen del SPE 0 (src/platform/ps3/gfx_spu.c) cada
 * microcódigo de la imagen se ejecuta en el SPE y en el PPE sobre copias
 * iguales: la salida debe ser la misma (los flotantes, casi: el SPE redondea
 * hacia cero; gfx_selftest_compare_*). Va en el host y en el PPE, no en el
 * SPE.
 */
#ifndef SALSA64_RSP_GFX_SELFTEST_H
#define SALSA64_RSP_GFX_SELFTEST_H

#include <stdint.h>

#define GFX_SELFTEST_RAM   0x20000u  /* bytes de la RDRAM de mentira */
#define GFX_SELFTEST_UCODE 0x7000u   /* job->ucode de la tarea (solo la dirección) */

/* Llena `ram` (GFX_SELFTEST_RAM bytes) con la lista para el microcódigo
 * `gbi` (enum salsa64_gbi) y devuelve su dirección (job->dl). `seed` cambia
 * los texels y los colores. */
uint32_t gfx_selftest_build(uint8_t *ram, uint32_t gbi, uint32_t seed);

/* Compara dos salidas de `n` bytes con el formato de gfx.h: los vértices
 * (gfx_vertex) o los registros de comandos. Los flotantes (coordenadas, s y
 * t, xf de las texturas, términos k, lo de GFX_OP_VGROUP) pueden diferir en
 * a lo más `ulps` pasos (o ser los dos casi cero); lo demás, exacto.
 * Devuelve el número de palabras distintas y en *first el byte de la
 * primera. */
uint32_t gfx_selftest_compare_vtx(const uint8_t *a, const uint8_t *b, uint32_t n, uint32_t ulps, uint32_t *first);
uint32_t gfx_selftest_compare_cmd(const uint8_t *a, const uint8_t *b, uint32_t n, uint32_t ulps, uint32_t *first);

#endif
