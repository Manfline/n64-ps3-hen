/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tareas JPEG de prueba (selftest.c): una RDRAM de mentira con
 * dos tareas (modo 2 y modo 0), tres tablas de cuantización y macrobloques
 * seudoaleatorios, algunos con coeficientes que saturan y desbordan. Dos
 * ejecuciones sobre copias iguales deben dejar las copias iguales: así se
 * compara el SPE con el PPE al arrancar en la PS3. El CRC-32 de la RDRAM
 * después de las dos tareas (JPEG_SELFTEST_CRC) lo verifica también la
 * prueba unitaria del host: host, PPE y SPE dan lo mismo.
 */
#ifndef SALSA64_RSP_JPEG_SELFTEST_H
#define SALSA64_RSP_JPEG_SELFTEST_H

#include <stdint.h>

#define JPEG_SELFTEST_RAM  0x3000u
#define JPEG_SELFTEST_MBS  8u          /* macrobloques de cada tarea */
#define JPEG_SELFTEST_SEED 20261012u
#define JPEG_SELFTEST_CRC  0xF5C74ACFu /* con JPEG_SELFTEST_SEED */

/* Llena `ram` (JPEG_SELFTEST_RAM bytes). */
void jpeg_selftest_build(uint8_t *ram, uint32_t seed);

/* Dirección de la estructura de la tarea `which` (0: modo 2, 1: modo 0),
 * para salsa64_jpeg_run o el trabajo del SPE. */
uint32_t jpeg_selftest_task(uint32_t which);

/* ---- Video de Resident Evil 2 (re2.h) ----
 * Cuatro tareas (RESIZE con difuminado, DECODE con la geometría del juego y dos
 * FILL, una con origen y destino alineados a 16 bytes, como en el juego, y otra
 * sin alinear) sobre una RDRAM de mentira, con los segmentos de datos que trae el
 * juego y direcciones de salida alineadas a 8 y no a 16. El CRC-32 de la RDRAM
 * después de las cuatro lo verifica también la prueba unitaria del host. */
#define RE2_SELFTEST_TASKS 4u
#define RE2_SELFTEST_RAM  0x9000u
#define RE2_SELFTEST_SEED 20261017u
#define RE2_SELFTEST_CRC  0xAE4B7E46u /* con RE2_SELFTEST_SEED */
#define RE2_SELFTEST_DRAM_STACK      0x00001234u  /* dram_stack y su tamaño en la tarea de prueba */
#define RE2_SELFTEST_DRAM_STACK_SIZE 0x04000000u

void re2_selftest_build(uint8_t *ram, uint32_t seed);

/* Dirección del segmento de datos de la tarea `which` (0: RESIZE, 1: DECODE, 2: FILL sin alinear, 3: FILL alineada). */
uint32_t re2_selftest_task(uint32_t which);

#endif
